/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_files.h"

#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cJSON.h"
#include "claw_cap.h"
#include "esp_log.h"

static const char *TAG = "cap_files";

#define CAP_FILES_MAX_FILE_SIZE (32 * 1024)

static char s_files_base_dir[128] = {0};

static bool cap_files_path_is_valid(const char *path)
{
    size_t base_len;

    if (!path || !path[0]) {
        return false;
    }
    if (s_files_base_dir[0] == '\0') {
        return false;
    }

    if (strstr(path, "..") != NULL) {
        return false;
    }

    base_len = strlen(s_files_base_dir);
    if (strncmp(path, s_files_base_dir, base_len) != 0) {
        return false;
    }

    return path[base_len] == '\0' || path[base_len] == '/';
}

static esp_err_t cap_files_resolve_path(const char *path, char *resolved, size_t resolved_size)
{
    int written;

    if (!path || !path[0] || !resolved || resolved_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_files_base_dir[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }

    if (path[0] == '/') {
        if (!cap_files_path_is_valid(path)) {
            return ESP_ERR_INVALID_ARG;
        }
        strlcpy(resolved, path, resolved_size);
        return ESP_OK;
    }

    if (strstr(path, "..") != NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    written = snprintf(resolved, resolved_size, "%s/%s", s_files_base_dir, path);
    if (written < 0 || (size_t)written >= resolved_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (!cap_files_path_is_valid(resolved)) {
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

static esp_err_t cap_files_ensure_dir(const char *path)
{
    struct stat st = {0};

    if (stat(path, &st) == 0) {
        return S_ISDIR(st.st_mode) ? ESP_OK : ESP_FAIL;
    }

    if (mkdir(path, 0755) != 0 && errno != EEXIST) {
        ESP_LOGE(TAG, "mkdir failed for %s: errno=%d", path, errno);
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t cap_files_ensure_parent_dirs(const char *path)
{
    char dir[256];
    char *slash = NULL;
    char *cursor = NULL;
    size_t base_len;

    if (!cap_files_path_is_valid(path)) {
        return ESP_ERR_INVALID_ARG;
    }

    strlcpy(dir, path, sizeof(dir));
    slash = strrchr(dir, '/');
    if (!slash) {
        return ESP_OK;
    }

    base_len = strlen(s_files_base_dir);
    if ((size_t)(slash - dir) <= base_len) {
        return cap_files_ensure_dir(s_files_base_dir);
    }
    *slash = '\0';

    if (cap_files_ensure_dir(s_files_base_dir) != ESP_OK) {
        return ESP_FAIL;
    }

    cursor = dir + base_len + 1;
    while (*cursor) {
        if (*cursor == '/') {
            *cursor = '\0';
            if (cap_files_ensure_dir(dir) != ESP_OK) {
                *cursor = '/';
                return ESP_FAIL;
            }
            *cursor = '/';
        }
        cursor++;
    }

    return cap_files_ensure_dir(dir);
}

static esp_err_t cap_files_list_recursive(const char *dir_path,
                                          const char *prefix,
                                          char *output,
                                          size_t output_size,
                                          size_t *offset,
                                          int *count)
{
    DIR *dir = NULL;
    struct dirent *entry = NULL;

    dir = opendir(dir_path);
    if (!dir) {
        return ESP_FAIL;
    }

    while ((entry = readdir(dir)) != NULL) {
        char full_path[256];
        struct stat st = {0};

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        if (snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name) >= (int)sizeof(full_path)) {
            closedir(dir);
            return ESP_ERR_INVALID_SIZE;
        }

        if (!cap_files_path_is_valid(full_path)) {
            continue;
        }

        if (stat(full_path, &st) != 0) {
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            esp_err_t err = cap_files_list_recursive(full_path,
                                                     prefix,
                                                     output,
                                                     output_size,
                                                     offset,
                                                     count);
            if (err != ESP_OK) {
                closedir(dir);
                return err;
            }
            continue;
        }

        if (prefix && strncmp(full_path, prefix, strlen(prefix)) != 0) {
            continue;
        }

        if (*offset < output_size - 1) {
            int written = snprintf(output + *offset, output_size - *offset, "%s\n", full_path);
            if (written < 0) {
                closedir(dir);
                return ESP_FAIL;
            }
            if ((size_t)written >= output_size - *offset) {
                *offset = output_size - 1;
            } else {
                *offset += (size_t)written;
            }
        }
        (*count)++;
    }

    closedir(dir);
    return ESP_OK;
}

static esp_err_t cap_files_copy_file_internal(const char *src_path, const char *dst_path)
{
    FILE *src = NULL;
    FILE *dst = NULL;
    uint8_t buffer[1024];
    struct stat st = {0};
    esp_err_t err = ESP_OK;

    if (!cap_files_path_is_valid(src_path) || !cap_files_path_is_valid(dst_path)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strcmp(src_path, dst_path) == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (stat(src_path, &st) != 0 || !S_ISREG(st.st_mode)) {
        return ESP_ERR_NOT_FOUND;
    }

    if (cap_files_ensure_parent_dirs(dst_path) != ESP_OK) {
        return ESP_FAIL;
    }

    src = fopen(src_path, "rb");
    if (!src) {
        return ESP_FAIL;
    }

    dst = fopen(dst_path, "wb");
    if (!dst) {
        fclose(src);
        return ESP_FAIL;
    }

    while (!feof(src)) {
        size_t read_size = fread(buffer, 1, sizeof(buffer), src);

        if (read_size > 0 && fwrite(buffer, 1, read_size, dst) != read_size) {
            err = ESP_FAIL;
            break;
        }
        if (ferror(src)) {
            err = ESP_FAIL;
            break;
        }
    }

    fclose(dst);
    fclose(src);

    if (err != ESP_OK) {
        unlink(dst_path);
    }

    return err;
}

static esp_err_t cap_files_read_file_execute(const char *input_json,
                                             const claw_cap_call_context_t *ctx,
                                             char *output,
                                             size_t output_size)
{
    cJSON *root = NULL;
    const char *path = NULL;
    char resolved_path[256];
    struct stat st = {0};
    FILE *file = NULL;
    size_t max_read;
    size_t read_size;

    (void)ctx;

    root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    path = cJSON_GetStringValue(cJSON_GetObjectItem(root, "path"));
    if (cap_files_resolve_path(path, resolved_path, sizeof(resolved_path)) != ESP_OK) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: path must stay under %s", s_files_base_dir);
        return ESP_ERR_INVALID_ARG;
    }

    max_read = output_size - 1;
    if (max_read > CAP_FILES_MAX_FILE_SIZE) {
        max_read = CAP_FILES_MAX_FILE_SIZE;
    }

    if (stat(resolved_path, &st) != 0 || !S_ISREG(st.st_mode)) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: file not found: %s", resolved_path);
        return ESP_ERR_NOT_FOUND;
    }
    if (st.st_size > (off_t)max_read) {
        ESP_LOGE(TAG, "read_file rejected oversized file %s: %ld > %u bytes",
                 resolved_path, (long)st.st_size, (unsigned)max_read);
        cJSON_Delete(root);
        snprintf(output,
                 output_size,
                 "Error: file too large: %s (%ld bytes > %u bytes max)",
                 resolved_path,
                 (long)st.st_size,
                 (unsigned)max_read);
        return ESP_ERR_INVALID_SIZE;
    }

    file = fopen(resolved_path, "rb");
    if (!file) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: file not found: %s", resolved_path);
        return ESP_ERR_NOT_FOUND;
    }

    read_size = fread(output, 1, max_read, file);
    if (ferror(file)) {
        fclose(file);
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: failed to read file: %s", resolved_path);
        return ESP_FAIL;
    }
    output[read_size] = '\0';
    fclose(file);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t cap_files_write_file_execute(const char *input_json,
                                              const claw_cap_call_context_t *ctx,
                                              char *output,
                                              size_t output_size)
{
    cJSON *root = NULL;
    const char *path = NULL;
    const char *content = NULL;
    char resolved_path[256];
    FILE *file = NULL;
    size_t content_len;
    size_t written;

    (void)ctx;

    root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    path = cJSON_GetStringValue(cJSON_GetObjectItem(root, "path"));
    content = cJSON_GetStringValue(cJSON_GetObjectItem(root, "content"));
    if (cap_files_resolve_path(path, resolved_path, sizeof(resolved_path)) != ESP_OK) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: path must stay under %s", s_files_base_dir);
        return ESP_ERR_INVALID_ARG;
    }
    if (!content) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: missing content");
        return ESP_ERR_INVALID_ARG;
    }

    if (cap_files_ensure_parent_dirs(resolved_path) != ESP_OK) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: failed to create parent directories for %s", resolved_path);
        return ESP_FAIL;
    }

    file = fopen(resolved_path, "wb");
    if (!file) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: cannot open file for writing: %s", resolved_path);
        return ESP_FAIL;
    }

    content_len = strlen(content);
    written = fwrite(content, 1, content_len, file);
    fclose(file);
    cJSON_Delete(root);

    if (written != content_len) {
        snprintf(output, output_size, "Error: wrote %d of %d bytes to %s",
                 (int)written,
                 (int)content_len,
                 resolved_path);
        return ESP_FAIL;
    }

    snprintf(output, output_size, "OK: wrote %d bytes to %s", (int)written, resolved_path);
    return ESP_OK;
}

static esp_err_t cap_files_delete_file_execute(const char *input_json,
                                               const claw_cap_call_context_t *ctx,
                                               char *output,
                                               size_t output_size)
{
    cJSON *root = NULL;
    const char *path = NULL;
    char resolved_path[256];
    struct stat st = {0};

    (void)ctx;

    root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    path = cJSON_GetStringValue(cJSON_GetObjectItem(root, "path"));
    if (cap_files_resolve_path(path, resolved_path, sizeof(resolved_path)) != ESP_OK) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: path must stay under %s", s_files_base_dir);
        return ESP_ERR_INVALID_ARG;
    }

    if (stat(resolved_path, &st) != 0 || !S_ISREG(st.st_mode)) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: file not found: %s", resolved_path);
        return ESP_ERR_NOT_FOUND;
    }

    if (unlink(resolved_path) != 0) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: failed to delete file: %s", resolved_path);
        return ESP_FAIL;
    }

    cJSON_Delete(root);
    snprintf(output, output_size, "OK: deleted %s", resolved_path);
    return ESP_OK;
}

static esp_err_t cap_files_copy_file_execute(const char *input_json,
                                             const claw_cap_call_context_t *ctx,
                                             char *output,
                                             size_t output_size)
{
    cJSON *root = NULL;
    const char *src_path = NULL;
    const char *dst_path = NULL;
    char resolved_src_path[256];
    char resolved_dst_path[256];
    esp_err_t err;

    (void)ctx;

    root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    src_path = cJSON_GetStringValue(cJSON_GetObjectItem(root, "src_path"));
    dst_path = cJSON_GetStringValue(cJSON_GetObjectItem(root, "dst_path"));
    if (cap_files_resolve_path(src_path, resolved_src_path, sizeof(resolved_src_path)) != ESP_OK
        || cap_files_resolve_path(dst_path, resolved_dst_path, sizeof(resolved_dst_path)) != ESP_OK) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: source and destination must stay under %s", s_files_base_dir);
        return ESP_ERR_INVALID_ARG;
    }
    if (strcmp(resolved_src_path, resolved_dst_path) == 0) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: source and destination must be different");
        return ESP_ERR_INVALID_ARG;
    }

    err = cap_files_copy_file_internal(resolved_src_path, resolved_dst_path);
    cJSON_Delete(root);
    if (err == ESP_ERR_NOT_FOUND) {
        snprintf(output, output_size, "Error: file not found: %s", resolved_src_path);
        return err;
    }
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: failed to copy %s to %s", resolved_src_path, resolved_dst_path);
        return err;
    }

    snprintf(output, output_size, "OK: copied %s to %s", resolved_src_path, resolved_dst_path);
    return ESP_OK;
}

static esp_err_t cap_files_move_file_execute(const char *input_json,
                                             const claw_cap_call_context_t *ctx,
                                             char *output,
                                             size_t output_size)
{
    cJSON *root = NULL;
    const char *src_path = NULL;
    const char *dst_path = NULL;
    char resolved_src_path[256];
    char resolved_dst_path[256];
    struct stat st = {0};

    (void)ctx;

    root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    src_path = cJSON_GetStringValue(cJSON_GetObjectItem(root, "src_path"));
    dst_path = cJSON_GetStringValue(cJSON_GetObjectItem(root, "dst_path"));
    if (cap_files_resolve_path(src_path, resolved_src_path, sizeof(resolved_src_path)) != ESP_OK
        || cap_files_resolve_path(dst_path, resolved_dst_path, sizeof(resolved_dst_path)) != ESP_OK) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: source and destination must stay under %s", s_files_base_dir);
        return ESP_ERR_INVALID_ARG;
    }
    if (strcmp(resolved_src_path, resolved_dst_path) == 0) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: source and destination must be different");
        return ESP_ERR_INVALID_ARG;
    }

    if (stat(resolved_src_path, &st) != 0 || !S_ISREG(st.st_mode)) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: file not found: %s", resolved_src_path);
        return ESP_ERR_NOT_FOUND;
    }

    if (cap_files_ensure_parent_dirs(resolved_dst_path) != ESP_OK) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: failed to create parent directories for %s", resolved_dst_path);
        return ESP_FAIL;
    }

    if (rename(resolved_src_path, resolved_dst_path) != 0) {
        esp_err_t err = cap_files_copy_file_internal(resolved_src_path, resolved_dst_path);

        if (err != ESP_OK) {
            cJSON_Delete(root);
            snprintf(output, output_size, "Error: failed to move %s to %s", resolved_src_path, resolved_dst_path);
            return err;
        }
        if (unlink(resolved_src_path) != 0) {
            unlink(resolved_dst_path);
            cJSON_Delete(root);
            snprintf(output, output_size, "Error: failed to remove source after moving: %s", resolved_src_path);
            return ESP_FAIL;
        }
    }

    cJSON_Delete(root);
    snprintf(output, output_size, "OK: moved %s to %s", resolved_src_path, resolved_dst_path);
    return ESP_OK;
}

static esp_err_t cap_files_list_dir_execute(const char *input_json,
                                            const claw_cap_call_context_t *ctx,
                                            char *output,
                                            size_t output_size)
{
    cJSON *root = NULL;
    const char *prefix_value = NULL;
    char resolved_prefix[256];
    const char *prefix = NULL;
    size_t offset = 0;
    int count = 0;
    esp_err_t err;

    (void)ctx;

    output[0] = '\0';
    root = cJSON_Parse(input_json);
    if (root) {
        prefix_value = cJSON_GetStringValue(cJSON_GetObjectItem(root, "prefix"));
    }

    if (prefix_value && prefix_value[0]) {
        if (cap_files_resolve_path(prefix_value, resolved_prefix, sizeof(resolved_prefix)) != ESP_OK) {
            cJSON_Delete(root);
            snprintf(output, output_size, "Error: prefix must stay under %s", s_files_base_dir);
            return ESP_ERR_INVALID_ARG;
        }
        prefix = resolved_prefix;
    }

    if (cap_files_ensure_dir(s_files_base_dir) != ESP_OK) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: cannot open %s", s_files_base_dir);
        return ESP_FAIL;
    }

    err = cap_files_list_recursive(s_files_base_dir, prefix, output, output_size, &offset, &count);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: failed to list files under %s", s_files_base_dir);
        return err;
    }

    if (count == 0) {
        snprintf(output, output_size, "(no files found)");
    }
    return ESP_OK;
}

static const claw_cap_descriptor_t s_files_descriptors[] = {
    {
        .id = "read_file",
        .name = "read_file",
        .family = "files",
        .description = "Read a text file.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}",
        .execute = cap_files_read_file_execute,
    },
    {
        .id = "write_file",
        .name = "write_file",
        .family = "files",
        .description = "Create or overwrite a text file",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"content\":{\"type\":\"string\"}},\"required\":[\"path\",\"content\"]}",
        .execute = cap_files_write_file_execute,
    },
    {
        .id = "delete_file",
        .name = "delete_file",
        .family = "files",
        .description = "Delete a file.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}",
        .execute = cap_files_delete_file_execute,
    },
    {
        .id = "copy_file",
        .name = "copy_file",
        .family = "files",
        .description = "Copy a file.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"src_path\":{\"type\":\"string\"},\"dst_path\":{\"type\":\"string\"}},\"required\":[\"src_path\",\"dst_path\"]}",
        .execute = cap_files_copy_file_execute,
    },
    {
        .id = "move_file",
        .name = "move_file",
        .family = "files",
        .description = "Move a file.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"src_path\":{\"type\":\"string\"},\"dst_path\":{\"type\":\"string\"}},\"required\":[\"src_path\",\"dst_path\"]}",
        .execute = cap_files_move_file_execute,
    },
    {
        .id = "list_dir",
        .name = "list_dir",
        .family = "files",
        .description = "Recursively list files, optionally filtered by prefix.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"prefix\":{\"type\":\"string\"}}}",
        .execute = cap_files_list_dir_execute,
    },
};

static const claw_cap_group_t s_files_group = {
    .group_id = "cap_files",
    .descriptors = s_files_descriptors,
    .descriptor_count = sizeof(s_files_descriptors) / sizeof(s_files_descriptors[0]),
};

esp_err_t cap_files_register_group(void)
{
    if (s_files_base_dir[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    if (claw_cap_group_exists(s_files_group.group_id)) {
        return ESP_OK;
    }

    return claw_cap_register_group(&s_files_group);
}

esp_err_t cap_files_set_base_dir(const char *base_dir)
{
    if (!base_dir || !base_dir[0] || base_dir[0] != '/') {
        return ESP_ERR_INVALID_ARG;
    }

    strlcpy(s_files_base_dir, base_dir, sizeof(s_files_base_dir));
    return ESP_OK;
}
