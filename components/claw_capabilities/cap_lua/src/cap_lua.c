/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_lua.h"

#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "cJSON.h"
#include "claw_cap.h"
#include "claw_core.h"
#include "esp_check.h"
#include "esp_log.h"

#include "cap_lua_internal.h"

static const char *TAG = "cap_lua";

typedef struct cap_lua_runtime_cleanup_node {
    cap_lua_runtime_cleanup_fn_t cleanup_fn;
    struct cap_lua_runtime_cleanup_node *next;
} cap_lua_runtime_cleanup_node_t;

typedef struct cap_lua_package_path_dir_node {
    char dir[CAP_LUA_JOB_PATH_MAX];
    struct cap_lua_package_path_dir_node *next;
} cap_lua_package_path_dir_node_t;

char g_cap_lua_base_dir[128];
static cap_lua_module_t s_modules[CAP_LUA_MAX_MODULES];
static cap_lua_package_path_dir_node_t *s_package_path_dirs;
static size_t s_package_path_dir_count;
static size_t s_module_count;
static cap_lua_runtime_cleanup_node_t *s_runtime_cleanups;
static size_t s_runtime_cleanup_count;
static bool s_builtin_modules_registered;
static bool s_module_registration_locked;

static bool cap_lua_text_contains_ci(const char *haystack, const char *needle);

static esp_err_t cap_lua_build_simple_request(const char *string_key,
                                              const char *string_value,
                                              const char *string_key2,
                                              const char *string_value2,
                                              bool has_bool,
                                              const char *bool_key,
                                              bool bool_value,
                                              bool has_number,
                                              const char *number_key,
                                              uint32_t number_value,
                                              char **json_out)
{
    cJSON *root = NULL;

    if (!json_out) {
        return ESP_ERR_INVALID_ARG;
    }
    *json_out = NULL;

    root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }

    if (string_key && string_value && !cJSON_AddStringToObject(root, string_key, string_value)) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    if (string_key2 && string_value2 &&
            !cJSON_AddStringToObject(root, string_key2, string_value2)) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    if (has_bool && bool_key && !cJSON_AddBoolToObject(root, bool_key, bool_value)) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    if (has_number && number_key &&
            !cJSON_AddNumberToObject(root, number_key, (double)number_value)) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    *json_out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return *json_out ? ESP_OK : ESP_ERR_NO_MEM;
}

const char *cap_lua_get_base_dir(void)
{
    return g_cap_lua_base_dir;
}

size_t cap_lua_get_package_path_dir_count(void)
{
    return s_package_path_dir_count;
}

const char *cap_lua_get_package_path_dir(size_t index)
{
    cap_lua_package_path_dir_node_t *node = s_package_path_dirs;
    size_t i = 0;

    while (node) {
        if (i == index) {
            return node->dir;
        }
        node = node->next;
        i++;
    }

    return NULL;
}

esp_err_t cap_lua_add_package_path_dir(const char *dir)
{
    cap_lua_package_path_dir_node_t *node = NULL;
    cap_lua_package_path_dir_node_t **tail = &s_package_path_dirs;

    if (!dir || dir[0] != '/' || strstr(dir, "..") != NULL) {
        ESP_LOGE(TAG, "add_package_path_dir: bad dir=%s", dir ? dir : "(null)");
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(dir) >= CAP_LUA_JOB_PATH_MAX) {
        ESP_LOGE(TAG, "add_package_path_dir: dir too long");
        return ESP_ERR_INVALID_SIZE;
    }

    while (*tail) {
        if (strcmp((*tail)->dir, dir) == 0) {
            return ESP_OK;
        }
        tail = &(*tail)->next;
    }

    node = calloc(1, sizeof(*node));
    if (!node) {
        ESP_LOGE(TAG, "add_package_path_dir: no memory for dir=%s", dir);
        return ESP_ERR_NO_MEM;
    }

    /* Keep a private copy so callers can pass stack-backed path buffers safely. */
    strlcpy(node->dir, dir, sizeof(node->dir));
    *tail = node;
    s_package_path_dir_count++;
    return ESP_OK;
}

bool cap_lua_path_is_valid(const char *path)
{
    size_t base_len;
    size_t path_len;

    if (!path || !g_cap_lua_base_dir[0]) {
        return false;
    }

    base_len = strlen(g_cap_lua_base_dir);
    if (strncmp(path, g_cap_lua_base_dir, base_len) != 0 || path[base_len] != '/') {
        return false;
    }
    if (strstr(path, "..") != NULL) {
        return false;
    }

    path_len = strlen(path);
    return path_len > 4 && strcmp(path + path_len - 4, ".lua") == 0;
}

esp_err_t cap_lua_resolve_path(const char *path, char *resolved, size_t resolved_size)
{
    int written;

    if (!path || !path[0] || !resolved || resolved_size == 0 || !g_cap_lua_base_dir[0]) {
        ESP_LOGE(TAG, "resolve_path: invalid arg");
        return ESP_ERR_INVALID_ARG;
    }

    /* Only accept relative paths from callers; resolved paths remain anchored under the Lua base dir. */
    if (path[0] == '/' || strstr(path, "..") != NULL) {
        ESP_LOGE(TAG, "resolve_path: bad path=%s", path ? path : "(null)");
        return ESP_ERR_INVALID_ARG;
    }

    written = snprintf(resolved, resolved_size, "%s/%s", g_cap_lua_base_dir, path);
    if (written < 0 || (size_t)written >= resolved_size) {
        ESP_LOGE(TAG, "resolve_path: path too long");
        return ESP_ERR_INVALID_SIZE;
    }
    if (!cap_lua_path_is_valid(resolved)) {
        ESP_LOGE(TAG, "resolve_path: invalid resolved path");
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

esp_err_t cap_lua_ensure_base_dir(void)
{
    if (!g_cap_lua_base_dir[0]) {
        return ESP_ERR_INVALID_STATE;
    }
    if (mkdir(g_cap_lua_base_dir, 0755) != 0 && errno != EEXIST) {
        ESP_LOGE(TAG, "Failed to create Lua base dir %s", g_cap_lua_base_dir);
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t cap_lua_build_args_json(cJSON *root, char **args_json_out)
{
    cJSON *args = NULL;
    cJSON *payload = NULL;

    if (!args_json_out) {
        return ESP_ERR_INVALID_ARG;
    }
    *args_json_out = NULL;

    args = cJSON_GetObjectItem(root, "args");
    if (cJSON_IsObject(args) || cJSON_IsArray(args)) {
        payload = cJSON_Duplicate(args, 1);
        if (!payload) {
            return ESP_ERR_NO_MEM;
        }
        *args_json_out = cJSON_PrintUnformatted(payload);
        cJSON_Delete(payload);
        if (!*args_json_out) {
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

static esp_err_t cap_lua_group_init(void)
{
    ESP_RETURN_ON_ERROR(cap_lua_register_builtin_modules(),
                        TAG,
                        "Failed to register builtin Lua modules");
    s_module_registration_locked = true;
    ESP_RETURN_ON_ERROR(cap_lua_runtime_init(), TAG, "Failed to init runtime");
    ESP_RETURN_ON_ERROR(cap_lua_async_init(), TAG, "Failed to init async runner");
    return ESP_OK;
}

size_t cap_lua_get_active_async_job_count(void)
{
    return cap_lua_async_active_count();
}

static esp_err_t cap_lua_group_start(void)
{
    return cap_lua_async_start();
}

static esp_err_t cap_lua_list_scripts_recursive(const char *dir_path,
                                                const char *prefix,
                                                const char *keyword,
                                                char *output,
                                                size_t output_size,
                                                size_t *offset,
                                                int *count)
{
    DIR *dir = NULL;
    struct dirent *entry = NULL;
    size_t base_len;

    if (!dir_path || !output || !offset || !count) {
        return ESP_ERR_INVALID_ARG;
    }

    dir = opendir(dir_path);
    if (!dir) {
        return ESP_FAIL;
    }
    base_len = strlen(g_cap_lua_base_dir);

    while ((entry = readdir(dir)) != NULL && *offset < output_size - 1) {
        char full_path[384];
        const char *relative_path = NULL;
        struct stat st = {0};

        if (entry->d_name[0] == '.') {
            continue;
        }

        if (snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name) >= sizeof(full_path)) {
            continue;
        }
        if (stat(full_path, &st) != 0) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            esp_err_t err = cap_lua_list_scripts_recursive(full_path, prefix, keyword, output, output_size, offset, count);
            if (err != ESP_OK) {
                closedir(dir);
                return err;
            }
            continue;
        }
        if (!cap_lua_path_is_valid(full_path)) {
            continue;
        }
        relative_path = full_path + base_len + 1;
        if (prefix && strncmp(relative_path, prefix, strlen(prefix)) != 0) {
            continue;
        }
        if (keyword && !cap_lua_text_contains_ci(relative_path, keyword)) {
            continue;
        }

        *offset += snprintf(output + *offset, output_size - *offset, "%s\n", relative_path);
        (*count)++;
    }

    closedir(dir);
    return ESP_OK;
}

static esp_err_t cap_lua_ensure_parent_dirs(const char *path)
{
    char dir_path[192];
    char *slash = NULL;
    char *cursor = NULL;
    size_t base_len;

    if (!path || !cap_lua_path_is_valid(path)) {
        return ESP_ERR_INVALID_ARG;
    }

    strlcpy(dir_path, path, sizeof(dir_path));
    slash = strrchr(dir_path, '/');
    if (!slash || slash == dir_path) {
        return ESP_OK;
    }
    *slash = '\0';

    base_len = strlen(g_cap_lua_base_dir);
    cursor = dir_path + base_len + 1;
    while ((slash = strchr(cursor, '/')) != NULL) {
        *slash = '\0';
        if (mkdir(dir_path, 0755) != 0 && errno != EEXIST) {
            return ESP_FAIL;
        }
        *slash = '/';
        cursor = slash + 1;
    }

    if (mkdir(dir_path, 0755) != 0 && errno != EEXIST) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t cap_lua_list_scripts_execute(const char *input_json,
                                              const claw_cap_call_context_t *ctx,
                                              char *output,
                                              size_t output_size)
{
    cJSON *root = NULL;
    const char *prefix = NULL;
    const char *keyword = NULL;
    size_t offset = 0;
    int count = 0;
    esp_err_t err;

    (void)ctx;

    if (!output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    output[0] = '\0';

    root = cJSON_Parse(input_json);
    if (root) {
        cJSON *prefix_item = cJSON_GetObjectItem(root, "prefix");
        cJSON *keyword_item = cJSON_GetObjectItem(root, "keyword");
        if (cJSON_IsString(prefix_item) && prefix_item->valuestring[0]) {
            prefix = prefix_item->valuestring;
        }
        if (cJSON_IsString(keyword_item) && keyword_item->valuestring[0]) {
            keyword = keyword_item->valuestring;
        }
    }

    if (prefix && (prefix[0] == '/' || strstr(prefix, "..") != NULL)) {
        cJSON_Delete(root);
        ESP_LOGE(TAG, "list_scripts: bad prefix=%s", prefix);
        snprintf(output, output_size, "Error: prefix must be a relative path");
        return ESP_ERR_INVALID_ARG;
    }

    err = cap_lua_list_scripts_recursive(g_cap_lua_base_dir, prefix, keyword, output, output_size, &offset, &count);
    if (err != ESP_OK) {
        cJSON_Delete(root);
        ESP_LOGE(TAG, "list_scripts: scan failed");
        snprintf(output, output_size, "Error: cannot list Lua scripts");
        return ESP_FAIL;
    }
    cJSON_Delete(root);
    if (count == 0) {
        snprintf(output, output_size, "(no Lua scripts found)");
    }
    return ESP_OK;
}

static esp_err_t cap_lua_write_script_execute(const char *input_json,
                                              const claw_cap_call_context_t *ctx,
                                              char *output,
                                              size_t output_size)
{
    cJSON *root = NULL;
    const char *path = NULL;
    const char *content = NULL;
    char request_path[128] = {0};
    char resolved_path[192];
    cJSON *overwrite_item = NULL;
    bool overwrite = true;
    struct stat st = {0};
    FILE *file = NULL;
    size_t content_len = 0;

    (void)ctx;

    root = cJSON_Parse(input_json);
    if (!root) {
        ESP_LOGE(TAG, "write_script: invalid json");
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    path = cJSON_GetStringValue(cJSON_GetObjectItem(root, "path"));
    content = cJSON_GetStringValue(cJSON_GetObjectItem(root, "content"));
    overwrite_item = cJSON_GetObjectItem(root, "overwrite");
    if (cJSON_IsBool(overwrite_item)) {
        overwrite = cJSON_IsTrue(overwrite_item);
    }
    if (path && path[0]) {
        strlcpy(request_path, path, sizeof(request_path));
    }

    if (cap_lua_resolve_path(path, resolved_path, sizeof(resolved_path)) != ESP_OK) {
        cJSON_Delete(root);
        ESP_LOGE(TAG, "write_script: bad path=%s", path ? path : "(null)");
        snprintf(output, output_size, "Error: path must be a relative .lua path");
        return ESP_ERR_INVALID_ARG;
    }
    if (!content) {
        cJSON_Delete(root);
        ESP_LOGE(TAG, "write_script: missing content");
        snprintf(output, output_size, "Error: missing content");
        return ESP_ERR_INVALID_ARG;
    }

    content_len = strlen(content);
    if (content_len > CAP_LUA_MAX_SCRIPT_SIZE) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: script exceeds %d bytes", CAP_LUA_MAX_SCRIPT_SIZE);
        return ESP_ERR_INVALID_SIZE;
    }
    if (!overwrite && stat(resolved_path, &st) == 0) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: script already exists: %s", request_path);
        return ESP_ERR_INVALID_STATE;
    }

    if (cap_lua_ensure_base_dir() != ESP_OK) {
        cJSON_Delete(root);
        ESP_LOGE(TAG, "write_script: storage prepare failed");
        snprintf(output, output_size, "Error: failed to prepare Lua script storage");
        return ESP_FAIL;
    }
    if (cap_lua_ensure_parent_dirs(resolved_path) != ESP_OK) {
        cJSON_Delete(root);
        ESP_LOGE(TAG, "write_script: mkdir failed path=%s", request_path);
        snprintf(output, output_size, "Error: failed to prepare parent directory");
        return ESP_FAIL;
    }

    file = fopen(resolved_path, "w");
    if (!file) {
        cJSON_Delete(root);
        ESP_LOGE(TAG, "write_script: open failed path=%s errno=%d", request_path, errno);
        snprintf(output, output_size, "Error: cannot open script for writing: %s", request_path);
        return ESP_FAIL;
    }
    if (fwrite(content, 1, content_len, file) != content_len) {
        fclose(file);
        cJSON_Delete(root);
        ESP_LOGE(TAG, "write_script: write failed path=%s", request_path);
        snprintf(output, output_size, "Error: failed to write script: %s", request_path);
        return ESP_FAIL;
    }

    fclose(file);
    snprintf(output, output_size, "OK: wrote Lua script %s (%d bytes)", request_path, (int)content_len);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t cap_lua_run_script_execute(const char *input_json,
                                            const claw_cap_call_context_t *ctx,
                                            char *output,
                                            size_t output_size)
{
    cJSON *root = NULL;
    const char *path = NULL;
    char resolved_path[192];
    cJSON *timeout_item = NULL;
    char *args_json = NULL;
    uint32_t timeout_ms = 0;
    esp_err_t err;

    (void)ctx;

    root = cJSON_Parse(input_json);
    if (!root) {
        ESP_LOGE(TAG, "run_script: invalid json");
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    path = cJSON_GetStringValue(cJSON_GetObjectItem(root, "path"));
    if (cap_lua_resolve_path(path, resolved_path, sizeof(resolved_path)) != ESP_OK) {
        cJSON_Delete(root);
        ESP_LOGE(TAG, "run_script: bad path=%s", path ? path : "(null)");
        snprintf(output, output_size, "Error: path must be a relative .lua path");
        return ESP_ERR_INVALID_ARG;
    }

    timeout_item = cJSON_GetObjectItem(root, "timeout_ms");
    if (timeout_item && (!cJSON_IsNumber(timeout_item) || timeout_item->valueint <= 0)) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: timeout_ms must be a positive integer");
        return ESP_ERR_INVALID_ARG;
    }
    if (cJSON_IsNumber(timeout_item)) {
        timeout_ms = (uint32_t)timeout_item->valueint;
    }

    err = cap_lua_build_args_json(root, &args_json);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        free(args_json);
        snprintf(output, output_size, "Error: failed to prepare Lua args");
        return err;
    }

    if (timeout_ms == 0) {
        timeout_ms = CAP_LUA_SYNC_DEFAULT_TIMEOUT_MS;
    }
    err = cap_lua_runtime_execute_file(resolved_path,
                                       args_json,
                                       timeout_ms,
                                       NULL,
                                       output,
                                       output_size);
    free(args_json);
    return err;
}

static const char *cap_lua_basename_from_path(const char *path)
{
    const char *slash = NULL;

    if (!path) {
        return NULL;
    }
    slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static esp_err_t cap_lua_run_script_async_execute(const char *input_json,
                                                  const claw_cap_call_context_t *ctx,
                                                  char *output,
                                                  size_t output_size)
{
    cJSON *root = NULL;
    const char *path = NULL;
    const char *name = NULL;
    const char *exclusive = NULL;
    char resolved_path[192];
    cJSON *timeout_item = NULL;
    cJSON *replace_item = NULL;
    char *args_json = NULL;
    char request_path[192] = {0};
    uint32_t timeout_ms = CAP_LUA_ASYNC_DEFAULT_TIMEOUT_MS;
    cap_lua_async_job_t job = {0};
    char job_id[CAP_LUA_JOB_ID_LEN] = {0};
    char err_buf[256] = {0};
    bool replace = false;
    esp_err_t err;

    (void)ctx;

    root = cJSON_Parse(input_json);
    if (!root) {
        ESP_LOGE(TAG, "run_async: invalid json");
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    path = cJSON_GetStringValue(cJSON_GetObjectItem(root, "path"));
    if (cap_lua_resolve_path(path, resolved_path, sizeof(resolved_path)) != ESP_OK) {
        cJSON_Delete(root);
        ESP_LOGE(TAG, "run_async: bad path=%s", path ? path : "(null)");
        snprintf(output, output_size, "Error: path must be a relative .lua path");
        return ESP_ERR_INVALID_ARG;
    }
    strlcpy(request_path, path ? path : resolved_path, sizeof(request_path));

    {
        struct stat script_stat;
        if (stat(resolved_path, &script_stat) != 0) {
            cJSON_Delete(root);
            ESP_LOGE(TAG, "run_async: missing script path=%s errno=%d", request_path, errno);
            snprintf(output, output_size, "Error: script not found: %s (errno=%d)", request_path, errno);
            return ESP_ERR_NOT_FOUND;
        }
    }

    timeout_item = cJSON_GetObjectItem(root, "timeout_ms");
    if (timeout_item && (!cJSON_IsNumber(timeout_item) || timeout_item->valueint < 0)) {
        cJSON_Delete(root);
        snprintf(output, output_size,
                 "Error: timeout_ms must be a non-negative integer (0 = until cancelled)");
        return ESP_ERR_INVALID_ARG;
    }
    if (cJSON_IsNumber(timeout_item)) {
        timeout_ms = (uint32_t)timeout_item->valueint;
    }

    name = cJSON_GetStringValue(cJSON_GetObjectItem(root, "name"));
    exclusive = cJSON_GetStringValue(cJSON_GetObjectItem(root, "exclusive"));
    replace_item = cJSON_GetObjectItem(root, "replace");
    if (cJSON_IsBool(replace_item)) {
        replace = cJSON_IsTrue(replace_item);
    }

    strlcpy(job.path, resolved_path, sizeof(job.path));
    if (name && name[0]) {
        strlcpy(job.name, name, sizeof(job.name));
    } else {
        const char *base = cap_lua_basename_from_path(resolved_path);
        if (base) {
            strlcpy(job.name, base, sizeof(job.name));
        }
    }
    if (exclusive && exclusive[0]) {
        strlcpy(job.exclusive, exclusive, sizeof(job.exclusive));
    }

    err = cap_lua_build_args_json(root, &args_json);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        free(args_json);
        snprintf(output, output_size, "Error: failed to prepare Lua args");
        return err;
    }

    job.args_json = args_json;
    job.timeout_ms = timeout_ms;
    job.replace = replace;
    job.created_at = time(NULL);
    err = cap_lua_async_submit(&job, job_id, sizeof(job_id), err_buf, sizeof(err_buf));
    free(args_json);
    if (err != ESP_OK) {
        if (err_buf[0]) {
            snprintf(output, output_size, "Error: %s", err_buf);
        } else if (err == ESP_ERR_INVALID_STATE) {
            snprintf(output, output_size, "Error: Lua async runner is not ready");
        } else {
            snprintf(output, output_size, "Error: failed to queue async Lua job (%s)",
                     esp_err_to_name(err));
        }
        return err;
    }

    cap_lua_job_status_t settle_status = CAP_LUA_JOB_RUNNING;
    char settle_summary[128] = {0};
    cap_lua_async_wait_settle(job_id, 150, &settle_status, settle_summary, sizeof(settle_summary));

    const char *status_label = cap_lua_job_status_name(settle_status);

    if (settle_status == CAP_LUA_JOB_FAILED || settle_status == CAP_LUA_JOB_TIMEOUT ||
        settle_status == CAP_LUA_JOB_STOPPED) {
        snprintf(output, output_size,
                 "Lua job %s (name=%s) ended early with status=%s. summary: %s",
                 job_id,
                 job.name[0] ? job.name : "(unnamed)",
                 status_label,
                 settle_summary[0] ? settle_summary : "(none)");
        return ESP_OK;
    }

    snprintf(output, output_size,
             "Started Lua job %s (name=%s, exclusive=%s, timeout_ms=%u%s, status=%s) for %s",
             job_id,
             job.name[0] ? job.name : "(unnamed)",
             job.exclusive[0] ? job.exclusive : "none",
             (unsigned)timeout_ms,
             timeout_ms == 0 ? " [until cancelled]" : "",
             status_label,
             request_path);
    return ESP_OK;
}

static esp_err_t cap_lua_stop_async_job_execute(const char *input_json,
                                                const claw_cap_call_context_t *ctx,
                                                char *output,
                                                size_t output_size)
{
    cJSON *root = NULL;
    const char *target = NULL;
    cJSON *wait_item = NULL;
    uint32_t wait_ms = 0;
    esp_err_t err;

    (void)ctx;

    root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }
    target = cJSON_GetStringValue(cJSON_GetObjectItem(root, "job_id"));
    if (!target || !target[0]) {
        target = cJSON_GetStringValue(cJSON_GetObjectItem(root, "name"));
    }
    if (!target || !target[0]) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: provide either 'job_id' or 'name'");
        return ESP_ERR_INVALID_ARG;
    }
    wait_item = cJSON_GetObjectItem(root, "wait_ms");
    if (cJSON_IsNumber(wait_item) && wait_item->valueint > 0) {
        wait_ms = (uint32_t)wait_item->valueint;
    }

    err = cap_lua_async_stop_job(target, wait_ms, output, output_size);
    cJSON_Delete(root);
    return err;
}

static esp_err_t cap_lua_stop_all_async_jobs_execute(const char *input_json,
                                                     const claw_cap_call_context_t *ctx,
                                                     char *output,
                                                     size_t output_size)
{
    cJSON *root = NULL;
    const char *exclusive = NULL;
    cJSON *wait_item = NULL;
    uint32_t wait_ms = 0;
    esp_err_t err;

    (void)ctx;

    if (input_json && input_json[0]) {
        root = cJSON_Parse(input_json);
        if (!root) {
            snprintf(output, output_size, "Error: invalid JSON");
            return ESP_ERR_INVALID_ARG;
        }
        exclusive = cJSON_GetStringValue(cJSON_GetObjectItem(root, "exclusive"));
        wait_item = cJSON_GetObjectItem(root, "wait_ms");
        if (cJSON_IsNumber(wait_item) && wait_item->valueint > 0) {
            wait_ms = (uint32_t)wait_item->valueint;
        }
    }

    err = cap_lua_async_stop_all_jobs(exclusive, wait_ms, output, output_size);
    cJSON_Delete(root);
    return err;
}

static esp_err_t cap_lua_list_async_jobs_execute(const char *input_json,
                                                 const claw_cap_call_context_t *ctx,
                                                 char *output,
                                                 size_t output_size)
{
    cJSON *root = NULL;
    const char *status = NULL;
    esp_err_t err;

    (void)ctx;

    root = cJSON_Parse(input_json);
    if (root) {
        status = cJSON_GetStringValue(cJSON_GetObjectItem(root, "status"));
        if (status &&
                strcmp(status, "all") != 0 &&
                strcmp(status, "queued") != 0 &&
                strcmp(status, "running") != 0 &&
                strcmp(status, "done") != 0 &&
                strcmp(status, "failed") != 0 &&
                strcmp(status, "timeout") != 0 &&
                strcmp(status, "stopped") != 0) {
            cJSON_Delete(root);
            snprintf(output,
                     output_size,
                     "Error: status must be one of all, queued, running, done, failed, timeout, stopped");
            return ESP_ERR_INVALID_ARG;
        }
    }

    err = cap_lua_async_list_jobs(status, output, output_size);
    cJSON_Delete(root);
    return err;
}

static esp_err_t cap_lua_get_async_job_execute(const char *input_json,
                                               const claw_cap_call_context_t *ctx,
                                               char *output,
                                               size_t output_size)
{
    cJSON *root = NULL;
    const char *job_id = NULL;
    esp_err_t err;

    (void)ctx;

    root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    job_id = cJSON_GetStringValue(cJSON_GetObjectItem(root, "job_id"));
    if (!job_id || !job_id[0]) {
        job_id = cJSON_GetStringValue(cJSON_GetObjectItem(root, "name"));
    }
    if (!job_id || !job_id[0]) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: provide either 'job_id' or 'name'");
        return ESP_ERR_INVALID_ARG;
    }

    err = cap_lua_async_get_job(job_id, output, output_size);
    cJSON_Delete(root);
    return err;
}

static const claw_cap_descriptor_t s_lua_descriptors[] = {
    {
        .id = "lua_list_scripts",
        .name = "lua_list_scripts",
        .family = "automation",
        .description = "List Lua script relative paths, filtered by an optional relative-path prefix and case-insensitive relative-path keyword match.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{\"prefix\":{\"type\":\"string\"},\"keyword\":{\"type\":\"string\"}}}",
        .execute = cap_lua_list_scripts_execute,
    },
    {
        .id = "lua_write_script",
        .name = "lua_write_script",
        .family = "automation",
        .description = "Write a Lua script.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"content\":{\"type\":\"string\"},\"overwrite\":{\"type\":\"boolean\"}},\"required\":[\"path\",\"content\"]}",
        .execute = cap_lua_write_script_execute,
    },
    {
        .id = "lua_run_script",
        .name = "lua_run_script",
        .family = "automation",
        .description = "Run a Lua script synchronously with optional args and timeout.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"args\":{\"anyOf\":[{\"type\":\"object\",\"properties\":{}},{\"type\":\"array\",\"items\":{}}]},\"timeout_ms\":{\"type\":\"integer\"}},\"required\":[\"path\"]}",
        .execute = cap_lua_run_script_execute,
    },
    {
        .id = "lua_run_script_async",
        .name = "lua_run_script_async",
        .family = "automation",
        .description =
        "Run a Lua script asynchronously, returns a job id. timeout_ms=0 "
        "means run until cancelled (default). Use 'name' to label, 'exclusive' "
        "for mutex groups (e.g. 'display'), 'replace':true to take over a "
        "conflicting slot.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"args\":{\"anyOf\":[{\"type\":\"object\",\"properties\":{}},{\"type\":\"array\",\"items\":{}}]},\"timeout_ms\":{\"type\":\"integer\",\"minimum\":0},\"name\":{\"type\":\"string\"},\"exclusive\":{\"type\":\"string\"},\"replace\":{\"type\":\"boolean\"}},\"required\":[\"path\"]}",
        .execute = cap_lua_run_script_async_execute,
    },
    {
        .id = "lua_list_async_jobs",
        .name = "lua_list_async_jobs",
        .family = "automation",
        .description = "List Lua async jobs by optional status filter.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{\"status\":{\"type\":\"string\",\"enum\":[\"all\",\"queued\",\"running\",\"done\",\"failed\",\"timeout\",\"stopped\"]}}}",
        .execute = cap_lua_list_async_jobs_execute,
    },
    {
        .id = "lua_get_async_job",
        .name = "lua_get_async_job",
        .family = "automation",
        .description = "Get the status and summary for a Lua async job by job_id or name.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{\"job_id\":{\"type\":\"string\"},\"name\":{\"type\":\"string\"}}}",
        .execute = cap_lua_get_async_job_execute,
    },
    {
        .id = "lua_stop_async_job",
        .name = "lua_stop_async_job",
        .family = "automation",
        .description =
        "Stop a running Lua async job by job_id or name. MUST be called whenever the user asks "
        "to stop, cancel, quit or close an async script; replying without calling this leaves "
        "the job running. Cooperative; default wait 2000 ms.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{\"job_id\":{\"type\":\"string\"},\"name\":{\"type\":\"string\"},\"wait_ms\":{\"type\":\"integer\",\"minimum\":1}}}",
        .execute = cap_lua_stop_async_job_execute,
    },
    {
        .id = "lua_stop_all_async_jobs",
        .name = "lua_stop_all_async_jobs",
        .family = "automation",
        .description =
        "Stop all running Lua async jobs, optionally filtered by exclusive group "
        "(e.g. exclusive='display'). MUST be called when the user asks to clear the screen, "
        "stop everything or cancel all background scripts.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{\"exclusive\":{\"type\":\"string\"},\"wait_ms\":{\"type\":\"integer\",\"minimum\":1}}}",
        .execute = cap_lua_stop_all_async_jobs_execute,
    },
};

static const claw_cap_group_t s_lua_group = {
    .group_id = "cap_lua",
    .descriptors = s_lua_descriptors,
    .descriptor_count = sizeof(s_lua_descriptors) / sizeof(s_lua_descriptors[0]),
    .group_init = cap_lua_group_init,
    .group_start = cap_lua_group_start,
};

esp_err_t cap_lua_register_group(const char *base_dir)
{
    if (!base_dir || base_dir[0] != '/') {
        ESP_LOGE(TAG, "register_group: bad base_dir");
        return ESP_ERR_INVALID_ARG;
    }
    if (claw_cap_group_exists(s_lua_group.group_id)) {
        return ESP_OK;
    }

    strlcpy(g_cap_lua_base_dir, base_dir, sizeof(g_cap_lua_base_dir));
    return claw_cap_register_group(&s_lua_group);
}

esp_err_t cap_lua_list_scripts(const char *prefix, const char *keyword, char *output, size_t output_size)
{
    char *input_json = NULL;
    esp_err_t err;

    err = cap_lua_build_simple_request("prefix",
                                       prefix,
                                       "keyword",
                                       keyword,
                                       false,
                                       NULL,
                                       false,
                                       false,
                                       NULL,
                                       0,
                                       &input_json);
    if (err != ESP_OK) {
        return err;
    }

    err = cap_lua_list_scripts_execute(input_json ? input_json : "{}",
                                       NULL,
                                       output,
                                       output_size);
    free(input_json);
    return err;
}

esp_err_t cap_lua_write_script(const char *path,
                               const char *content,
                               bool overwrite,
                               char *output,
                               size_t output_size)
{
    cJSON *root = NULL;
    char *input_json = NULL;
    esp_err_t err = ESP_OK;

    if (!path || !content) {
        return ESP_ERR_INVALID_ARG;
    }

    root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    if (!cJSON_AddStringToObject(root, "path", path) ||
            !cJSON_AddStringToObject(root, "content", content) ||
            !cJSON_AddBoolToObject(root, "overwrite", overwrite)) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    input_json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!input_json) {
        return ESP_ERR_NO_MEM;
    }

    err = cap_lua_write_script_execute(input_json, NULL, output, output_size);
    free(input_json);
    return err;
}

esp_err_t cap_lua_run_script(const char *path,
                             const char *args_json,
                             uint32_t timeout_ms,
                             char *output,
                             size_t output_size)
{
    cJSON *root = NULL;
    cJSON *args = NULL;
    char *input_json = NULL;
    esp_err_t err = ESP_OK;

    if (!path) {
        return ESP_ERR_INVALID_ARG;
    }

    root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    if (!cJSON_AddStringToObject(root, "path", path)) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    if (args_json && args_json[0]) {
        args = cJSON_Parse(args_json);
        if (!args || (!cJSON_IsObject(args) && !cJSON_IsArray(args))) {
            cJSON_Delete(args);
            cJSON_Delete(root);
            return ESP_ERR_INVALID_ARG;
        }
        cJSON_AddItemToObject(root, "args", args);
    }
    if (timeout_ms > 0 && !cJSON_AddNumberToObject(root, "timeout_ms", (double)timeout_ms)) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    input_json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!input_json) {
        return ESP_ERR_NO_MEM;
    }

    err = cap_lua_run_script_execute(input_json, NULL, output, output_size);
    free(input_json);
    return err;
}

esp_err_t cap_lua_run_script_async(const char *path,
                                   const char *args_json,
                                   uint32_t timeout_ms,
                                   const char *name,
                                   const char *exclusive,
                                   bool replace,
                                   char *output,
                                   size_t output_size)
{
    cJSON *root = NULL;
    cJSON *args = NULL;
    char *input_json = NULL;
    esp_err_t err = ESP_OK;

    if (!path) {
        return ESP_ERR_INVALID_ARG;
    }

    root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    if (!cJSON_AddStringToObject(root, "path", path)) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    if (args_json && args_json[0]) {
        args = cJSON_Parse(args_json);
        if (!args || (!cJSON_IsObject(args) && !cJSON_IsArray(args))) {
            cJSON_Delete(args);
            cJSON_Delete(root);
            return ESP_ERR_INVALID_ARG;
        }
        cJSON_AddItemToObject(root, "args", args);
    }
    if (!cJSON_AddNumberToObject(root, "timeout_ms", (double)timeout_ms)) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    if (name && name[0] && !cJSON_AddStringToObject(root, "name", name)) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    if (exclusive && exclusive[0] && !cJSON_AddStringToObject(root, "exclusive", exclusive)) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    if (replace && !cJSON_AddBoolToObject(root, "replace", true)) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    input_json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!input_json) {
        return ESP_ERR_NO_MEM;
    }

    err = cap_lua_run_script_async_execute(input_json, NULL, output, output_size);
    free(input_json);
    return err;
}

esp_err_t cap_lua_stop_job(const char *id_or_name,
                           uint32_t wait_ms,
                           char *output,
                           size_t output_size)
{
    return cap_lua_async_stop_job(id_or_name, wait_ms, output, output_size);
}

esp_err_t cap_lua_stop_all_jobs(const char *exclusive_filter,
                                uint32_t wait_ms,
                                char *output,
                                size_t output_size)
{
    return cap_lua_async_stop_all_jobs(exclusive_filter, wait_ms, output, output_size);
}

static esp_err_t cap_lua_async_jobs_collect(const claw_core_request_t *request,
                                            claw_core_context_t *out_context,
                                            void *user_ctx)
{
    cap_lua_async_job_snapshot_t snapshots[CAP_LUA_ASYNC_MAX_CONCURRENT];
    size_t count;
    char *content = NULL;
    size_t cap = 1024;
    size_t off = 0;
    time_t now;

    (void)request;
    (void)user_ctx;

    if (!out_context) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out_context, 0, sizeof(*out_context));

    count = cap_lua_async_collect_active_snapshots(snapshots,
                                                   sizeof(snapshots) / sizeof(snapshots[0]));
    if (count == 0) {
        return ESP_ERR_NOT_FOUND;
    }

    content = calloc(1, cap);
    if (!content) {
        return ESP_ERR_NO_MEM;
    }

    now = time(NULL);
    off += snprintf(content + off, cap - off,
                    "Active Lua async jobs (%u, max %u):\n",
                    (unsigned)count,
                    (unsigned)CAP_LUA_ASYNC_MAX_CONCURRENT);
    for (size_t i = 0; i < count && off < cap - 1; i++) {
        long runtime_s = (long)(now - (snapshots[i].started_at ? snapshots[i].started_at
                                                                : snapshots[i].created_at));
        if (runtime_s < 0) {
            runtime_s = 0;
        }
        const char *status_name = NULL;
        switch (snapshots[i].status) {
        case CAP_LUA_JOB_QUEUED:  status_name = "queued"; break;
        case CAP_LUA_JOB_RUNNING: status_name = "running"; break;
        default:                  status_name = "active"; break;
        }
        int written = snprintf(content + off, cap - off,
                               "- id=%s name=%s exclusive=%s status=%s runtime=%lds path=%s\n",
                               snapshots[i].job_id,
                               snapshots[i].name[0] ? snapshots[i].name : "(unnamed)",
                               snapshots[i].exclusive[0] ? snapshots[i].exclusive : "none",
                               status_name,
                               runtime_s,
                               snapshots[i].path);
        if (written < 0 || (size_t)written >= cap - off) {
            break;
        }
        off += (size_t)written;
    }

    if (off < cap - 1) {
        off += snprintf(content + off, cap - off,
                        "Listing only; jobs keep running until you call lua_stop_async_job, "
                        "lua_stop_all_async_jobs, or lua_run_script_async with replace:true. "
                        "Never claim a job is stopped/switched without calling one of these.\n");
    }

    out_context->kind = CLAW_CORE_CONTEXT_KIND_SYSTEM_PROMPT;
    out_context->content = content;
    return ESP_OK;
}

const claw_core_context_provider_t cap_lua_async_jobs_provider = {
    .name = "Lua Async Jobs",
    .collect = cap_lua_async_jobs_collect,
    .user_ctx = NULL,
};

/* Case-insensitive substring search (the LLM's casing is not stable). */
static bool cap_lua_text_contains_ci(const char *haystack, const char *needle)
{
    if (!haystack || !needle || !needle[0]) {
        return false;
    }
    size_t hlen = strlen(haystack);
    size_t nlen = strlen(needle);
    if (nlen > hlen) {
        return false;
    }
    for (size_t i = 0; i + nlen <= hlen; i++) {
        if (strncasecmp(haystack + i, needle, nlen) == 0) {
            return true;
        }
    }
    return false;
}

void cap_lua_honesty_observe_completion(const claw_core_completion_summary_t *summary,
                                        void *user_ctx)
{
    (void)user_ctx;
    if (!summary || !summary->final_text || !summary->final_text[0]) {
        return;
    }
    /* Honesty check is meaningful only when the model could see active jobs. */
    const char *providers = summary->context_providers_csv ? summary->context_providers_csv : "";
    if (!strstr(providers, "Lua Async Jobs")) {
        return;
    }
    const char *tools = summary->tool_calls_csv ? summary->tool_calls_csv : "";
    if (strstr(tools, "lua_stop_async_job") ||
        strstr(tools, "lua_stop_all_async_jobs") ||
        strstr(tools, "lua_run_script_async")) {
        return;
    }
    static const char *const claim_keywords[] = {
        "已取消", "已停止", "已关闭", "已清除", "取消了", "停止了", "关掉了", "关闭了",
        "stopped", "cancelled", "canceled", "cleared",
    };
    bool claims_stop = false;
    for (size_t i = 0; i < sizeof(claim_keywords) / sizeof(claim_keywords[0]); i++) {
        if (cap_lua_text_contains_ci(summary->final_text, claim_keywords[i])) {
            claims_stop = true;
            break;
        }
    }
    if (!claims_stop) {
        return;
    }
    /* Truncate the reply so the warning stays readable in the log. */
    char snippet[96] = {0};
    strlcpy(snippet, summary->final_text, sizeof(snippet));
    ESP_LOGW(TAG,
             "honesty: request=%" PRIu32
             " reply claims stop/cancel but no lua_stop_* tool was called this turn"
             " (providers=[%s] tools=[%s] reply=%.80s%s)",
             summary->request_id,
             providers,
             tools[0] ? tools : "(none)",
             snippet,
             strlen(summary->final_text) > sizeof(snippet) - 1 ? "..." : "");
}

esp_err_t cap_lua_list_jobs(const char *status, char *output, size_t output_size)
{
    char *input_json = NULL;
    esp_err_t err;

    err = cap_lua_build_simple_request("status",
                                       status,
                                       NULL,
                                       NULL,
                                       false,
                                       NULL,
                                       false,
                                       false,
                                       NULL,
                                       0,
                                       &input_json);
    if (err != ESP_OK) {
        return err;
    }

    err = cap_lua_list_async_jobs_execute(input_json ? input_json : "{}",
                                          NULL,
                                          output,
                                          output_size);
    free(input_json);
    return err;
}

esp_err_t cap_lua_get_job(const char *job_id, char *output, size_t output_size)
{
    char *input_json = NULL;
    esp_err_t err;

    err = cap_lua_build_simple_request("job_id",
                                       job_id,
                                       NULL,
                                       NULL,
                                       false,
                                       NULL,
                                       false,
                                       false,
                                       NULL,
                                       0,
                                       &input_json);
    if (err != ESP_OK) {
        return err;
    }

    err = cap_lua_get_async_job_execute(input_json ? input_json : "{}",
                                        NULL,
                                        output,
                                        output_size);
    free(input_json);
    return err;
}

esp_err_t cap_lua_register_module(const char *name, lua_CFunction open_fn)
{
    size_t i;

    if (!name || !name[0] || !open_fn) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_module_registration_locked) {
        return ESP_ERR_INVALID_STATE;
    }

    for (i = 0; i < s_module_count; i++) {
        if (strcmp(s_modules[i].name, name) == 0) {
            return ESP_ERR_INVALID_STATE;
        }
    }

    if (s_module_count >= CAP_LUA_MAX_MODULES) {
        ESP_LOGE(TAG,
                 "Lua module registry full: tried to add '%s' with %u/%u slots used",
                 name,
                 (unsigned)s_module_count,
                 (unsigned)CAP_LUA_MAX_MODULES);
        return ESP_ERR_NO_MEM;
    }

    s_modules[s_module_count].name = name;
    s_modules[s_module_count].open_fn = open_fn;
    s_module_count++;
    return ESP_OK;
}

esp_err_t cap_lua_register_modules(const cap_lua_module_t *modules, size_t count)
{
    size_t i;
    esp_err_t err;

    if (!modules || count == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    for (i = 0; i < count; i++) {
        err = cap_lua_register_module(modules[i].name, modules[i].open_fn);
        if (err != ESP_OK) {
            return err;
        }
    }

    return ESP_OK;
}

esp_err_t cap_lua_register_runtime_cleanup(cap_lua_runtime_cleanup_fn_t cleanup_fn)
{
    cap_lua_runtime_cleanup_node_t *node = NULL;
    cap_lua_runtime_cleanup_node_t *it = NULL;

    if (!cleanup_fn) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_module_registration_locked) {
        return ESP_ERR_INVALID_STATE;
    }

    for (it = s_runtime_cleanups; it != NULL; it = it->next) {
        if (it->cleanup_fn == cleanup_fn) {
            return ESP_ERR_INVALID_STATE;
        }
    }

    node = calloc(1, sizeof(*node));
    if (!node) {
        return ESP_ERR_NO_MEM;
    }

    node->cleanup_fn = cleanup_fn;
    node->next = s_runtime_cleanups;
    s_runtime_cleanups = node;
    s_runtime_cleanup_count++;
    return ESP_OK;
}

esp_err_t cap_lua_register_builtin_modules(void)
{
    s_builtin_modules_registered = true;
    return ESP_OK;
}

size_t cap_lua_get_module_count(void)
{
    return s_module_count;
}

const cap_lua_module_t *cap_lua_get_module(size_t index)
{
    if (index >= s_module_count) {
        return NULL;
    }

    return &s_modules[index];
}

size_t cap_lua_get_runtime_cleanup_count(void)
{
    return s_runtime_cleanup_count;
}

cap_lua_runtime_cleanup_fn_t cap_lua_get_runtime_cleanup(size_t index)
{
    size_t i = 0;
    cap_lua_runtime_cleanup_node_t *it = s_runtime_cleanups;

    while (it != NULL) {
        if (i == index) {
            return it->cleanup_fn;
        }
        i++;
        it = it->next;
    }

    return NULL;
}
