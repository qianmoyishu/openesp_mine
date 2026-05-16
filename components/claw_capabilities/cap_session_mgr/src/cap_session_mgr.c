/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_session_mgr.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "cJSON.h"
#include "claw_cap.h"
#include "esp_log.h"
#include "freertos/semphr.h"

static const char *TAG = "cap_session_mgr";

#define CAP_SESSION_MGR_MAP_DIRNAME  "chat_map"
#define CAP_SESSION_MGR_PATH_SIZE    256
#define CAP_SESSION_MGR_KEY_SIZE     128
#define CAP_SESSION_MGR_ID_SIZE      128

typedef struct {
    bool configured;
    char session_root_dir[160];
    char mapping_root_dir[192];
    SemaphoreHandle_t mutex;
} cap_session_mgr_state_t;

static cap_session_mgr_state_t s_session_mgr = {0};

static bool cap_session_mgr_is_chat_event(const claw_event_t *event)
{
    return event &&
           strcmp(event->event_type, "message") == 0 &&
           event->source_channel[0] != '\0' &&
           event->chat_id[0] != '\0';
}

static uint32_t cap_session_mgr_hash(const char *text)
{
    uint32_t hash = 2166136261u;
    const unsigned char *ptr = (const unsigned char *)text;

    while (ptr && *ptr) {
        hash ^= *ptr++;
        hash *= 16777619u;
    }

    return hash;
}

static void cap_session_mgr_sanitize(const char *src, char *dst, size_t dst_size)
{
    size_t off = 0;

    if (!dst || dst_size == 0) {
        return;
    }
    dst[0] = '\0';
    if (!src) {
        return;
    }

    while (*src && off + 1 < dst_size) {
        char ch = *src++;

        if ((ch >= 'a' && ch <= 'z') ||
                (ch >= 'A' && ch <= 'Z') ||
                (ch >= '0' && ch <= '9')) {
            dst[off++] = ch;
        } else if (off == 0 || dst[off - 1] != '_') {
            dst[off++] = '_';
        }
    }
    if (off > 0 && dst[off - 1] == '_') {
        off--;
    }
    dst[off] = '\0';
}

static esp_err_t cap_session_mgr_ensure_dir(const char *path)
{
    struct stat st = {0};

    if (!path || !path[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (stat(path, &st) == 0) {
        return S_ISDIR(st.st_mode) ? ESP_OK : ESP_FAIL;
    }
    if (mkdir(path, 0755) == 0 || errno == EEXIST) {
        return ESP_OK;
    }
    return ESP_FAIL;
}

static esp_err_t cap_session_mgr_build_chat_key(const char *source_channel, const char *chat_id, char *buf, size_t buf_size)
{
    int written;

    if (!source_channel || !source_channel[0] || !chat_id || !chat_id[0] || !buf || buf_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    written = snprintf(buf, buf_size, "%s:%s", source_channel, chat_id);
    if (written < 0 || (size_t)written >= buf_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

static esp_err_t cap_session_mgr_build_mapping_path(const char *chat_key, char *path, size_t path_size)
{
    char safe_key[40];
    uint32_t hash;
    int written;

    if (!chat_key || !chat_key[0] || !path || path_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    cap_session_mgr_sanitize(chat_key, safe_key, sizeof(safe_key));
    if (strlen(safe_key) > 24) {
        safe_key[24] = '\0';
    }
    hash = cap_session_mgr_hash(chat_key);
    written = snprintf(path,
                       path_size,
                       "%s/chat_%s_%08" PRIx32 ".json",
                       s_session_mgr.mapping_root_dir,
                       safe_key[0] ? safe_key : "default",
                       hash);
    if (written < 0 || (size_t)written >= path_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

static esp_err_t cap_session_mgr_write_mapping_locked(const char *chat_key, int version)
{
    char path[CAP_SESSION_MGR_PATH_SIZE];
    cJSON *root = NULL;
    char *json = NULL;
    FILE *file = NULL;
    esp_err_t err;

    err = cap_session_mgr_build_mapping_path(chat_key, path, sizeof(path));
    if (err != ESP_OK) {
        return err;
    }

    root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "chat_key", chat_key);
    cJSON_AddNumberToObject(root, "version", version);
    json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        return ESP_ERR_NO_MEM;
    }

    file = fopen(path, "wb");
    if (!file) {
        free(json);
        return ESP_FAIL;
    }
    if (fputs(json, file) < 0) {
        fclose(file);
        free(json);
        return ESP_FAIL;
    }
    fclose(file);
    free(json);
    return ESP_OK;
}

static esp_err_t cap_session_mgr_read_version_locked(const char *chat_key, int *out_version)
{
    char path[CAP_SESSION_MGR_PATH_SIZE];
    FILE *file = NULL;
    long size;
    char *text = NULL;
    size_t read_bytes;
    cJSON *root = NULL;
    cJSON *version = NULL;
    esp_err_t err;

    if (!chat_key || !chat_key[0] || !out_version) {
        return ESP_ERR_INVALID_ARG;
    }

    err = cap_session_mgr_build_mapping_path(chat_key, path, sizeof(path));
    if (err != ESP_OK) {
        return err;
    }

    file = fopen(path, "rb");
    if (!file) {
        return errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return ESP_FAIL;
    }
    size = ftell(file);
    if (size < 0) {
        fclose(file);
        return ESP_FAIL;
    }
    if (fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return ESP_FAIL;
    }

    text = calloc(1, (size_t)size + 1);
    if (!text) {
        fclose(file);
        return ESP_ERR_NO_MEM;
    }
    read_bytes = fread(text, 1, (size_t)size, file);
    fclose(file);
    text[read_bytes] = '\0';

    root = cJSON_Parse(text);
    free(text);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    version = cJSON_GetObjectItemCaseSensitive(root, "version");
    if (!cJSON_IsNumber(version) || version->valueint < 1) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    *out_version = version->valueint;
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t cap_session_mgr_build_current_session_id_locked(const char *source_channel,
                                                                 const char *chat_id,
                                                                 char *buf,
                                                                 size_t buf_size)
{
    char chat_key[CAP_SESSION_MGR_KEY_SIZE];
    int version = 0;
    int written;
    esp_err_t err;

    err = cap_session_mgr_build_chat_key(source_channel, chat_id, chat_key, sizeof(chat_key));
    if (err != ESP_OK) {
        return err;
    }

    err = cap_session_mgr_read_version_locked(chat_key, &version);
    if (err == ESP_ERR_NOT_FOUND || err == ESP_ERR_INVALID_RESPONSE) {
        version = 1;
        err = cap_session_mgr_write_mapping_locked(chat_key, version);
    }
    if (err != ESP_OK) {
        return err;
    }

    written = snprintf(buf, buf_size, "%s:v%d", chat_key, version);
    if (written < 0 || (size_t)written >= buf_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

static esp_err_t cap_session_mgr_roll_locked(const char *source_channel,
                                             const char *chat_id,
                                             char *new_session_id,
                                             size_t new_session_id_size,
                                             int *out_version)
{
    char chat_key[CAP_SESSION_MGR_KEY_SIZE];
    int version = 0;
    esp_err_t err;

    err = cap_session_mgr_build_chat_key(source_channel, chat_id, chat_key, sizeof(chat_key));
    if (err != ESP_OK) {
        return err;
    }

    err = cap_session_mgr_read_version_locked(chat_key, &version);
    if (err == ESP_ERR_NOT_FOUND || err == ESP_ERR_INVALID_RESPONSE) {
        version = 0;
        err = ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    version++;
    err = cap_session_mgr_write_mapping_locked(chat_key, version);
    if (err != ESP_OK) {
        return err;
    }

    if (new_session_id && new_session_id_size > 0) {
        err = cap_session_mgr_build_current_session_id_locked(source_channel, chat_id, new_session_id, new_session_id_size);
        if (err != ESP_OK) {
            return err;
        }
    }
    if (out_version) {
        *out_version = version;
    }

    return ESP_OK;
}

static esp_err_t cap_session_mgr_roll_execute(const char *input_json,
                                              const claw_cap_call_context_t *ctx,
                                              char *output,
                                              size_t output_size)
{
    char session_id[CAP_SESSION_MGR_ID_SIZE];
    int version = 0;
    esp_err_t err;

    (void)input_json;

    if (!ctx || !ctx->channel || !ctx->channel[0] || !ctx->chat_id || !ctx->chat_id[0]) {
        if (output && output_size > 0) {
            snprintf(output, output_size, "{\"ok\":false,\"error\":\"channel and chat_id are required\"}");
        }
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_session_mgr.configured || !s_session_mgr.mutex) {
        if (output && output_size > 0) {
            snprintf(output, output_size, "{\"ok\":false,\"error\":\"session manager not configured\"}");
        }
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTakeRecursive(s_session_mgr.mutex, portMAX_DELAY);
    err = cap_session_mgr_roll_locked(ctx->channel, ctx->chat_id, session_id, sizeof(session_id), &version);
    xSemaphoreGiveRecursive(s_session_mgr.mutex);
    if (err != ESP_OK) {
        if (output && output_size > 0) {
            snprintf(output, output_size, "{\"ok\":false,\"error\":\"%s\"}", esp_err_to_name(err));
        }
        return err;
    }

    ESP_LOGI(TAG, "Rolled chat session %s:%s to version %d", ctx->channel, ctx->chat_id, version);
    if (output && output_size > 0) {
        snprintf(output, output_size, "{\"ok\":true,\"session_id\":\"%s\",\"version\":%d}", session_id, version);
    }
    return ESP_OK;
}

static const claw_cap_descriptor_t s_session_mgr_caps[] = {
    {
        .id = "roll_chat_session",
        .name = "roll_chat_session",
        .family = "system",
        .description = "Advance the current chat to a fresh persistent session.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_RESTRICTED,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute = cap_session_mgr_roll_execute,
    },
};

static const claw_cap_group_t s_session_mgr_group = {
    .group_id = "cap_session_mgr",
    .plugin_name = "cap_session_mgr",
    .version = "1.0.0",
    .descriptors = s_session_mgr_caps,
    .descriptor_count = sizeof(s_session_mgr_caps) / sizeof(s_session_mgr_caps[0]),
};

esp_err_t cap_session_mgr_register_group(void)
{
    return claw_cap_register_group(&s_session_mgr_group);
}

esp_err_t cap_session_mgr_set_session_root_dir(const char *session_root_dir)
{
    int written;
    SemaphoreHandle_t mutex = s_session_mgr.mutex;

    if (!session_root_dir || !session_root_dir[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(&s_session_mgr, 0, sizeof(s_session_mgr));
    s_session_mgr.mutex = mutex;
    strlcpy(s_session_mgr.session_root_dir, session_root_dir, sizeof(s_session_mgr.session_root_dir));
    written = snprintf(s_session_mgr.mapping_root_dir,
                       sizeof(s_session_mgr.mapping_root_dir),
                       "%s/%s",
                       session_root_dir,
                       CAP_SESSION_MGR_MAP_DIRNAME);
    if (written < 0 || (size_t)written >= sizeof(s_session_mgr.mapping_root_dir)) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (!s_session_mgr.mutex) {
        s_session_mgr.mutex = xSemaphoreCreateRecursiveMutex();
    }
    if (!s_session_mgr.mutex) {
        return ESP_ERR_NO_MEM;
    }
    if (cap_session_mgr_ensure_dir(s_session_mgr.session_root_dir) != ESP_OK ||
            cap_session_mgr_ensure_dir(s_session_mgr.mapping_root_dir) != ESP_OK) {
        return ESP_FAIL;
    }

    s_session_mgr.configured = true;
    return ESP_OK;
}

size_t cap_session_mgr_build_session_id(const claw_event_t *event, char *buf, size_t buf_size, void *user_ctx)
{
    esp_err_t err;

    (void)user_ctx;

    if (!buf || buf_size == 0 || !event) {
        return 0;
    }
    if (!cap_session_mgr_is_chat_event(event) || !s_session_mgr.configured || !s_session_mgr.mutex) {
        return claw_event_router_build_session_id(event, buf, buf_size);
    }

    xSemaphoreTakeRecursive(s_session_mgr.mutex, portMAX_DELAY);
    err = cap_session_mgr_build_current_session_id_locked(event->source_channel, event->chat_id, buf, buf_size);
    xSemaphoreGiveRecursive(s_session_mgr.mutex);
    if (err != ESP_OK) {
        ESP_LOGW(TAG,
                 "Falling back to default session id for %s:%s: %s",
                 event->source_channel,
                 event->chat_id,
                 esp_err_to_name(err));
        return claw_event_router_build_session_id(event, buf, buf_size);
    }

    return strlen(buf);
}
