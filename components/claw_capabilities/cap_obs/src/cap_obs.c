/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_obs.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "claw_cap.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "mbedtls/base64.h"
#include "mbedtls/md.h"

static const char *TAG = "cap_obs";

#define CAP_OBS_ENDPOINT_LEN       320
#define CAP_OBS_BUCKET_LEN         128
#define CAP_OBS_AK_LEN             128
#define CAP_OBS_SK_LEN             160
#define CAP_OBS_TOKEN_LEN          1024
#define CAP_OBS_URL_LEN            1024
#define CAP_OBS_CANONICAL_LEN      768
#define CAP_OBS_SIGN_LEN           2200
#define CAP_OBS_AUTH_LEN           192
#define CAP_OBS_DATE_LEN           64
#define CAP_OBS_HTTP_BUF_MAX       (24 * 1024)
#define CAP_OBS_DEFAULT_LIST_COUNT 50
#define CAP_OBS_DEFAULT_GET_BYTES  8192

typedef struct {
    char endpoint[CAP_OBS_ENDPOINT_LEN];
    char bucket[CAP_OBS_BUCKET_LEN];
    char access_key[CAP_OBS_AK_LEN];
    char secret_key[CAP_OBS_SK_LEN];
    char security_token[CAP_OBS_TOKEN_LEN];
} cap_obs_state_t;

typedef struct {
    char *data;
    size_t len;
    size_t cap;
    bool truncated;
} cap_obs_http_buf_t;

static cap_obs_state_t s_obs = {0};

static bool cap_obs_is_configured(void)
{
    return s_obs.endpoint[0] && s_obs.bucket[0] && s_obs.access_key[0] && s_obs.secret_key[0];
}

static void cap_obs_copy_trimmed(char *dst, size_t dst_size, const char *src)
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

static void cap_obs_copy_endpoint(char *dst, size_t dst_size, const char *src)
{
    char temp[CAP_OBS_ENDPOINT_LEN];
    char *slash = NULL;
    size_t len;

    cap_obs_copy_trimmed(temp, sizeof(temp), src);

    if (strncmp(temp, "https://", 8) == 0) {
        memmove(temp, temp + 8, strlen(temp + 8) + 1);
    } else if (strncmp(temp, "http://", 7) == 0) {
        memmove(temp, temp + 7, strlen(temp + 7) + 1);
    }

    slash = strchr(temp, '/');
    if (slash) {
        *slash = '\0';
    }

    len = strlen(temp);
    while (len > 0 && temp[len - 1] == '.') {
        temp[--len] = '\0';
    }

    strlcpy(dst, temp, dst_size);
}

static esp_err_t cap_obs_http_event_handler(esp_http_client_event_t *event)
{
    cap_obs_http_buf_t *buf = NULL;
    size_t can_copy;

    if (!event || event->event_id != HTTP_EVENT_ON_DATA || event->data_len <= 0) {
        return ESP_OK;
    }

    buf = (cap_obs_http_buf_t *)event->user_data;
    if (!buf || !buf->data || buf->cap == 0) {
        return ESP_OK;
    }

    if (buf->len + (size_t)event->data_len + 1 > buf->cap) {
        buf->truncated = true;
        if (buf->len >= buf->cap - 1) {
            return ESP_OK;
        }
        can_copy = (buf->cap - 1) - buf->len;
    } else {
        can_copy = (size_t)event->data_len;
    }

    memcpy(buf->data + buf->len, event->data, can_copy);
    buf->len += can_copy;
    buf->data[buf->len] = '\0';
    return ESP_OK;
}

static size_t cap_obs_url_encode(const char *src, char *dst, size_t dst_size, bool keep_slash)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t pos = 0;

    if (!src || !dst || dst_size == 0) {
        return 0;
    }

    while (*src && pos < dst_size - 1) {
        unsigned char c = (unsigned char)*src;
        bool safe = (c >= 'A' && c <= 'Z') ||
                    (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '9') ||
                    c == '-' || c == '_' || c == '.' || c == '~' ||
                    (keep_slash && c == '/');

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
        src++;
    }

    dst[pos] = '\0';
    return pos;
}

static esp_err_t cap_obs_make_date(char *date, size_t date_size)
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

static esp_err_t cap_obs_hmac_sha1_base64(const char *key,
                                          const char *data,
                                          char *output,
                                          size_t output_size)
{
    const mbedtls_md_info_t *md_info = NULL;
    unsigned char digest[20];
    size_t olen = 0;
    int ret;

    if (!key || !data || !output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA1);
    if (!md_info) {
        return ESP_FAIL;
    }

    ret = mbedtls_md_hmac(md_info,
                          (const unsigned char *)key,
                          strlen(key),
                          (const unsigned char *)data,
                          strlen(data),
                          digest);
    if (ret != 0) {
        return ESP_FAIL;
    }

    ret = mbedtls_base64_encode((unsigned char *)output,
                                output_size,
                                &olen,
                                digest,
                                sizeof(digest));
    if (ret != 0 || olen >= output_size) {
        return ESP_FAIL;
    }

    output[olen] = '\0';
    return ESP_OK;
}

static esp_err_t cap_obs_make_auth(const char *method,
                                   const char *date,
                                   const char *canonical_resource,
                                   char *auth,
                                   size_t auth_size)
{
    char string_to_sign[CAP_OBS_SIGN_LEN];
    char signature[64];
    int written;
    esp_err_t err;

    if (!method || !date || !canonical_resource || !auth || auth_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_obs.security_token[0]) {
        written = snprintf(string_to_sign,
                           sizeof(string_to_sign),
                           "%s\n\n\n%s\nx-obs-security-token:%s\n%s",
                           method,
                           date,
                           s_obs.security_token,
                           canonical_resource);
    } else {
        written = snprintf(string_to_sign,
                           sizeof(string_to_sign),
                           "%s\n\n\n%s\n%s",
                           method,
                           date,
                           canonical_resource);
    }
    if (written < 0 || (size_t)written >= sizeof(string_to_sign)) {
        return ESP_ERR_NO_MEM;
    }

    err = cap_obs_hmac_sha1_base64(s_obs.secret_key, string_to_sign, signature, sizeof(signature));
    if (err != ESP_OK) {
        return err;
    }

    written = snprintf(auth, auth_size, "OBS %s:%s", s_obs.access_key, signature);
    if (written < 0 || (size_t)written >= auth_size) {
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

static esp_err_t cap_obs_http_get(const char *url,
                                  const char *canonical_resource,
                                  size_t max_response_bytes,
                                  cap_obs_http_buf_t *out_buf,
                                  int *out_status)
{
    esp_http_client_config_t config = {0};
    esp_http_client_handle_t client = NULL;
    char date[CAP_OBS_DATE_LEN];
    char auth[CAP_OBS_AUTH_LEN];
    size_t cap;
    esp_err_t err;
    int status;

    if (!url || !canonical_resource || !out_buf) {
        return ESP_ERR_INVALID_ARG;
    }

    cap = max_response_bytes + 1;
    if (cap < 1024) {
        cap = 1024;
    }
    if (cap > CAP_OBS_HTTP_BUF_MAX) {
        cap = CAP_OBS_HTTP_BUF_MAX;
    }

    memset(out_buf, 0, sizeof(*out_buf));
    out_buf->data = calloc(1, cap);
    if (!out_buf->data) {
        return ESP_ERR_NO_MEM;
    }
    out_buf->cap = cap;

    err = cap_obs_make_date(date, sizeof(date));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create OBS date");
        goto fail;
    }
    err = cap_obs_make_auth("GET", date, canonical_resource, auth, sizeof(auth));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to sign OBS request");
        goto fail;
    }

    config.url = url;
    config.method = HTTP_METHOD_GET;
    config.event_handler = cap_obs_http_event_handler;
    config.user_data = out_buf;
    config.timeout_ms = 30000;
    config.buffer_size = 1024;
    config.buffer_size_tx = 2048;
    config.crt_bundle_attach = esp_crt_bundle_attach;

    client = esp_http_client_init(&config);
    if (!client) {
        err = ESP_FAIL;
        goto fail;
    }

    esp_http_client_set_header(client, "Date", date);
    esp_http_client_set_header(client, "Authorization", auth);
    if (s_obs.security_token[0]) {
        esp_http_client_set_header(client, "x-obs-security-token", s_obs.security_token);
    }

    err = esp_http_client_perform(client);
    status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    client = NULL;

    if (out_status) {
        *out_status = status;
    }
    if (err != ESP_OK) {
        goto fail;
    }
    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG, "OBS request failed status=%d body=%s", status, out_buf->data);
        return ESP_FAIL;
    }

    return ESP_OK;

fail:
    if (client) {
        esp_http_client_cleanup(client);
    }
    free(out_buf->data);
    memset(out_buf, 0, sizeof(*out_buf));
    return err;
}

static void cap_obs_format_request_error(const char *op,
                                         esp_err_t err,
                                         int status,
                                         const cap_obs_http_buf_t *buf,
                                         char *output,
                                         size_t output_size)
{
    const char *body = (buf && buf->data && buf->data[0]) ? buf->data : "";

    if (body[0]) {
        snprintf(output,
                 output_size,
                 "Error: OBS %s request failed (%s, status=%d).\nOBS response:\n%.*s%s",
                 op,
                 esp_err_to_name(err),
                 status,
                 900,
                 body,
                 strlen(body) > 900 ? "\n(response truncated)" : "");
    } else {
        snprintf(output,
                 output_size,
                 "Error: OBS %s request failed (%s, status=%d).",
                 op,
                 esp_err_to_name(err),
                 status);
    }
}

static bool cap_obs_extract_tag(const char *begin,
                                const char *end,
                                const char *tag,
                                char *output,
                                size_t output_size)
{
    char open_tag[32];
    char close_tag[32];
    const char *open = NULL;
    const char *value = NULL;
    const char *close = NULL;
    size_t len;

    if (!begin || !tag || !output || output_size == 0) {
        return false;
    }

    snprintf(open_tag, sizeof(open_tag), "<%s>", tag);
    snprintf(close_tag, sizeof(close_tag), "</%s>", tag);
    open = strstr(begin, open_tag);
    if (!open || (end && open >= end)) {
        return false;
    }
    value = open + strlen(open_tag);
    close = strstr(value, close_tag);
    if (!close || (end && close > end)) {
        return false;
    }

    len = (size_t)(close - value);
    if (len >= output_size) {
        len = output_size - 1;
    }
    memcpy(output, value, len);
    output[len] = '\0';
    return true;
}

static void cap_obs_xml_unescape(char *text)
{
    char *src = text;
    char *dst = text;

    if (!text) {
        return;
    }

    while (*src) {
        if (strncmp(src, "&amp;", 5) == 0) {
            *dst++ = '&';
            src += 5;
        } else if (strncmp(src, "&lt;", 4) == 0) {
            *dst++ = '<';
            src += 4;
        } else if (strncmp(src, "&gt;", 4) == 0) {
            *dst++ = '>';
            src += 4;
        } else if (strncmp(src, "&quot;", 6) == 0) {
            *dst++ = '"';
            src += 6;
        } else if (strncmp(src, "&apos;", 6) == 0) {
            *dst++ = '\'';
            src += 6;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

static void cap_obs_format_list_response(const char *xml,
                                         bool truncated_response,
                                         char *output,
                                         size_t output_size)
{
    const char *cursor = xml;
    size_t offset = 0;
    int index = 0;
    int written;

    written = snprintf(output,
                       output_size,
                       "Huawei OBS objects in bucket %s:\n",
                       s_obs.bucket);
    if (written < 0 || (size_t)written >= output_size) {
        output[output_size - 1] = '\0';
        return;
    }
    offset = (size_t)written;

    while (cursor && *cursor && offset < output_size - 1) {
        const char *contents = strstr(cursor, "<Contents>");
        const char *contents_end = contents ? strstr(contents, "</Contents>") : NULL;
        char key[256] = {0};
        char size[32] = {0};
        char modified[64] = {0};

        if (!contents || !contents_end) {
            break;
        }

        if (cap_obs_extract_tag(contents, contents_end, "Key", key, sizeof(key))) {
            cap_obs_extract_tag(contents, contents_end, "Size", size, sizeof(size));
            cap_obs_extract_tag(contents, contents_end, "LastModified", modified, sizeof(modified));
            cap_obs_xml_unescape(key);

            written = snprintf(output + offset,
                               output_size - offset,
                               "%d. %s%s%s%s%s%s\n",
                               index + 1,
                               key,
                               size[0] ? " (" : "",
                               size[0] ? size : "",
                               size[0] ? " bytes)" : "",
                               modified[0] ? ", " : "",
                               modified[0] ? modified : "");
            if (written < 0 || (size_t)written >= output_size - offset) {
                output[output_size - 1] = '\0';
                return;
            }
            offset += (size_t)written;
            index++;
        }

        cursor = contents_end + strlen("</Contents>");
    }

    if (index == 0) {
        snprintf(output,
                 output_size,
                 "No objects found in bucket %s.%s",
                 s_obs.bucket,
                 truncated_response ? " Response was truncated." : "");
        return;
    }

    if (truncated_response && offset < output_size - 1) {
        snprintf(output + offset, output_size - offset, "(response truncated)\n");
    }
}

static int cap_obs_get_json_int(cJSON *root, const char *name, int fallback)
{
    cJSON *item = cJSON_GetObjectItem(root, name);

    if (!cJSON_IsNumber(item)) {
        return fallback;
    }
    return item->valueint;
}

static esp_err_t cap_obs_list_objects_execute(const char *input_json,
                                              const claw_cap_call_context_t *ctx,
                                              char *output,
                                              size_t output_size)
{
    cJSON *root = NULL;
    const char *prefix = NULL;
    int max_keys = CAP_OBS_DEFAULT_LIST_COUNT;
    char encoded_prefix[256];
    char url[CAP_OBS_URL_LEN];
    char canonical[CAP_OBS_CANONICAL_LEN];
    cap_obs_http_buf_t buf = {0};
    int status = 0;
    esp_err_t err;

    (void)ctx;

    if (!cap_obs_is_configured()) {
        snprintf(output, output_size, "Error: OBS endpoint, bucket, AK, and SK must be configured.");
        return ESP_ERR_INVALID_STATE;
    }

    root = cJSON_Parse(input_json && input_json[0] ? input_json : "{}");
    if (root) {
        prefix = cJSON_GetStringValue(cJSON_GetObjectItem(root, "prefix"));
        max_keys = cap_obs_get_json_int(root, "max_keys", CAP_OBS_DEFAULT_LIST_COUNT);
    }
    if (max_keys < 1) {
        max_keys = 1;
    } else if (max_keys > 1000) {
        max_keys = 1000;
    }

    cap_obs_url_encode(prefix ? prefix : "", encoded_prefix, sizeof(encoded_prefix), false);
    snprintf(canonical, sizeof(canonical), "/%s/", s_obs.bucket);
    if (encoded_prefix[0]) {
        snprintf(url,
                 sizeof(url),
                 "https://%s.%s/?prefix=%s&max-keys=%d",
                 s_obs.bucket,
                 s_obs.endpoint,
                 encoded_prefix,
                 max_keys);
    } else {
        snprintf(url,
                 sizeof(url),
                 "https://%s.%s/?max-keys=%d",
                 s_obs.bucket,
                 s_obs.endpoint,
                 max_keys);
    }

    err = cap_obs_http_get(url, canonical, CAP_OBS_HTTP_BUF_MAX - 1, &buf, &status);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        cap_obs_format_request_error("list", err, status, &buf, output, output_size);
        free(buf.data);
        return err;
    }

    cap_obs_format_list_response(buf.data, buf.truncated, output, output_size);
    free(buf.data);
    return ESP_OK;
}

static esp_err_t cap_obs_get_object_execute(const char *input_json,
                                            const claw_cap_call_context_t *ctx,
                                            char *output,
                                            size_t output_size)
{
    cJSON *root = NULL;
    const char *key = NULL;
    int max_bytes = CAP_OBS_DEFAULT_GET_BYTES;
    char encoded_key[384];
    char url[CAP_OBS_URL_LEN];
    char canonical[CAP_OBS_CANONICAL_LEN];
    cap_obs_http_buf_t buf = {0};
    int status = 0;
    esp_err_t err;

    (void)ctx;

    if (!cap_obs_is_configured()) {
        snprintf(output, output_size, "Error: OBS endpoint, bucket, AK, and SK must be configured.");
        return ESP_ERR_INVALID_STATE;
    }

    root = cJSON_Parse(input_json && input_json[0] ? input_json : "{}");
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input.");
        return ESP_ERR_INVALID_ARG;
    }

    key = cJSON_GetStringValue(cJSON_GetObjectItem(root, "key"));
    max_bytes = cap_obs_get_json_int(root, "max_bytes", CAP_OBS_DEFAULT_GET_BYTES);
    if (!key || !key[0] || key[0] == '/') {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: key is required and must be relative.");
        return ESP_ERR_INVALID_ARG;
    }
    if (max_bytes < 1) {
        max_bytes = 1;
    } else if (max_bytes > CAP_OBS_HTTP_BUF_MAX - 1) {
        max_bytes = CAP_OBS_HTTP_BUF_MAX - 1;
    }

    cap_obs_url_encode(key, encoded_key, sizeof(encoded_key), true);
    snprintf(url, sizeof(url), "https://%s.%s/%s", s_obs.bucket, s_obs.endpoint, encoded_key);
    snprintf(canonical, sizeof(canonical), "/%s/%s", s_obs.bucket, encoded_key);

    err = cap_obs_http_get(url, canonical, (size_t)max_bytes, &buf, &status);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        cap_obs_format_request_error("get", err, status, &buf, output, output_size);
        free(buf.data);
        return err;
    }

    snprintf(output,
             output_size,
             "Object: %s\nBytes: %u%s\n\n%s",
             key,
             (unsigned)buf.len,
             buf.truncated ? " (truncated)" : "",
             buf.data);
    free(buf.data);
    return ESP_OK;
}

static const claw_cap_descriptor_t s_obs_descriptors[] = {
    {
        .id = "obs_list_objects",
        .name = "obs_list_objects",
        .family = "storage",
        .description = "List objects in the configured Huawei Cloud OBS bucket. Optional input: prefix, max_keys.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{\"prefix\":{\"type\":\"string\"},\"max_keys\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":1000}}}",
        .execute = cap_obs_list_objects_execute,
    },
    {
        .id = "obs_get_object",
        .name = "obs_get_object",
        .family = "storage",
        .description = "Read a text object from the configured Huawei Cloud OBS bucket. Input: key, optional max_bytes.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{\"key\":{\"type\":\"string\"},\"max_bytes\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":24575}},\"required\":[\"key\"]}",
        .execute = cap_obs_get_object_execute,
    },
};

static const claw_cap_group_t s_obs_group = {
    .group_id = "cap_obs",
    .descriptors = s_obs_descriptors,
    .descriptor_count = sizeof(s_obs_descriptors) / sizeof(s_obs_descriptors[0]),
};

esp_err_t cap_obs_set_config(const cap_obs_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    cap_obs_copy_endpoint(s_obs.endpoint, sizeof(s_obs.endpoint), config->endpoint);
    cap_obs_copy_trimmed(s_obs.bucket, sizeof(s_obs.bucket), config->bucket);
    cap_obs_copy_trimmed(s_obs.access_key, sizeof(s_obs.access_key), config->access_key);
    cap_obs_copy_trimmed(s_obs.secret_key, sizeof(s_obs.secret_key), config->secret_key);
    cap_obs_copy_trimmed(s_obs.security_token, sizeof(s_obs.security_token), config->security_token);
    return ESP_OK;
}

esp_err_t cap_obs_register_group(void)
{
    if (claw_cap_group_exists(s_obs_group.group_id)) {
        return ESP_OK;
    }

    return claw_cap_register_group(&s_obs_group);
}
