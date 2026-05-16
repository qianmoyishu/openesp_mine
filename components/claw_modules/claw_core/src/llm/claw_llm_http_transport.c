/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "llm/claw_llm_http_transport.h"

#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_tls.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if CONFIG_ESP_HTTP_CLIENT_ENABLE_CUSTOM_TRANSPORT
#include "esp_transport.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/error.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#endif

static const char *TAG = "llm_http";

#define CLAW_LLM_HTTP_RB_INITIAL_CAP 4096

static volatile bool *s_abort_flag = NULL;
static TaskHandle_t   s_abort_owner = NULL;

void claw_llm_http_arm_abort(volatile bool *flag)
{
    TaskHandle_t self = xTaskGetCurrentTaskHandle();
    assert(s_abort_flag == NULL || s_abort_owner == self);
    s_abort_flag = flag;
    s_abort_owner = self;
}

void claw_llm_http_disarm_abort(void)
{
    if (s_abort_owner == xTaskGetCurrentTaskHandle()) {
        s_abort_flag = NULL;
        s_abort_owner = NULL;
    }
}

static inline bool abort_requested(void)
{
    return s_abort_flag &&
           s_abort_owner == xTaskGetCurrentTaskHandle() &&
           *s_abort_flag;
}

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} response_buffer_t;

static char *dup_printf(const char *fmt, ...)
{
    va_list args;
    va_list copy;
    int needed;
    char *buf;

    va_start(args, fmt);
    va_copy(copy, args);
    needed = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (needed < 0) {
        va_end(args);
        return NULL;
    }

    buf = calloc(1, (size_t)needed + 1);
    if (!buf) {
        va_end(args);
        return NULL;
    }

    vsnprintf(buf, (size_t)needed + 1, fmt, args);
    va_end(args);
    return buf;
}

static esp_err_t response_buffer_init(response_buffer_t *buffer)
{
    if (!buffer) {
        return ESP_ERR_INVALID_ARG;
    }

    buffer->data = calloc(1, CLAW_LLM_HTTP_RB_INITIAL_CAP);
    if (!buffer->data) {
        return ESP_ERR_NO_MEM;
    }

    buffer->cap = CLAW_LLM_HTTP_RB_INITIAL_CAP;
    buffer->len = 0;
    return ESP_OK;
}

static esp_err_t response_buffer_append(response_buffer_t *buffer, const char *data, size_t len)
{
    char *grown;
    size_t cap;

    if (!buffer || !data) {
        return ESP_ERR_INVALID_ARG;
    }

    cap = buffer->cap;
    while (buffer->len + len + 1 > cap) {
        cap *= 2;
    }

    if (cap != buffer->cap) {
        grown = realloc(buffer->data, cap);
        if (!grown) {
            return ESP_ERR_NO_MEM;
        }
        buffer->data = grown;
        buffer->cap = cap;
    }

    memcpy(buffer->data + buffer->len, data, len);
    buffer->len += len;
    buffer->data[buffer->len] = '\0';
    return ESP_OK;
}

static void response_buffer_free(response_buffer_t *buffer)
{
    if (!buffer) {
        return;
    }

    free(buffer->data);
    memset(buffer, 0, sizeof(*buffer));
}

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    response_buffer_t *buffer = (response_buffer_t *)evt->user_data;

    if (abort_requested()) {
        return ESP_FAIL;
    }

    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        return response_buffer_append(buffer, (const char *)evt->data, evt->data_len);
    }

    return ESP_OK;
}

static char *build_auth_header_value(const char *auth_type, const char *api_key)
{
    const char *kind = auth_type ? auth_type : "bearer";

    if (!api_key || !api_key[0]) {
        return NULL;
    }
    if (strcmp(kind, "none") == 0) {
        return NULL;
    }
    if (strcmp(kind, "api-key") == 0) {
        return strdup(api_key);
    }

    return dup_printf("Bearer %s", api_key);
}

static const char *auth_header_name(const char *auth_type)
{
    if (auth_type && strcmp(auth_type, "api-key") == 0) {
        return "X-API-Key";
    }
    return "Authorization";
}

static char *parse_error_message_body(const char *body, int status)
{
    cJSON *root;
    cJSON *error;
    cJSON *message;
    char *fallback;

    if (!body || !body[0]) {
        return dup_printf("HTTP %d", status);
    }

    root = cJSON_Parse(body);
    if (!root) {
        return dup_printf("HTTP %d: %.160s", status, body);
    }

    error = cJSON_GetObjectItem(root, "error");
    if (error && cJSON_IsObject(error)) {
        message = cJSON_GetObjectItem(error, "message");
        if (message && cJSON_IsString(message) && message->valuestring[0]) {
            fallback = dup_printf("HTTP %d: %s", status, message->valuestring);
            cJSON_Delete(root);
            return fallback;
        }
    }

    message = cJSON_GetObjectItem(root, "message");
    if (message && cJSON_IsString(message) && message->valuestring[0]) {
        fallback = dup_printf("HTTP %d: %s", status, message->valuestring);
        cJSON_Delete(root);
        return fallback;
    }

    cJSON_Delete(root);
    return dup_printf("HTTP %d: %.160s", status, body);
}

static bool url_is_https(const char *url)
{
    return url && strncmp(url, "https://", 8) == 0;
}

#if CONFIG_ESP_HTTP_CLIENT_ENABLE_CUSTOM_TRANSPORT
typedef struct {
    char *proxy_host;
    int proxy_port;
    int sockfd;
    esp_tls_last_error_t tcp_error;
    mbedtls_net_context net;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config ssl_conf;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    bool tls_ready;
} llm_proxy_transport_ctx_t;

static esp_err_t socket_write_all(int sockfd, const char *data, size_t len)
{
    size_t written = 0;

    while (written < len) {
        int ret = send(sockfd, data + written, len - written, 0);

        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            return ESP_FAIL;
        }
        if (ret == 0) {
            return ESP_FAIL;
        }
        written += (size_t)ret;
    }
    return ESP_OK;
}

static esp_err_t proxy_read_connect_response(int sockfd, int timeout_ms, char *buf, size_t buf_size)
{
    size_t len = 0;
    char *line_end = NULL;

    if (!buf || buf_size < 5) {
        return ESP_ERR_INVALID_ARG;
    }
    buf[0] = '\0';
    while (len + 1 < buf_size) {
        fd_set readset;
        struct timeval timeout = {
            .tv_sec = timeout_ms / 1000,
            .tv_usec = (timeout_ms % 1000) * 1000,
        };
        int ret;

        FD_ZERO(&readset);
        FD_SET(sockfd, &readset);
        ret = select(sockfd + 1, &readset, NULL, NULL, timeout_ms >= 0 ? &timeout : NULL);
        if (ret <= 0) {
            return ret == 0 ? ESP_ERR_TIMEOUT : ESP_FAIL;
        }

        ret = recv(sockfd, buf + len, 1, 0);
        if (ret <= 0) {
            return ESP_FAIL;
        }
        len += (size_t)ret;
        buf[len] = '\0';
        if (strstr(buf, "\r\n\r\n")) {
            break;
        }
    }

    if (!strstr(buf, "\r\n\r\n")) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (strncmp(buf, "HTTP/1.0 200", 12) != 0 && strncmp(buf, "HTTP/1.1 200", 12) != 0) {
        ESP_LOGE(TAG, "Proxy CONNECT failed: %.96s", buf);
        return ESP_FAIL;
    }
    line_end = strstr(buf, "\r\n");
    if (line_end) {
        *line_end = '\0';
        ESP_LOGI(TAG, "Proxy CONNECT response: %s", buf);
        *line_end = '\r';
    }
    if (strstr(buf, "\r\nContent-Length:") || strstr(buf, "\r\nContent-Type:")) {
        ESP_LOGW(TAG, "Proxy CONNECT 200 response has content headers; check that the configured port is an HTTP proxy port");
    }
    return ESP_OK;
}

static esp_err_t proxy_send_connect(int sockfd, const char *host, int port, int timeout_ms)
{
    char request[256];
    char response[512];
    int len;

    len = snprintf(request,
                   sizeof(request),
                   "CONNECT %s:%d HTTP/1.1\r\n"
                   "Host: %s:%d\r\n"
                   "Proxy-Connection: Keep-Alive\r\n"
                   "\r\n",
                   host,
                   port,
                   host,
                   port);
    if (len < 0 || len >= (int)sizeof(request)) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (socket_write_all(sockfd, request, (size_t)len) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write proxy CONNECT request");
        return ESP_FAIL;
    }
    return proxy_read_connect_response(sockfd, timeout_ms, response, sizeof(response));
}

static void proxy_transport_tls_free(llm_proxy_transport_ctx_t *ctx)
{
    if (!ctx) {
        return;
    }
    if (ctx->tls_ready) {
        mbedtls_ssl_close_notify(&ctx->ssl);
    }
    mbedtls_ssl_free(&ctx->ssl);
    mbedtls_ssl_config_free(&ctx->ssl_conf);
    mbedtls_ctr_drbg_free(&ctx->ctr_drbg);
    mbedtls_entropy_free(&ctx->entropy);
    ctx->tls_ready = false;
}

static esp_err_t proxy_start_tls(llm_proxy_transport_ctx_t *ctx, const char *host)
{
    const char *pers = "llm_proxy";
    int ret;

    if (!ctx || !host) {
        return ESP_ERR_INVALID_ARG;
    }

    mbedtls_net_init(&ctx->net);
    mbedtls_ssl_init(&ctx->ssl);
    mbedtls_ssl_config_init(&ctx->ssl_conf);
    mbedtls_entropy_init(&ctx->entropy);
    mbedtls_ctr_drbg_init(&ctx->ctr_drbg);

    ret = mbedtls_ctr_drbg_seed(&ctx->ctr_drbg,
                                mbedtls_entropy_func,
                                &ctx->entropy,
                                (const unsigned char *)pers,
                                strlen(pers));
    if (ret != 0) {
        ESP_LOGE(TAG, "mbedtls_ctr_drbg_seed failed: -0x%x", -ret);
        return ESP_FAIL;
    }

    ret = mbedtls_ssl_config_defaults(&ctx->ssl_conf,
                                      MBEDTLS_SSL_IS_CLIENT,
                                      MBEDTLS_SSL_TRANSPORT_STREAM,
                                      MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) {
        ESP_LOGE(TAG, "mbedtls_ssl_config_defaults failed: -0x%x", -ret);
        return ESP_FAIL;
    }

    mbedtls_ssl_conf_authmode(&ctx->ssl_conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_rng(&ctx->ssl_conf, mbedtls_ctr_drbg_random, &ctx->ctr_drbg);
    if (esp_crt_bundle_attach(&ctx->ssl_conf) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to attach certificate bundle for proxy TLS");
        return ESP_FAIL;
    }

    ret = mbedtls_ssl_setup(&ctx->ssl, &ctx->ssl_conf);
    if (ret != 0) {
        ESP_LOGE(TAG, "mbedtls_ssl_setup failed: -0x%x", -ret);
        return ESP_FAIL;
    }
    ret = mbedtls_ssl_set_hostname(&ctx->ssl, host);
    if (ret != 0) {
        ESP_LOGE(TAG, "mbedtls_ssl_set_hostname(%s) failed: -0x%x", host, -ret);
        return ESP_FAIL;
    }

    ctx->net.fd = ctx->sockfd;
    mbedtls_ssl_set_bio(&ctx->ssl, &ctx->net, mbedtls_net_send, mbedtls_net_recv, NULL);

    do {
        ret = mbedtls_ssl_handshake(&ctx->ssl);
    } while (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE);

    if (ret != 0) {
        char err_buf[96];

        mbedtls_strerror(ret, err_buf, sizeof(err_buf));
        ESP_LOGE(TAG, "TLS handshake via proxy failed: -0x%x (%s)", -ret, err_buf);
        return ESP_FAIL;
    }

    if (mbedtls_ssl_get_verify_result(&ctx->ssl) != 0) {
        ESP_LOGE(TAG, "TLS certificate verification via proxy failed");
        return ESP_FAIL;
    }

    ctx->tls_ready = true;
    return ESP_OK;
}

static int proxy_transport_connect(esp_transport_handle_t t, const char *host, int port, int timeout_ms)
{
    llm_proxy_transport_ctx_t *ctx = (llm_proxy_transport_ctx_t *)esp_transport_get_context_data(t);
    esp_tls_cfg_t tcp_cfg = {
        .timeout_ms = timeout_ms,
        .is_plain_tcp = true,
        .addr_family = ESP_TLS_AF_INET,
    };
    esp_tls_error_handle_t error_handle = esp_transport_get_error_handle(t);
    esp_err_t err;

    if (!ctx || !ctx->proxy_host || !ctx->proxy_host[0] || ctx->proxy_port <= 0 || !host || !host[0]) {
        return -1;
    }
    if (!error_handle) {
        memset(&ctx->tcp_error, 0, sizeof(ctx->tcp_error));
        error_handle = &ctx->tcp_error;
    }

    ESP_LOGI(TAG, "Connecting to %s:%d via HTTP proxy %s:%d",
             host, port, ctx->proxy_host, ctx->proxy_port);

    ctx->sockfd = -1;
    err = esp_tls_plain_tcp_connect(ctx->proxy_host,
                                    strlen(ctx->proxy_host),
                                    ctx->proxy_port,
                                    &tcp_cfg,
                                    error_handle,
                                    &ctx->sockfd);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Proxy TCP connect failed: %s", esp_err_to_name(err));
        return -1;
    }

    err = proxy_send_connect(ctx->sockfd, host, port, timeout_ms);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Proxy CONNECT %s:%d failed: %s", host, port, esp_err_to_name(err));
        close(ctx->sockfd);
        ctx->sockfd = -1;
        return -1;
    }

    err = proxy_start_tls(ctx, host);
    if (err != ESP_OK) {
        proxy_transport_tls_free(ctx);
        close(ctx->sockfd);
        ctx->sockfd = -1;
        return -1;
    }

    return 0;
}

static int proxy_transport_read(esp_transport_handle_t t, char *buffer, int len, int timeout_ms)
{
    llm_proxy_transport_ctx_t *ctx = (llm_proxy_transport_ctx_t *)esp_transport_get_context_data(t);
    fd_set readset;
    struct timeval timeout = {
        .tv_sec = timeout_ms / 1000,
        .tv_usec = (timeout_ms % 1000) * 1000,
    };
    int ret;

    if (!ctx || !ctx->tls_ready || !buffer || len <= 0) {
        return ERR_TCP_TRANSPORT_CONNECTION_FAILED;
    }
    if (!mbedtls_ssl_check_pending(&ctx->ssl)) {
        FD_ZERO(&readset);
        FD_SET(ctx->sockfd, &readset);
        ret = select(ctx->sockfd + 1, &readset, NULL, NULL, timeout_ms >= 0 ? &timeout : NULL);
        if (ret < 0) {
            return ERR_TCP_TRANSPORT_CONNECTION_FAILED;
        }
        if (ret == 0) {
            return ERR_TCP_TRANSPORT_CONNECTION_TIMEOUT;
        }
    }

    ret = mbedtls_ssl_read(&ctx->ssl, (unsigned char *)buffer, (size_t)len);
    if (ret > 0) {
        return ret;
    }
    if (ret == 0) {
        return ERR_TCP_TRANSPORT_CONNECTION_CLOSED_BY_FIN;
    }
    if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
        return ERR_TCP_TRANSPORT_CONNECTION_TIMEOUT;
    }
    ESP_LOGE(TAG, "Proxy TLS read failed: -0x%x", -ret);
    return ERR_TCP_TRANSPORT_CONNECTION_FAILED;
}

static int proxy_transport_write(esp_transport_handle_t t, const char *buffer, int len, int timeout_ms)
{
    llm_proxy_transport_ctx_t *ctx = (llm_proxy_transport_ctx_t *)esp_transport_get_context_data(t);
    fd_set writeset;
    struct timeval timeout = {
        .tv_sec = timeout_ms / 1000,
        .tv_usec = (timeout_ms % 1000) * 1000,
    };
    int ret;

    if (!ctx || !ctx->tls_ready || !buffer || len <= 0) {
        return -1;
    }

    FD_ZERO(&writeset);
    FD_SET(ctx->sockfd, &writeset);
    ret = select(ctx->sockfd + 1, NULL, &writeset, NULL, timeout_ms >= 0 ? &timeout : NULL);
    if (ret <= 0) {
        return ret;
    }

    ret = mbedtls_ssl_write(&ctx->ssl, (const unsigned char *)buffer, (size_t)len);
    if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
        return 0;
    }
    if (ret < 0) {
        ESP_LOGE(TAG, "Proxy TLS write failed: -0x%x", -ret);
    }
    return ret;
}

static int proxy_transport_poll_read(esp_transport_handle_t t, int timeout_ms)
{
    llm_proxy_transport_ctx_t *ctx = (llm_proxy_transport_ctx_t *)esp_transport_get_context_data(t);
    fd_set readset;
    struct timeval timeout = {
        .tv_sec = timeout_ms / 1000,
        .tv_usec = (timeout_ms % 1000) * 1000,
    };

    if (!ctx || ctx->sockfd < 0) {
        return -1;
    }
    if (ctx->tls_ready && mbedtls_ssl_check_pending(&ctx->ssl)) {
        return 1;
    }
    FD_ZERO(&readset);
    FD_SET(ctx->sockfd, &readset);
    return select(ctx->sockfd + 1, &readset, NULL, NULL, timeout_ms >= 0 ? &timeout : NULL);
}

static int proxy_transport_poll_write(esp_transport_handle_t t, int timeout_ms)
{
    llm_proxy_transport_ctx_t *ctx = (llm_proxy_transport_ctx_t *)esp_transport_get_context_data(t);
    fd_set writeset;
    struct timeval timeout = {
        .tv_sec = timeout_ms / 1000,
        .tv_usec = (timeout_ms % 1000) * 1000,
    };

    if (!ctx || ctx->sockfd < 0) {
        return -1;
    }
    FD_ZERO(&writeset);
    FD_SET(ctx->sockfd, &writeset);
    return select(ctx->sockfd + 1, NULL, &writeset, NULL, timeout_ms >= 0 ? &timeout : NULL);
}

static int proxy_transport_close(esp_transport_handle_t t)
{
    llm_proxy_transport_ctx_t *ctx = (llm_proxy_transport_ctx_t *)esp_transport_get_context_data(t);

    if (!ctx) {
        return -1;
    }
    proxy_transport_tls_free(ctx);
    if (ctx->sockfd >= 0) {
        close(ctx->sockfd);
        ctx->sockfd = -1;
    }
    return 0;
}

static int proxy_transport_destroy(esp_transport_handle_t t)
{
    llm_proxy_transport_ctx_t *ctx = (llm_proxy_transport_ctx_t *)esp_transport_get_context_data(t);

    if (ctx) {
        proxy_transport_close(t);
        free(ctx->proxy_host);
        free(ctx);
        esp_transport_set_context_data(t, NULL);
    }
    return 0;
}

static esp_transport_handle_t proxy_transport_init(const char *proxy_host, uint16_t proxy_port)
{
    esp_transport_handle_t transport = NULL;
    llm_proxy_transport_ctx_t *ctx = NULL;

    if (!proxy_host || !proxy_host[0] || proxy_port == 0) {
        return NULL;
    }

    transport = esp_transport_init();
    ctx = calloc(1, sizeof(*ctx));
    if (!transport || !ctx) {
        free(ctx);
        if (transport) {
            esp_transport_destroy(transport);
        }
        return NULL;
    }

    ctx->proxy_host = strdup(proxy_host);
    ctx->proxy_port = proxy_port;
    ctx->sockfd = -1;
    if (!ctx->proxy_host) {
        free(ctx);
        esp_transport_destroy(transport);
        return NULL;
    }

    esp_transport_set_context_data(transport, ctx);
    esp_transport_set_func(transport,
                           proxy_transport_connect,
                           proxy_transport_read,
                           proxy_transport_write,
                           proxy_transport_close,
                           proxy_transport_poll_read,
                           proxy_transport_poll_write,
                           proxy_transport_destroy);
    return transport;
}
#endif

esp_err_t claw_llm_http_post_json(const claw_llm_http_json_request_t *request,
                                  claw_llm_http_response_t *out_response,
                                  char **out_error_message)
{
    response_buffer_t buffer = {0};
    esp_http_client_config_t config = {0};
    esp_http_client_handle_t client = NULL;
#if CONFIG_ESP_HTTP_CLIENT_ENABLE_CUSTOM_TRANSPORT
    esp_transport_handle_t proxy_transport = NULL;
#endif
    char *auth_header_value = NULL;
    int status_code = 0;
    esp_err_t err;

    if (out_response) {
        memset(out_response, 0, sizeof(*out_response));
    }
    if (out_error_message) {
        *out_error_message = NULL;
    }
    if (!request || !request->url || !request->body || !out_response || !out_error_message) {
        return ESP_ERR_INVALID_ARG;
    }

    err = response_buffer_init(&buffer);
    if (err != ESP_OK) {
        *out_error_message = dup_printf("Out of memory allocating HTTP buffer");
        ESP_LOGE(TAG, "OOM allocating HTTP response buffer");
        return err;
    }

    config.url = request->url;
    config.event_handler = http_event_handler;
    config.user_data = &buffer;
    config.timeout_ms = request->timeout_ms;
    config.buffer_size = 4096;
    config.buffer_size_tx = 4096;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.addr_type = HTTP_ADDR_TYPE_INET;

#if CONFIG_ESP_HTTP_CLIENT_ENABLE_CUSTOM_TRANSPORT
    if (request->proxy_enabled && request->proxy_host && request->proxy_host[0] &&
            request->proxy_port > 0 && url_is_https(request->url)) {
        proxy_transport = proxy_transport_init(request->proxy_host, request->proxy_port);
        if (!proxy_transport) {
            *out_error_message = dup_printf("Failed to create LLM proxy transport");
            err = ESP_ERR_NO_MEM;
            goto cleanup;
        }
        config.transport = proxy_transport;
    }
#else
    if (request->proxy_enabled && request->proxy_host && request->proxy_host[0] &&
            request->proxy_port > 0 && url_is_https(request->url)) {
        *out_error_message = dup_printf("LLM proxy is enabled, but ESP_HTTP_CLIENT custom transport is disabled");
        err = ESP_ERR_NOT_SUPPORTED;
        goto cleanup;
    }
#endif

    client = esp_http_client_init(&config);
    if (!client) {
        *out_error_message = dup_printf("Failed to create HTTP client");
        ESP_LOGE(TAG, "Failed to create HTTP client for %s", request->url);
        err = ESP_FAIL;
        goto cleanup;
    }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    auth_header_value = build_auth_header_value(request->auth_type, request->api_key);
    if (auth_header_value) {
        esp_http_client_set_header(client, auth_header_name(request->auth_type), auth_header_value);
    }
    if (request->headers && request->header_count > 0) {
        size_t i;

        for (i = 0; i < request->header_count; i++) {
            const claw_llm_http_header_t *header = &request->headers[i];

            if (!header->name || !header->name[0] || !header->value) {
                continue;
            }
            esp_http_client_set_header(client, header->name, header->value);
        }
    }
    esp_http_client_set_post_field(client, request->body, (int)strlen(request->body));

    ESP_LOGD(TAG, "POST %s", request->url);
    err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        if (abort_requested()) {
            *out_error_message = dup_printf("HTTP request aborted by caller");
            ESP_LOGW(TAG, "HTTP perform aborted: %s", esp_err_to_name(err));
            err = ESP_ERR_INVALID_STATE;
        } else {
            *out_error_message = dup_printf("HTTP request failed: %s (%s)",
                                            esp_err_to_name(err),
                                            request->url);
            ESP_LOGE(TAG, "HTTP perform failed for %s: %s",
                     request->url,
                     esp_err_to_name(err));
        }
        goto cleanup;
    }

    status_code = esp_http_client_get_status_code(client);
    ESP_LOGD(TAG, "HTTP status=%d", status_code);
    if (status_code != 200) {
        err = ESP_FAIL;
        *out_error_message = parse_error_message_body(buffer.data, status_code);
        ESP_LOGE(TAG, "LLM error: %s", *out_error_message ? *out_error_message : "(null)");
        goto cleanup;
    }

    out_response->status_code = status_code;
    out_response->body = buffer.data;
    buffer.data = NULL;
    err = ESP_OK;

cleanup:
    free(auth_header_value);
    if (client) {
        esp_http_client_cleanup(client);
    }
#if CONFIG_ESP_HTTP_CLIENT_ENABLE_CUSTOM_TRANSPORT
    if (proxy_transport) {
        esp_transport_destroy(proxy_transport);
    }
#endif
    response_buffer_free(&buffer);
    return err;
}

void claw_llm_http_response_free(claw_llm_http_response_t *response)
{
    if (!response) {
        return;
    }

    free(response->body);
    memset(response, 0, sizeof(*response));
}
