/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_xfyun_speech.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "claw_cap.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "mbedtls/md.h"

#define CAP_XFYUN_STR_LEN              320
#define CAP_XFYUN_PATH_LEN             192
#define CAP_XFYUN_URL_LEN              1400
#define CAP_XFYUN_AUTH_LEN             512
#define CAP_XFYUN_DATE_LEN             64
#define CAP_XFYUN_SIG_LEN              64
#define CAP_XFYUN_TEXT_MAX             4096
#define CAP_XFYUN_ERR_LEN              256
#define CAP_XFYUN_WS_STACK             6144
#define CAP_XFYUN_WS_BUF               4096
#define CAP_XFYUN_FRAME_BYTES          1280
#define CAP_XFYUN_CONNECT_TIMEOUT_MS   15000
#define CAP_XFYUN_RESULT_TIMEOUT_MS    45000
#define CAP_XFYUN_TTS_OUTPUT_DEFAULT   "/xfyun_tts.pcm"
#define CAP_XFYUN_WAV_HEADER_BYTES     44

#define CAP_XFYUN_EVT_CONNECTED BIT0
#define CAP_XFYUN_EVT_DONE      BIT1
#define CAP_XFYUN_EVT_ERROR     BIT2

typedef enum {
    CAP_XFYUN_OP_ASR = 0,
    CAP_XFYUN_OP_TTS,
} cap_xfyun_op_t;

typedef struct {
    char app_id[64];
    char api_key[CAP_XFYUN_STR_LEN];
    char api_secret[CAP_XFYUN_STR_LEN];
    char base_dir[CAP_XFYUN_PATH_LEN];
} cap_xfyun_state_t;

typedef struct {
    EventGroupHandle_t events;
    cap_xfyun_op_t op;
    char text[CAP_XFYUN_TEXT_MAX];
    size_t text_len;
    FILE *out_file;
    char error[CAP_XFYUN_ERR_LEN];
    char *frame_buf;
    size_t frame_len;
    size_t frame_expected;
} cap_xfyun_ws_ctx_t;

typedef struct {
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t bits_per_sample;
    uint32_t data_offset;
    uint32_t data_size;
} cap_xfyun_wav_info_t;

static cap_xfyun_state_t s_xfyun = {0};

static void cap_xfyun_copy_trimmed(char *dst, size_t dst_size, const char *src)
{
    const char *begin = src;
    const char *end = NULL;
    size_t len;

    if (!dst || dst_size == 0) {
        return;
    }
    dst[0] = '\0';
    if (!src) {
        return;
    }
    while (*begin && isspace((unsigned char)*begin)) {
        begin++;
    }
    end = begin + strlen(begin);
    while (end > begin && isspace((unsigned char)end[-1])) {
        end--;
    }
    len = (size_t)(end - begin);
    if (len >= dst_size) {
        len = dst_size - 1;
    }
    memcpy(dst, begin, len);
    dst[len] = '\0';
}

static bool cap_xfyun_is_configured(void)
{
    return s_xfyun.app_id[0] && s_xfyun.api_key[0] && s_xfyun.api_secret[0];
}

static uint16_t cap_xfyun_read_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t cap_xfyun_read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void cap_xfyun_write_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
}

static void cap_xfyun_write_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);
    p[3] = (uint8_t)((v >> 24) & 0xff);
}

static esp_err_t cap_xfyun_write_wav_header(FILE *file, uint32_t data_size)
{
    uint8_t header[CAP_XFYUN_WAV_HEADER_BYTES] = {0};

    if (!file) {
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(header, "RIFF", 4);
    cap_xfyun_write_le32(header + 4, 36 + data_size);
    memcpy(header + 8, "WAVEfmt ", 8);
    cap_xfyun_write_le32(header + 16, 16);
    cap_xfyun_write_le16(header + 20, 1);
    cap_xfyun_write_le16(header + 22, 1);
    cap_xfyun_write_le32(header + 24, 16000);
    cap_xfyun_write_le32(header + 28, 16000 * 2);
    cap_xfyun_write_le16(header + 32, 2);
    cap_xfyun_write_le16(header + 34, 16);
    memcpy(header + 36, "data", 4);
    cap_xfyun_write_le32(header + 40, data_size);

    return fwrite(header, 1, sizeof(header), file) == sizeof(header) ? ESP_OK : ESP_FAIL;
}

static bool cap_xfyun_string_equals_ignore_case(const char *a, const char *b)
{
    if (!a || !b) {
        return false;
    }

    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) {
            return false;
        }
        a++;
        b++;
    }

    return *a == '\0' && *b == '\0';
}

static bool cap_xfyun_path_allowed(const char *path)
{
    return path && path[0] && strstr(path, "..") == NULL && strchr(path, '\\') == NULL;
}

static esp_err_t cap_xfyun_resolve_path(const char *path, char *out, size_t out_size)
{
    int written;

    if (!cap_xfyun_path_allowed(path) || !out || out_size == 0 || !s_xfyun.base_dir[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    if (strncmp(path, s_xfyun.base_dir, strlen(s_xfyun.base_dir)) == 0) {
        strlcpy(out, path, out_size);
        return ESP_OK;
    }

    written = snprintf(out,
                       out_size,
                       "%s%s%s",
                       s_xfyun.base_dir,
                       path[0] == '/' ? "" : "/",
                       path);
    if (written < 0 || (size_t)written >= out_size) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static esp_err_t cap_xfyun_base64_encode(const uint8_t *input,
                                         size_t input_len,
                                         char **out)
{
    size_t out_len = ((input_len + 2) / 3) * 4 + 1;
    size_t olen = 0;
    char *buf = NULL;

    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = NULL;

    buf = malloc(out_len);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }
    if (mbedtls_base64_encode((unsigned char *)buf, out_len, &olen, input, input_len) != 0) {
        free(buf);
        return ESP_FAIL;
    }
    buf[olen] = '\0';
    *out = buf;
    return ESP_OK;
}

static esp_err_t cap_xfyun_base64_decode_to_file(const char *b64, FILE *file)
{
    size_t raw_len = 0;
    unsigned char *raw = NULL;
    int ret;

    if (!b64 || !file) {
        return ESP_ERR_INVALID_ARG;
    }

    ret = mbedtls_base64_decode(NULL, 0, &raw_len, (const unsigned char *)b64, strlen(b64));
    if (ret != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL || raw_len == 0) {
        return ESP_FAIL;
    }

    raw = malloc(raw_len);
    if (!raw) {
        return ESP_ERR_NO_MEM;
    }

    ret = mbedtls_base64_decode(raw, raw_len, &raw_len, (const unsigned char *)b64, strlen(b64));
    if (ret != 0) {
        free(raw);
        return ESP_FAIL;
    }
    if (fwrite(raw, 1, raw_len, file) != raw_len) {
        free(raw);
        return ESP_FAIL;
    }
    free(raw);
    return ESP_OK;
}

static size_t cap_xfyun_url_encode(const char *src, char *dst, size_t dst_size)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t pos = 0;

    if (!src || !dst || dst_size == 0) {
        return 0;
    }

    while (*src && pos < dst_size - 1) {
        unsigned char c = (unsigned char)*src++;
        bool safe = (c >= 'A' && c <= 'Z') ||
                    (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '9') ||
                    c == '-' || c == '_' || c == '.' || c == '~';
        if (safe) {
            dst[pos++] = (char)c;
        } else {
            if (pos + 3 >= dst_size) {
                break;
            }
            dst[pos++] = '%';
            dst[pos++] = hex[c >> 4];
            dst[pos++] = hex[c & 0x0F];
        }
    }
    dst[pos] = '\0';
    return pos;
}

static esp_err_t cap_xfyun_make_date(char *date, size_t date_size)
{
    time_t now;
    struct tm tm_utc;

    if (!date || date_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    time(&now);
    gmtime_r(&now, &tm_utc);
    if (strftime(date, date_size, "%a, %d %b %Y %H:%M:%S GMT", &tm_utc) == 0) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t cap_xfyun_hmac_sha256_base64(const char *key,
                                              const char *data,
                                              char *output,
                                              size_t output_size)
{
    const mbedtls_md_info_t *md_info = NULL;
    unsigned char digest[32];
    size_t olen = 0;

    md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!md_info || !key || !data || !output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (mbedtls_md_hmac(md_info,
                        (const unsigned char *)key,
                        strlen(key),
                        (const unsigned char *)data,
                        strlen(data),
                        digest) != 0) {
        return ESP_FAIL;
    }
    if (mbedtls_base64_encode((unsigned char *)output,
                              output_size,
                              &olen,
                              digest,
                              sizeof(digest)) != 0 ||
            olen >= output_size) {
        return ESP_FAIL;
    }
    output[olen] = '\0';
    return ESP_OK;
}

static esp_err_t cap_xfyun_make_ws_url(const char *host,
                                       const char *path,
                                       char *url,
                                       size_t url_size)
{
    char date[CAP_XFYUN_DATE_LEN];
    char signature_origin[256];
    char signature[CAP_XFYUN_SIG_LEN];
    char authorization_origin[CAP_XFYUN_AUTH_LEN];
    char *authorization_b64 = NULL;
    char date_enc[128];
    char authorization_enc[CAP_XFYUN_URL_LEN];
    int written;
    esp_err_t err;

    err = cap_xfyun_make_date(date, sizeof(date));
    if (err != ESP_OK) {
        return err;
    }

    written = snprintf(signature_origin,
                       sizeof(signature_origin),
                       "host: %s\ndate: %s\nGET %s HTTP/1.1",
                       host,
                       date,
                       path);
    if (written < 0 || (size_t)written >= sizeof(signature_origin)) {
        return ESP_ERR_NO_MEM;
    }

    err = cap_xfyun_hmac_sha256_base64(s_xfyun.api_secret,
                                       signature_origin,
                                       signature,
                                       sizeof(signature));
    if (err != ESP_OK) {
        return err;
    }

    written = snprintf(authorization_origin,
                       sizeof(authorization_origin),
                       "api_key=\"%s\", algorithm=\"hmac-sha256\", headers=\"host date request-line\", signature=\"%s\"",
                       s_xfyun.api_key,
                       signature);
    if (written < 0 || (size_t)written >= sizeof(authorization_origin)) {
        return ESP_ERR_NO_MEM;
    }

    err = cap_xfyun_base64_encode((const uint8_t *)authorization_origin,
                                  strlen(authorization_origin),
                                  &authorization_b64);
    if (err != ESP_OK) {
        return err;
    }

    cap_xfyun_url_encode(date, date_enc, sizeof(date_enc));
    cap_xfyun_url_encode(authorization_b64, authorization_enc, sizeof(authorization_enc));
    free(authorization_b64);

    written = snprintf(url,
                       url_size,
                       "wss://%s%s?authorization=%s&date=%s&host=%s",
                       host,
                       path,
                       authorization_enc,
                       date_enc,
                       host);
    if (written < 0 || (size_t)written >= url_size) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void cap_xfyun_set_error(cap_xfyun_ws_ctx_t *ctx, const char *fmt, ...)
{
    va_list ap;

    if (!ctx) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(ctx->error, sizeof(ctx->error), fmt, ap);
    va_end(ap);
    xEventGroupSetBits(ctx->events, CAP_XFYUN_EVT_ERROR);
}

static void cap_xfyun_append_text(cap_xfyun_ws_ctx_t *ctx, const char *text)
{
    size_t len;

    if (!ctx || !text) {
        return;
    }
    len = strlen(text);
    if (ctx->text_len + len >= sizeof(ctx->text)) {
        len = sizeof(ctx->text) - ctx->text_len - 1;
    }
    if (len > 0) {
        memcpy(ctx->text + ctx->text_len, text, len);
        ctx->text_len += len;
        ctx->text[ctx->text_len] = '\0';
    }
}

static void cap_xfyun_parse_asr_result(cap_xfyun_ws_ctx_t *ctx, cJSON *root)
{
    cJSON *data = cJSON_GetObjectItem(root, "data");
    cJSON *result = data ? cJSON_GetObjectItem(data, "result") : NULL;
    cJSON *ws = result ? cJSON_GetObjectItem(result, "ws") : NULL;
    cJSON *item = NULL;

    if (!cJSON_IsArray(ws)) {
        return;
    }

    cJSON_ArrayForEach(item, ws) {
        cJSON *cw = cJSON_GetObjectItem(item, "cw");
        cJSON *word = NULL;
        if (!cJSON_IsArray(cw)) {
            continue;
        }
        cJSON_ArrayForEach(word, cw) {
            const char *w = cJSON_GetStringValue(cJSON_GetObjectItem(word, "w"));
            if (w) {
                cap_xfyun_append_text(ctx, w);
            }
        }
    }
}

static void cap_xfyun_process_json(cap_xfyun_ws_ctx_t *ctx, const char *json)
{
    cJSON *root = cJSON_Parse(json);
    cJSON *code_item = NULL;
    cJSON *data = NULL;
    int code;
    int status;

    if (!root) {
        cap_xfyun_set_error(ctx, "invalid JSON response");
        return;
    }

    code_item = cJSON_GetObjectItem(root, "code");
    code = cJSON_IsNumber(code_item) ? code_item->valueint : -1;
    if (code != 0) {
        const char *message = cJSON_GetStringValue(cJSON_GetObjectItem(root, "message"));
        cap_xfyun_set_error(ctx, "server code=%d message=%s", code, message ? message : "");
        cJSON_Delete(root);
        return;
    }

    data = cJSON_GetObjectItem(root, "data");
    status = cJSON_IsNumber(cJSON_GetObjectItem(data, "status")) ?
             cJSON_GetObjectItem(data, "status")->valueint : -1;

    if (ctx->op == CAP_XFYUN_OP_ASR) {
        cap_xfyun_parse_asr_result(ctx, root);
    } else {
        const char *audio = cJSON_GetStringValue(cJSON_GetObjectItem(data, "audio"));
        if (audio && audio[0] && cap_xfyun_base64_decode_to_file(audio, ctx->out_file) != ESP_OK) {
            cap_xfyun_set_error(ctx, "failed to write decoded TTS audio");
            cJSON_Delete(root);
            return;
        }
    }

    if (status == 2) {
        xEventGroupSetBits(ctx->events, CAP_XFYUN_EVT_DONE);
    }
    cJSON_Delete(root);
}

static void cap_xfyun_ws_event_handler(void *handler_args,
                                       esp_event_base_t base,
                                       int32_t event_id,
                                       void *event_data)
{
    cap_xfyun_ws_ctx_t *ctx = (cap_xfyun_ws_ctx_t *)handler_args;
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

    (void)base;

    if (!ctx) {
        return;
    }

    if (event_id == WEBSOCKET_EVENT_CONNECTED) {
        xEventGroupSetBits(ctx->events, CAP_XFYUN_EVT_CONNECTED);
    } else if (event_id == WEBSOCKET_EVENT_DATA && data && data->data_len > 0) {
        if (data->payload_offset == 0) {
            free(ctx->frame_buf);
            ctx->frame_expected = data->payload_len > 0 ? (size_t)data->payload_len : (size_t)data->data_len;
            ctx->frame_buf = calloc(1, ctx->frame_expected + 1);
            ctx->frame_len = 0;
        }
        if (!ctx->frame_buf || ctx->frame_len + (size_t)data->data_len > ctx->frame_expected) {
            cap_xfyun_set_error(ctx, "websocket frame buffer overflow");
            return;
        }
        memcpy(ctx->frame_buf + ctx->frame_len, data->data_ptr, data->data_len);
        ctx->frame_len += (size_t)data->data_len;
        ctx->frame_buf[ctx->frame_len] = '\0';
        if (ctx->frame_len >= ctx->frame_expected) {
            cap_xfyun_process_json(ctx, ctx->frame_buf);
            free(ctx->frame_buf);
            ctx->frame_buf = NULL;
            ctx->frame_len = 0;
            ctx->frame_expected = 0;
        }
    } else if (event_id == WEBSOCKET_EVENT_ERROR) {
        cap_xfyun_set_error(ctx, "websocket error");
    } else if (event_id == WEBSOCKET_EVENT_DISCONNECTED) {
        EventBits_t bits = xEventGroupGetBits(ctx->events);
        if ((bits & CAP_XFYUN_EVT_DONE) == 0) {
            cap_xfyun_set_error(ctx, "websocket disconnected before completion");
        }
    }
}

static char *cap_xfyun_build_asr_frame(int status,
                                       uint32_t sample_rate,
                                       const uint8_t *audio,
                                       size_t audio_len)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *common = cJSON_CreateObject();
    cJSON *business = cJSON_CreateObject();
    cJSON *data = cJSON_CreateObject();
    char *audio_b64 = NULL;
    char format[32];
    char *json = NULL;

    if (!root || !common || !business || !data) {
        goto cleanup;
    }
    if (audio_len > 0 && cap_xfyun_base64_encode(audio, audio_len, &audio_b64) != ESP_OK) {
        goto cleanup;
    }

    snprintf(format, sizeof(format), "audio/L16;rate=%u", (unsigned)sample_rate);
    cJSON_AddStringToObject(common, "app_id", s_xfyun.app_id);
    cJSON_AddStringToObject(business, "language", "zh_cn");
    cJSON_AddStringToObject(business, "domain", "iat");
    cJSON_AddStringToObject(business, "accent", "mandarin");
    cJSON_AddStringToObject(data, "format", format);
    cJSON_AddStringToObject(data, "encoding", "raw");
    cJSON_AddNumberToObject(data, "status", status);
    cJSON_AddStringToObject(data, "audio", audio_b64 ? audio_b64 : "");
    cJSON_AddItemToObject(root, "common", common);
    cJSON_AddItemToObject(root, "business", business);
    cJSON_AddItemToObject(root, "data", data);
    common = business = data = NULL;
    json = cJSON_PrintUnformatted(root);

cleanup:
    free(audio_b64);
    cJSON_Delete(common);
    cJSON_Delete(business);
    cJSON_Delete(data);
    cJSON_Delete(root);
    return json;
}

static char *cap_xfyun_build_tts_frame(const char *text, const char *voice)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *common = cJSON_CreateObject();
    cJSON *business = cJSON_CreateObject();
    cJSON *data = cJSON_CreateObject();
    char *text_b64 = NULL;
    char *json = NULL;

    if (!root || !common || !business || !data) {
        goto cleanup;
    }
    if (cap_xfyun_base64_encode((const uint8_t *)text, strlen(text), &text_b64) != ESP_OK) {
        goto cleanup;
    }

    cJSON_AddStringToObject(common, "app_id", s_xfyun.app_id);
    cJSON_AddStringToObject(business, "aue", "raw");
    cJSON_AddStringToObject(business, "auf", "audio/L16;rate=16000");
    cJSON_AddStringToObject(business, "vcn", voice && voice[0] ? voice : "xiaoyan");
    cJSON_AddStringToObject(business, "tte", "UTF8");
    cJSON_AddNumberToObject(data, "status", 2);
    cJSON_AddStringToObject(data, "text", text_b64);
    cJSON_AddItemToObject(root, "common", common);
    cJSON_AddItemToObject(root, "business", business);
    cJSON_AddItemToObject(root, "data", data);
    common = business = data = NULL;
    json = cJSON_PrintUnformatted(root);

cleanup:
    free(text_b64);
    cJSON_Delete(common);
    cJSON_Delete(business);
    cJSON_Delete(data);
    cJSON_Delete(root);
    return json;
}

static esp_err_t cap_xfyun_ws_start(const char *host,
                                    const char *path,
                                    cap_xfyun_ws_ctx_t *ctx,
                                    esp_websocket_client_handle_t *out_client)
{
    char url[CAP_XFYUN_URL_LEN];
    esp_websocket_client_config_t config = {0};
    esp_websocket_client_handle_t client = NULL;
    EventBits_t bits;
    esp_err_t err;

    err = cap_xfyun_make_ws_url(host, path, url, sizeof(url));
    if (err != ESP_OK) {
        return err;
    }

    config.uri = url;
    config.buffer_size = CAP_XFYUN_WS_BUF;
    config.task_stack = CAP_XFYUN_WS_STACK;
    config.network_timeout_ms = 15000;
    config.disable_auto_reconnect = true;
    config.crt_bundle_attach = esp_crt_bundle_attach;

    client = esp_websocket_client_init(&config);
    if (!client) {
        return ESP_FAIL;
    }
    esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY, cap_xfyun_ws_event_handler, ctx);
    err = esp_websocket_client_start(client);
    if (err != ESP_OK) {
        esp_websocket_client_destroy(client);
        return err;
    }

    bits = xEventGroupWaitBits(ctx->events,
                               CAP_XFYUN_EVT_CONNECTED | CAP_XFYUN_EVT_ERROR,
                               pdFALSE,
                               pdFALSE,
                               pdMS_TO_TICKS(CAP_XFYUN_CONNECT_TIMEOUT_MS));
    if ((bits & CAP_XFYUN_EVT_CONNECTED) == 0) {
        esp_websocket_client_stop(client);
        esp_websocket_client_destroy(client);
        return ESP_ERR_TIMEOUT;
    }

    *out_client = client;
    return ESP_OK;
}

static void cap_xfyun_ws_cleanup(esp_websocket_client_handle_t client, cap_xfyun_ws_ctx_t *ctx)
{
    if (client) {
        esp_websocket_client_stop(client);
        esp_websocket_client_destroy(client);
    }
    if (ctx) {
        free(ctx->frame_buf);
        ctx->frame_buf = NULL;
        if (ctx->events) {
            vEventGroupDelete(ctx->events);
            ctx->events = NULL;
        }
    }
}

static esp_err_t cap_xfyun_parse_wav(FILE *file, cap_xfyun_wav_info_t *info)
{
    uint8_t header[12];
    bool got_fmt = false;
    bool got_data = false;

    if (!file || !info) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(info, 0, sizeof(*info));
    if (fread(header, 1, sizeof(header), file) != sizeof(header) ||
            memcmp(header, "RIFF", 4) != 0 ||
            memcmp(header + 8, "WAVE", 4) != 0) {
        return ESP_ERR_INVALID_ARG;
    }

    while (!got_data) {
        uint8_t chunk[8];
        uint32_t chunk_size;
        long chunk_data_pos;

        if (fread(chunk, 1, sizeof(chunk), file) != sizeof(chunk)) {
            break;
        }
        chunk_size = cap_xfyun_read_le32(chunk + 4);
        chunk_data_pos = ftell(file);
        if (memcmp(chunk, "fmt ", 4) == 0) {
            uint8_t fmt[16];
            if (chunk_size < sizeof(fmt) || fread(fmt, 1, sizeof(fmt), file) != sizeof(fmt)) {
                return ESP_ERR_INVALID_ARG;
            }
            if (cap_xfyun_read_le16(fmt) != 1) {
                return ESP_ERR_NOT_SUPPORTED;
            }
            info->channels = cap_xfyun_read_le16(fmt + 2);
            info->sample_rate = cap_xfyun_read_le32(fmt + 4);
            info->bits_per_sample = cap_xfyun_read_le16(fmt + 14);
            got_fmt = true;
        } else if (memcmp(chunk, "data", 4) == 0) {
            info->data_offset = (uint32_t)chunk_data_pos;
            info->data_size = chunk_size;
            got_data = true;
        }
        fseek(file, chunk_data_pos + chunk_size + (chunk_size & 1U), SEEK_SET);
    }

    if (!got_fmt || !got_data || info->channels != 1 || info->bits_per_sample != 16 ||
            (info->sample_rate != 8000 && info->sample_rate != 16000)) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return ESP_OK;
}

static esp_err_t cap_xfyun_send_asr_file(esp_websocket_client_handle_t client,
                                         FILE *file,
                                         uint32_t sample_rate,
                                         uint32_t data_size)
{
    uint8_t *buf = NULL;
    uint32_t sent_total = 0;
    int status = 0;
    esp_err_t err = ESP_OK;

    buf = malloc(CAP_XFYUN_FRAME_BYTES);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }

    while (sent_total < data_size) {
        size_t want = data_size - sent_total;
        size_t got;
        char *frame = NULL;
        int sent;

        if (want > CAP_XFYUN_FRAME_BYTES) {
            want = CAP_XFYUN_FRAME_BYTES;
        }
        got = fread(buf, 1, want, file);
        if (got == 0) {
            err = ESP_FAIL;
            break;
        }
        sent_total += (uint32_t)got;
        status = (sent_total >= data_size) ? (status == 0 ? 2 : 2) : status;
        frame = cap_xfyun_build_asr_frame(status, sample_rate, buf, got);
        if (!frame) {
            err = ESP_ERR_NO_MEM;
            break;
        }
        sent = esp_websocket_client_send_text(client, frame, strlen(frame), pdMS_TO_TICKS(5000));
        free(frame);
        if (sent < 0) {
            err = ESP_FAIL;
            break;
        }
        if (status == 0) {
            status = 1;
        }
        vTaskDelay(pdMS_TO_TICKS(40));
    }

    free(buf);
    return err;
}

static esp_err_t cap_xfyun_asr_file_execute(const char *input_json,
                                            const claw_cap_call_context_t *ctx,
                                            char *output,
                                            size_t output_size)
{
    cJSON *root = NULL;
    const char *path = NULL;
    char resolved[CAP_XFYUN_PATH_LEN];
    FILE *file = NULL;
    cap_xfyun_wav_info_t wav = {0};
    cap_xfyun_ws_ctx_t ws = {0};
    esp_websocket_client_handle_t client = NULL;
    EventBits_t bits;
    esp_err_t err;

    (void)ctx;

    if (!cap_xfyun_is_configured()) {
        snprintf(output, output_size, "Error: XFYun APP ID, API Key, and API Secret must be configured.");
        return ESP_ERR_INVALID_STATE;
    }

    root = cJSON_Parse(input_json && input_json[0] ? input_json : "{}");
    path = root ? cJSON_GetStringValue(cJSON_GetObjectItem(root, "path")) : NULL;
    if (!path || cap_xfyun_resolve_path(path, resolved, sizeof(resolved)) != ESP_OK) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: path is required and must stay under %s.", s_xfyun.base_dir);
        return ESP_ERR_INVALID_ARG;
    }
    cJSON_Delete(root);

    file = fopen(resolved, "rb");
    if (!file) {
        snprintf(output, output_size, "Error: cannot open %s.", resolved);
        return ESP_FAIL;
    }

    err = cap_xfyun_parse_wav(file, &wav);
    if (err != ESP_OK) {
        fclose(file);
        snprintf(output, output_size, "Error: ASR input must be mono 16-bit PCM WAV at 8k or 16k.");
        return err;
    }
    fseek(file, wav.data_offset, SEEK_SET);

    ws.events = xEventGroupCreate();
    ws.op = CAP_XFYUN_OP_ASR;
    if (!ws.events) {
        fclose(file);
        return ESP_ERR_NO_MEM;
    }

    err = cap_xfyun_ws_start("iat-api.xfyun.cn", "/v2/iat", &ws, &client);
    if (err == ESP_OK) {
        err = cap_xfyun_send_asr_file(client, file, wav.sample_rate, wav.data_size);
    }
    fclose(file);
    if (err == ESP_OK) {
        bits = xEventGroupWaitBits(ws.events,
                                   CAP_XFYUN_EVT_DONE | CAP_XFYUN_EVT_ERROR,
                                   pdFALSE,
                                   pdFALSE,
                                   pdMS_TO_TICKS(CAP_XFYUN_RESULT_TIMEOUT_MS));
        if (bits & CAP_XFYUN_EVT_ERROR) {
            err = ESP_FAIL;
        } else if ((bits & CAP_XFYUN_EVT_DONE) == 0) {
            err = ESP_ERR_TIMEOUT;
        }
    }

    if (err == ESP_OK) {
        snprintf(output, output_size, "%s", ws.text[0] ? ws.text : "(no speech recognized)");
    } else {
        snprintf(output, output_size, "Error: XFYun ASR failed (%s): %s", esp_err_to_name(err), ws.error);
    }

    cap_xfyun_ws_cleanup(client, &ws);
    return err;
}

static esp_err_t cap_xfyun_tts_execute(const char *input_json,
                                       const claw_cap_call_context_t *ctx,
                                       char *output,
                                       size_t output_size)
{
    cJSON *root = NULL;
    const char *text = NULL;
    const char *voice = NULL;
    const char *path = NULL;
    const char *format = NULL;
    char resolved[CAP_XFYUN_PATH_LEN];
    char *frame = NULL;
    cap_xfyun_ws_ctx_t ws = {0};
    esp_websocket_client_handle_t client = NULL;
    EventBits_t bits;
    int sent;
    bool wav_output = false;
    long end_pos = 0;
    uint32_t data_size = 0;
    esp_err_t err;

    (void)ctx;

    if (!cap_xfyun_is_configured()) {
        snprintf(output, output_size, "Error: XFYun APP ID, API Key, and API Secret must be configured.");
        return ESP_ERR_INVALID_STATE;
    }

    root = cJSON_Parse(input_json && input_json[0] ? input_json : "{}");
    text = root ? cJSON_GetStringValue(cJSON_GetObjectItem(root, "text")) : NULL;
    voice = root ? cJSON_GetStringValue(cJSON_GetObjectItem(root, "voice")) : NULL;
    path = root ? cJSON_GetStringValue(cJSON_GetObjectItem(root, "path")) : NULL;
    format = root ? cJSON_GetStringValue(cJSON_GetObjectItem(root, "format")) : NULL;
    wav_output = (format && cap_xfyun_string_equals_ignore_case(format, "wav")) ||
                 (!format && path && cap_xfyun_string_equals_ignore_case(strrchr(path, '.') ? strrchr(path, '.') + 1 : "", "wav"));
    if (!text || !text[0]) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: text is required.");
        return ESP_ERR_INVALID_ARG;
    }
    if (cap_xfyun_resolve_path(path && path[0] ? path : CAP_XFYUN_TTS_OUTPUT_DEFAULT,
                               resolved,
                               sizeof(resolved)) != ESP_OK) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: output path must stay under %s.", s_xfyun.base_dir);
        return ESP_ERR_INVALID_ARG;
    }

    ws.events = xEventGroupCreate();
    ws.op = CAP_XFYUN_OP_TTS;
    ws.out_file = fopen(resolved, "wb");
    if (!ws.events || !ws.out_file) {
        cJSON_Delete(root);
        cap_xfyun_ws_cleanup(NULL, &ws);
        if (ws.out_file) {
            fclose(ws.out_file);
        }
        return ESP_ERR_NO_MEM;
    }
    if (wav_output && cap_xfyun_write_wav_header(ws.out_file, 0) != ESP_OK) {
        cJSON_Delete(root);
        cap_xfyun_ws_cleanup(NULL, &ws);
        fclose(ws.out_file);
        ws.out_file = NULL;
        return ESP_FAIL;
    }

    err = cap_xfyun_ws_start("tts-api.xfyun.cn", "/v2/tts", &ws, &client);
    if (err == ESP_OK) {
        frame = cap_xfyun_build_tts_frame(text, voice);
        if (!frame) {
            err = ESP_ERR_NO_MEM;
        } else {
            sent = esp_websocket_client_send_text(client, frame, strlen(frame), pdMS_TO_TICKS(5000));
            if (sent < 0) {
                err = ESP_FAIL;
            }
        }
    }
    cJSON_Delete(root);
    free(frame);

    if (err == ESP_OK) {
        bits = xEventGroupWaitBits(ws.events,
                                   CAP_XFYUN_EVT_DONE | CAP_XFYUN_EVT_ERROR,
                                   pdFALSE,
                                   pdFALSE,
                                   pdMS_TO_TICKS(CAP_XFYUN_RESULT_TIMEOUT_MS));
        if (bits & CAP_XFYUN_EVT_ERROR) {
            err = ESP_FAIL;
        } else if ((bits & CAP_XFYUN_EVT_DONE) == 0) {
            err = ESP_ERR_TIMEOUT;
        }
    }

    if (ws.out_file) {
        if (wav_output && err == ESP_OK) {
            end_pos = ftell(ws.out_file);
            if (end_pos >= CAP_XFYUN_WAV_HEADER_BYTES) {
                data_size = (uint32_t)(end_pos - CAP_XFYUN_WAV_HEADER_BYTES);
                if (fseek(ws.out_file, 0, SEEK_SET) != 0 ||
                        cap_xfyun_write_wav_header(ws.out_file, data_size) != ESP_OK ||
                        fseek(ws.out_file, 0, SEEK_END) != 0) {
                    err = ESP_FAIL;
                    cap_xfyun_set_error(&ws, "failed to finalize WAV header");
                }
            } else {
                err = ESP_FAIL;
                cap_xfyun_set_error(&ws, "invalid WAV output size");
            }
        }
        fclose(ws.out_file);
        ws.out_file = NULL;
    }

    if (err == ESP_OK) {
        if (wav_output) {
            snprintf(output, output_size, "OK: synthesized 16k WAV audio to %s with voice=%s.",
                     resolved, voice && voice[0] ? voice : "xiaoyan");
        } else {
            snprintf(output, output_size, "OK: synthesized raw 16k PCM audio to %s", resolved);
        }
    } else {
        snprintf(output, output_size, "Error: XFYun TTS failed (%s): %s", esp_err_to_name(err), ws.error);
    }
    cap_xfyun_ws_cleanup(client, &ws);
    return err;
}

static const claw_cap_descriptor_t s_xfyun_descriptors[] = {
    {
        .id = "xfyun_asr_file",
        .name = "xfyun_asr_file",
        .family = "speech",
        .description = "Recognize speech from a mono 16-bit PCM WAV file using XFYun ASR.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}",
        .execute = cap_xfyun_asr_file_execute,
    },
    {
        .id = "xfyun_tts",
        .name = "xfyun_tts",
        .family = "speech",
        .description = "Synthesize text to a raw 16 kHz PCM or WAV file using XFYun TTS.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"},\"voice\":{\"type\":\"string\"},\"format\":{\"type\":\"string\",\"enum\":[\"raw\",\"pcm\",\"wav\"]}},\"required\":[\"text\"]}",
        .execute = cap_xfyun_tts_execute,
    },
};

static const claw_cap_group_t s_xfyun_group = {
    .group_id = "cap_xfyun_speech",
    .descriptors = s_xfyun_descriptors,
    .descriptor_count = sizeof(s_xfyun_descriptors) / sizeof(s_xfyun_descriptors[0]),
};

esp_err_t cap_xfyun_speech_set_config(const cap_xfyun_speech_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }
    cap_xfyun_copy_trimmed(s_xfyun.app_id, sizeof(s_xfyun.app_id), config->app_id);
    cap_xfyun_copy_trimmed(s_xfyun.api_key, sizeof(s_xfyun.api_key), config->api_key);
    cap_xfyun_copy_trimmed(s_xfyun.api_secret, sizeof(s_xfyun.api_secret), config->api_secret);
    return ESP_OK;
}

esp_err_t cap_xfyun_speech_set_base_dir(const char *base_dir)
{
    if (!base_dir || !base_dir[0] || base_dir[0] != '/') {
        return ESP_ERR_INVALID_ARG;
    }
    strlcpy(s_xfyun.base_dir, base_dir, sizeof(s_xfyun.base_dir));
    return ESP_OK;
}

esp_err_t cap_xfyun_speech_register_group(void)
{
    if (claw_cap_group_exists(s_xfyun_group.group_id)) {
        return ESP_OK;
    }
    return claw_cap_register_group(&s_xfyun_group);
}
