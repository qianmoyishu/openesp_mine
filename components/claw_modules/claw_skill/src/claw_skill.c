/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "claw_skill.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <sys/stat.h>

#include "cJSON.h"
#include "esp_log.h"

static const char *TAG = "claw_skill";
static const char *SKILLS_LIST_FILE = "skills_list.json";

#define CLAW_SKILL_DEFAULT_MAX_FILES 64
#define CLAW_SKILL_DEFAULT_MAX_BYTES 2048
#define CLAW_SKILL_MAX_PATH          192
#define CLAW_SKILL_MAX_GUARDS        8

typedef struct {
    char skill_id[64];
    claw_skill_deactivate_guard_t guard;
} claw_skill_guard_entry_t;

static claw_skill_guard_entry_t *s_guards = NULL;
static size_t s_guard_count;

#ifdef CONFIG_CLAW_SKILL_DEBUG_LOG
#define CLAW_SKILL_DIAGE(...) ESP_LOGE(TAG, __VA_ARGS__)
#define CLAW_SKILL_DIAGI(...) ESP_LOGI(TAG, __VA_ARGS__)
#else
#define CLAW_SKILL_DIAGE(...) do { } while (0)
#define CLAW_SKILL_DIAGI(...) do { } while (0)
#endif

typedef struct {
    char *id;
    char *file;
    char *summary;
    char **cap_groups;
    size_t cap_group_count;
} claw_skill_registry_entry_t;

typedef struct {
    int initialized;
    char skills_root_dir[CLAW_SKILL_MAX_PATH];
    char session_state_root_dir[CLAW_SKILL_MAX_PATH];
    size_t max_skill_files;
    size_t max_file_bytes;
    claw_skill_registry_entry_t *entries;
    size_t entry_count;
} claw_skill_state_t;

static claw_skill_state_t *s_skill = NULL;

static bool string_array_contains(const char *const *items, size_t count, const char *value);
static esp_err_t push_unique_string(char ***items, size_t *count, const char *value);

static void safe_copy(char *dst, size_t dst_size, const char *src)
{
    size_t len;

    if (!dst || dst_size == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }

    len = strnlen(src, dst_size - 1);
    memcpy(dst, src, len);
    dst[len] = '\0';
}

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

static void free_string_array(char **items, size_t count)
{
    size_t i;

    if (!items) {
        return;
    }

    for (i = 0; i < count; i++) {
        free(items[i]);
    }
    free(items);
}

static void free_registry_entry(claw_skill_registry_entry_t *entry)
{
    if (!entry) {
        return;
    }

    free(entry->id);
    free(entry->file);
    free(entry->summary);
    free_string_array(entry->cap_groups, entry->cap_group_count);
    memset(entry, 0, sizeof(*entry));
}

static void free_registry_entries(claw_skill_registry_entry_t *entries, size_t count)
{
    size_t i;

    if (!entries) {
        return;
    }

    for (i = 0; i < count; i++) {
        free_registry_entry(&entries[i]);
    }
    free(entries);
}

static void claw_skill_reset(void)
{
    size_t i;

    if (!s_skill) {
        free(s_guards);
        s_guards = NULL;
        s_guard_count = 0;
        return;
    }

    for (i = 0; i < s_skill->entry_count; i++) {
        free_registry_entry(&s_skill->entries[i]);
    }
    free(s_skill->entries);
    memset(s_skill, 0, sizeof(*s_skill));
    free(s_skill);
    s_skill = NULL;
    free(s_guards);
    s_guards = NULL;
    s_guard_count = 0;
}

static bool is_markdown_skill_file(const char *name)
{
    size_t len;

    if (!name) {
        return false;
    }

    len = strlen(name);
    return len > 3 && strcmp(name + len - 3, ".md") == 0;
}

static bool skill_path_is_valid(const char *path)
{
    if (!path || !path[0]) {
        return false;
    }
    if (path[0] == '/' || strstr(path, "..") != NULL) {
        return false;
    }
    return strchr(path, '\\') == NULL;
}

static char *build_skill_path_dup(const char *filename)
{
    if (!s_skill) {
        return NULL;
    }

    return dup_printf("%s/%s", s_skill->skills_root_dir, filename);
}

static esp_err_t ensure_dir(const char *path)
{
    struct stat st = {0};

    if (!path || !path[0]) {
        CLAW_SKILL_DIAGE("ensure_dir: invalid path");
        return ESP_ERR_INVALID_ARG;
    }
    if (stat(path, &st) == 0) {
        if (!S_ISDIR(st.st_mode)) {
            CLAW_SKILL_DIAGE("ensure_dir: path exists but is not a directory: %s", path);
            return ESP_FAIL;
        }
        return ESP_OK;
    }
    if (mkdir(path, 0755) != 0) {
        CLAW_SKILL_DIAGE("ensure_dir: failed to create directory: %s", path);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void sanitize_session_id(const char *session_id, char *buf, size_t size)
{
    size_t off = 0;

    if (!buf || size == 0) {
        return;
    }
    buf[0] = '\0';
    if (!session_id) {
        return;
    }

    while (*session_id && off + 1 < size) {
        char ch = *session_id++;

        if ((ch >= 'a' && ch <= 'z') ||
                (ch >= 'A' && ch <= 'Z') ||
                (ch >= '0' && ch <= '9')) {
            buf[off++] = ch;
        } else if (off == 0 || buf[off - 1] != '_') {
            buf[off++] = '_';
        }
    }
    if (off > 0 && buf[off - 1] == '_') {
        off--;
    }
    buf[off] = '\0';
}

static char *build_session_state_path_dup(const char *session_id)
{
    char safe_session_id[48];
    uint32_t hash = 2166136261u;
    const unsigned char *p = (const unsigned char *)session_id;
    size_t len;

    if (!s_skill || !session_id || !session_id[0] || !s_skill->session_state_root_dir[0]) {
        return NULL;
    }

    sanitize_session_id(session_id, safe_session_id, sizeof(safe_session_id));
    while (p && *p) {
        hash ^= *p++;
        hash *= 16777619u;
    }

    len = strnlen(safe_session_id, sizeof(safe_session_id) - 1);
    if (len > 24) {
        safe_session_id[24] = '\0';
    }

    return dup_printf("%s/s_%s_%08" PRIx32 ".skills.json",
                      s_skill->session_state_root_dir,
                      safe_session_id[0] ? safe_session_id : "default",
                      hash);
}

static esp_err_t read_file_dup(const char *path, char **out_data)
{
    FILE *file = NULL;
    long size;
    char *data = NULL;
    size_t read_bytes;

    if (!path || !out_data) {
        CLAW_SKILL_DIAGE("read_file_dup: invalid arguments");
        return ESP_ERR_INVALID_ARG;
    }
    *out_data = NULL;
    CLAW_SKILL_DIAGI("read %s", path);

    file = fopen(path, "rb");
    if (!file) {
        CLAW_SKILL_DIAGE("read_file_dup: failed to open file: %s", path);
        return ESP_ERR_NOT_FOUND;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        CLAW_SKILL_DIAGE("read_file_dup: failed to seek end: %s", path);
        fclose(file);
        return ESP_FAIL;
    }
    size = ftell(file);
    if (size < 0) {
        CLAW_SKILL_DIAGE("read_file_dup: failed to get file size: %s", path);
        fclose(file);
        return ESP_FAIL;
    }
    if (fseek(file, 0, SEEK_SET) != 0) {
        CLAW_SKILL_DIAGE("read_file_dup: failed to rewind file: %s", path);
        fclose(file);
        return ESP_FAIL;
    }

    data = calloc(1, (size_t)size + 1);
    if (!data) {
        CLAW_SKILL_DIAGE("read_file_dup: out of memory for file: %s (%ld bytes)", path, size);
        fclose(file);
        return ESP_ERR_NO_MEM;
    }

    read_bytes = fread(data, 1, (size_t)size, file);
    fclose(file);
    data[read_bytes] = '\0';
    *out_data = data;
    return ESP_OK;
}

static esp_err_t write_file_text(const char *path, const char *text)
{
    FILE *file = NULL;

    if (!path || !text) {
        CLAW_SKILL_DIAGE("write_file_text: invalid arguments");
        return ESP_ERR_INVALID_ARG;
    }
    CLAW_SKILL_DIAGI("write %s", path);

    file = fopen(path, "wb");
    if (!file) {
        CLAW_SKILL_DIAGE("write_file_text: failed to open file: %s", path);
        return ESP_FAIL;
    }
    if (fputs(text, file) < 0) {
        CLAW_SKILL_DIAGE("write_file_text: failed to write file: %s", path);
        fclose(file);
        return ESP_FAIL;
    }
    fclose(file);
    return ESP_OK;
}

static esp_err_t read_limited_file(const char *path, char *buf, size_t size)
{
    FILE *file = NULL;
    size_t n;

    if (!path || !buf || size == 0) {
        CLAW_SKILL_DIAGE("read_limited_file: invalid arguments");
        return ESP_ERR_INVALID_ARG;
    }
    CLAW_SKILL_DIAGI("read limited %s", path);

    file = fopen(path, "rb");
    if (!file) {
        CLAW_SKILL_DIAGE("read_limited_file: failed to open file: %s", path);
        return ESP_ERR_NOT_FOUND;
    }
    n = fread(buf, 1, size - 1, file);
    fclose(file);
    buf[n] = '\0';
    return ESP_OK;
}

static esp_err_t json_dup_required_string(cJSON *object, const char *key, char **out_value)
{
    cJSON *item;

    if (!object || !key || !out_value) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_value = NULL;

    item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsString(item) || !item->valuestring || !item->valuestring[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_value = strdup(item->valuestring);
    return *out_value ? ESP_OK : ESP_ERR_NO_MEM;
}

static esp_err_t json_dup_optional_unique_string_array(cJSON *object,
                                                       const char *key,
                                                       char ***out_items,
                                                       size_t *out_count)
{
    cJSON *array = NULL;
    char **items = NULL;
    size_t count = 0;
    int index;

    if (!object || !key || !out_items || !out_count) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_items = NULL;
    *out_count = 0;

    array = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!array) {
        return ESP_OK;
    }
    if (!cJSON_IsArray(array)) {
        return ESP_ERR_INVALID_ARG;
    }

    for (index = 0; index < cJSON_GetArraySize(array); index++) {
        cJSON *item = cJSON_GetArrayItem(array, index);
        esp_err_t err;

        if (!cJSON_IsString(item) || !item->valuestring || !item->valuestring[0]) {
            free_string_array(items, count);
            return ESP_ERR_INVALID_ARG;
        }
        if (string_array_contains((const char *const *)items, count, item->valuestring)) {
            free_string_array(items, count);
            return ESP_ERR_INVALID_ARG;
        }

        err = push_unique_string(&items, &count, item->valuestring);
        if (err != ESP_OK) {
            free_string_array(items, count);
            return err;
        }
    }

    *out_items = items;
    *out_count = count;
    return ESP_OK;
}

static const claw_skill_registry_entry_t *claw_skill_find_entry(const char *skill_id)
{
    size_t i;

    if (!s_skill || !skill_id || !skill_id[0]) {
        return NULL;
    }

    for (i = 0; i < s_skill->entry_count; i++) {
        if (strcmp(s_skill->entries[i].id, skill_id) == 0) {
            return &s_skill->entries[i];
        }
    }

    return NULL;
}

static bool string_array_contains(const char *const *items, size_t count, const char *value)
{
    size_t i;

    if (!items || !value) {
        return false;
    }

    for (i = 0; i < count; i++) {
        if (items[i] && strcmp(items[i], value) == 0) {
            return true;
        }
    }

    return false;
}

static esp_err_t push_unique_string(char ***items, size_t *count, const char *value)
{
    char **grown;

    if (!items || !count || !value || !value[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (string_array_contains((const char *const *) * items, *count, value)) {
        return ESP_OK;
    }

    grown = realloc(*items, sizeof(char *) * (*count + 1));
    if (!grown) {
        return ESP_ERR_NO_MEM;
    }
    *items = grown;
    (*items)[*count] = strdup(value);
    if (!(*items)[*count]) {
        return ESP_ERR_NO_MEM;
    }
    (*count)++;
    return ESP_OK;
}

static esp_err_t validate_registry_entry(claw_skill_registry_entry_t *entry)
{
    char *path = NULL;
    FILE *file = NULL;
    size_t i;

    if (!entry || !entry->id || !entry->file || !entry->summary) {
        CLAW_SKILL_DIAGE("validate_registry_entry: missing required fields");
        return ESP_ERR_INVALID_ARG;
    }
    if (!skill_path_is_valid(entry->file) || !is_markdown_skill_file(entry->file)) {
        CLAW_SKILL_DIAGE("validate_registry_entry: invalid skill file path for id=%s file=%s",
                         entry->id ? entry->id : "(null)",
                         entry->file ? entry->file : "(null)");
        return ESP_ERR_INVALID_ARG;
    }
    for (i = 0; i < entry->cap_group_count; i++) {
        if (!entry->cap_groups[i] || !entry->cap_groups[i][0]) {
            CLAW_SKILL_DIAGE("validate_registry_entry: invalid cap_group for id=%s at index=%u",
                             entry->id ? entry->id : "(null)",
                             (unsigned)i);
            return ESP_ERR_INVALID_ARG;
        }
    }

    path = build_skill_path_dup(entry->file);
    if (!path) {
        CLAW_SKILL_DIAGE("validate_registry_entry: failed to build skill path for id=%s",
                         entry->id ? entry->id : "(null)");
        return ESP_ERR_NO_MEM;
    }
    file = fopen(path, "rb");
    if (!file) {
        CLAW_SKILL_DIAGE("validate_registry_entry: missing markdown file for id=%s path=%s",
                         entry->id ? entry->id : "(null)",
                         path);
        free(path);
        return ESP_ERR_NOT_FOUND;
    }
    free(path);
    fclose(file);
    return ESP_OK;
}

static esp_err_t load_registry_from_json(void)
{
    char *path = NULL;
    char *json_text = NULL;
    cJSON *root = NULL;
    cJSON *skills = NULL;
    cJSON *skill = NULL;
    claw_skill_registry_entry_t *entries = NULL;
    size_t entry_count = 0;
    size_t entry_index = 0;
    esp_err_t err = ESP_OK;

    path = build_skill_path_dup(SKILLS_LIST_FILE);
    if (!path) {
        ESP_LOGE(TAG, "registry path alloc failed");
        return ESP_ERR_NO_MEM;
    }
    CLAW_SKILL_DIAGI("load registry %s", path);

    err = read_file_dup(path, &json_text);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "read %s: %s", path, esp_err_to_name(err));
        free(path);
        return err;
    }
    free(path);

    root = cJSON_Parse(json_text);
    free(json_text);
    if (!root || !cJSON_IsObject(root)) {
        ESP_LOGE(TAG, "bad %s", SKILLS_LIST_FILE);
        cJSON_Delete(root);
        return ESP_ERR_INVALID_STATE;
    }

    skills = cJSON_GetObjectItemCaseSensitive(root, "skills");
    if (!cJSON_IsArray(skills)) {
        ESP_LOGE(TAG, "bad skills array");
        err = ESP_ERR_INVALID_ARG;
        goto cleanup;
    }

    entry_count = (size_t)cJSON_GetArraySize(skills);
    if (entry_count == 0 || entry_count > s_skill->max_skill_files) {
        ESP_LOGE(TAG, "bad skill count %u/%u", (unsigned)entry_count, (unsigned)s_skill->max_skill_files);
        err = ESP_ERR_INVALID_ARG;
        goto cleanup;
    }

    entries = calloc(entry_count, sizeof(*entries));
    if (!entries) {
        ESP_LOGE(TAG, "registry alloc %u", (unsigned)entry_count);
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    cJSON_ArrayForEach(skill, skills) {
        claw_skill_registry_entry_t *entry;
        size_t i;

        if (!cJSON_IsObject(skill) || entry_index >= entry_count) {
            ESP_LOGE(TAG, "bad skill entry #%u", (unsigned)entry_index);
            err = ESP_ERR_INVALID_ARG;
            goto cleanup;
        }

        entry = &entries[entry_index];
        err = json_dup_required_string(skill, "id", &entry->id);
        if (err == ESP_OK) {
            err = json_dup_required_string(skill, "file", &entry->file);
        }
        if (err == ESP_OK) {
            err = json_dup_required_string(skill, "summary", &entry->summary);
        }
        if (err == ESP_OK) {
            err = json_dup_optional_unique_string_array(skill,
                                                        "cap_groups",
                                                        &entry->cap_groups,
                                                        &entry->cap_group_count);
        }
        if (err == ESP_OK) {
            err = validate_registry_entry(entry);
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "skill #%u (%s): %s", (unsigned)entry_index, entry->id ? entry->id : "(unknown)", esp_err_to_name(err));
            goto cleanup;
        }

        for (i = 0; i < entry_index; i++) {
            if (strcmp(entries[i].id, entry->id) == 0) {
                ESP_LOGE(TAG, "duplicate skill %s", entry->id);
                err = ESP_ERR_INVALID_ARG;
                goto cleanup;
            }
        }

        entry_index++;
    }

    s_skill->entries = entries;
    s_skill->entry_count = entry_count;
    entries = NULL;

cleanup:
    free_registry_entries(entries, entry_count);
    cJSON_Delete(root);
    return err;
}

static esp_err_t claw_skill_read_document(const claw_skill_registry_entry_t *entry,
                                          char *buf,
                                          size_t size)
{
    char *path = NULL;
    esp_err_t err;

    if (!entry || !buf || size == 0) {
        ESP_LOGE(TAG, "read doc: bad arg");
        return ESP_ERR_INVALID_ARG;
    }

    path = build_skill_path_dup(entry->file);
    if (!path) {
        ESP_LOGE(TAG, "read doc %s: no path", entry->id ? entry->id : "(null)");
        return ESP_ERR_NO_MEM;
    }
    CLAW_SKILL_DIAGI("read doc %s", entry->id ? entry->id : "(null)");
    err = read_limited_file(path, buf, size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "read doc %s: %s", entry->id ? entry->id : "(null)", esp_err_to_name(err));
    }
    free(path);
    return err;
}

static esp_err_t load_active_skill_ids_from_disk(const char *session_id,
                                                 char ***out_skill_ids,
                                                 size_t *out_skill_count)
{
    char *path = NULL;
    char *json_text = NULL;
    cJSON *root = NULL;
    char **loaded = NULL;
    size_t loaded_count = 0;
    size_t i;
    esp_err_t err;

    if (!out_skill_ids || !out_skill_count) {
        ESP_LOGE(TAG, "load active: bad arg");
        return ESP_ERR_INVALID_ARG;
    }
    *out_skill_ids = NULL;
    *out_skill_count = 0;

    if (!s_skill || !s_skill->initialized || !session_id || !session_id[0]) {
        ESP_LOGE(TAG, "load active: bad state");
        return ESP_ERR_INVALID_STATE;
    }

    path = build_session_state_path_dup(session_id);
    if (!path) {
        ESP_LOGE(TAG, "session path %s", session_id ? session_id : "(null)");
        return ESP_ERR_INVALID_ARG;
    }
    CLAW_SKILL_DIAGI("load active %s", session_id);

    err = read_file_dup(path, &json_text);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "read session %s: %s", session_id, esp_err_to_name(err));
        free(path);
        return err;
    }
    free(path);

    root = cJSON_Parse(json_text);
    free(json_text);
    if (!cJSON_IsArray(root)) {
        ESP_LOGE(TAG, "bad session json %s", session_id);
        cJSON_Delete(root);
        return ESP_ERR_INVALID_STATE;
    }

    for (i = 0; i < (size_t)cJSON_GetArraySize(root); i++) {
        cJSON *item = cJSON_GetArrayItem(root, (int)i);

        if (!cJSON_IsString(item) || !item->valuestring || !item->valuestring[0]) {
            continue;
        }
        if (!claw_skill_find_entry(item->valuestring)) {
            continue;
        }
        err = push_unique_string(&loaded, &loaded_count, item->valuestring);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "store skill %s: %s", item->valuestring, esp_err_to_name(err));
            free_string_array(loaded, loaded_count);
            cJSON_Delete(root);
            return err;
        }
    }

    cJSON_Delete(root);
    if (loaded_count == 0) {
        free_string_array(loaded, loaded_count);
        return ESP_ERR_NOT_FOUND;
    }

    *out_skill_ids = loaded;
    *out_skill_count = loaded_count;
    return ESP_OK;
}

static esp_err_t save_active_skill_ids_to_disk(const char *session_id,
                                               const char *const *skill_ids,
                                               size_t skill_count)
{
    char *path = NULL;
    cJSON *root = NULL;
    char *json_text = NULL;
    esp_err_t err = ESP_OK;
    size_t i;

    if (!s_skill || !s_skill->initialized || !session_id || !session_id[0]) {
        ESP_LOGE(TAG, "save active: bad state");
        return ESP_ERR_INVALID_STATE;
    }

    path = build_session_state_path_dup(session_id);
    if (!path) {
        ESP_LOGE(TAG, "session path %s", session_id ? session_id : "(null)");
        return ESP_ERR_INVALID_ARG;
    }
    CLAW_SKILL_DIAGI("save active %s (%u)", session_id, (unsigned)skill_count);

    if (skill_count == 0) {
        remove(path);
        free(path);
        return ESP_OK;
    }

    root = cJSON_CreateArray();
    if (!root) {
        ESP_LOGE(TAG, "session array alloc %s", session_id);
        free(path);
        return ESP_ERR_NO_MEM;
    }

    for (i = 0; i < skill_count; i++) {
        cJSON *item;

        if (!skill_ids[i] || !skill_ids[i][0]) {
            continue;
        }
        item = cJSON_CreateString(skill_ids[i]);
        if (!item) {
            ESP_LOGE(TAG, "skill item alloc %s", skill_ids[i]);
            err = ESP_ERR_NO_MEM;
            goto cleanup;
        }
        cJSON_AddItemToArray(root, item);
    }

    json_text = cJSON_PrintUnformatted(root);
    if (!json_text) {
        ESP_LOGE(TAG, "session encode %s", session_id);
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    err = write_file_text(path, json_text);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "write session %s: %s", session_id, esp_err_to_name(err));
    }

cleanup:
    free(path);
    cJSON_Delete(root);
    free(json_text);
    return err;
}

static esp_err_t claw_skill_render_skills_list(char *buf, size_t size)
{
    size_t i;
    size_t off = 0;

    if (!s_skill || !s_skill->initialized || !buf || size == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    buf[0] = '\0';
    off += snprintf(buf + off, size - off, "Available skills:\n");
    for (i = 0; i < s_skill->entry_count && off + 1 < size; i++) {
        const claw_skill_registry_entry_t *entry = &s_skill->entries[i];

        off += snprintf(buf + off,
                        size - off,
                        "- %s: %s\n",
                        entry->id,
                        entry->summary);
    }

    return ESP_OK;
}

static esp_err_t claw_skill_build_prompt_block(const char *const *skill_ids,
                                               size_t skill_count,
                                               char *buf,
                                               size_t size)
{
    size_t i;
    size_t off = 0;

    if (!s_skill || !s_skill->initialized || !buf || size == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    buf[0] = '\0';
    for (i = 0; i < skill_count && off + 1 < size; i++) {
        const claw_skill_registry_entry_t *entry = claw_skill_find_entry(skill_ids[i]);
        char *content = NULL;
        esp_err_t err;

        if (!entry) {
            return ESP_ERR_NOT_FOUND;
        }

        content = calloc(1, s_skill->max_file_bytes + 1);
        if (!content) {
            return ESP_ERR_NO_MEM;
        }

        err = claw_skill_read_document(entry, content, s_skill->max_file_bytes + 1);
        if (err != ESP_OK) {
            free(content);
            return err;
        }

        off += snprintf(buf + off,
                        size - off,
                        "%s### %s\n%s\n",
                        i == 0 ? "" : "\n",
                        entry->id,
                        content);
        free(content);
    }

    return ESP_OK;
}

esp_err_t claw_skill_init(const claw_skill_config_t *config)
{
    esp_err_t err;

    if (!config || !config->skills_root_dir || !config->session_state_root_dir) {
        ESP_LOGE(TAG, "init: bad config");
        return ESP_ERR_INVALID_ARG;
    }

    claw_skill_reset();
    s_skill = calloc(1, sizeof(*s_skill));
    s_guards = calloc(CLAW_SKILL_MAX_GUARDS, sizeof(*s_guards));
    if (!s_skill || !s_guards) {
        claw_skill_reset();
        return ESP_ERR_NO_MEM;
    }
    safe_copy(s_skill->skills_root_dir, sizeof(s_skill->skills_root_dir), config->skills_root_dir);
    safe_copy(s_skill->session_state_root_dir,
              sizeof(s_skill->session_state_root_dir),
              config->session_state_root_dir);
    s_skill->max_skill_files = config->max_skill_files ? config->max_skill_files : CLAW_SKILL_DEFAULT_MAX_FILES;
    s_skill->max_file_bytes = config->max_file_bytes ? config->max_file_bytes : CLAW_SKILL_DEFAULT_MAX_BYTES;

    err = ensure_dir(s_skill->session_state_root_dir);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "init dir %s: %s", s_skill->session_state_root_dir, esp_err_to_name(err));
        claw_skill_reset();
        return err;
    }

    err = load_registry_from_json();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "init registry: %s", esp_err_to_name(err));
        claw_skill_reset();
        return err;
    }

    s_skill->initialized = 1;
    ESP_LOGI(TAG, "Initialized registry with %u skill(s)", (unsigned)s_skill->entry_count);
    return ESP_OK;
}

esp_err_t claw_skill_reload_registry(void)
{
    claw_skill_registry_entry_t *old_entries = NULL;
    size_t old_count = 0;
    esp_err_t err;

    if (!s_skill || !s_skill->initialized) {
        ESP_LOGE(TAG, "reload before init");
        return ESP_ERR_INVALID_STATE;
    }

    old_entries = s_skill->entries;
    old_count = s_skill->entry_count;
    s_skill->entries = NULL;
    s_skill->entry_count = 0;

    err = load_registry_from_json();
    if (err == ESP_OK) {
        free_registry_entries(old_entries, old_count);
        ESP_LOGI(TAG, "Reloaded registry with %u skill(s)", (unsigned)s_skill->entry_count);
        return ESP_OK;
    }

    s_skill->entries = old_entries;
    s_skill->entry_count = old_count;
    ESP_LOGE(TAG, "reload registry: %s", esp_err_to_name(err));
    return err;
}

const char *claw_skill_get_skills_root_dir(void)
{
    if (!s_skill || !s_skill->initialized || !s_skill->skills_root_dir[0]) {
        return NULL;
    }

    return s_skill->skills_root_dir;
}

esp_err_t claw_skill_read_skills_list(char *buf, size_t size)
{
    return claw_skill_render_skills_list(buf, size);
}

esp_err_t claw_skill_load_active_skill_ids(const char *session_id,
                                           char ***out_skill_ids,
                                           size_t *out_skill_count)
{
    return load_active_skill_ids_from_disk(session_id, out_skill_ids, out_skill_count);
}

esp_err_t claw_skill_load_active_cap_groups(const char *session_id,
                                            char ***out_group_ids,
                                            size_t *out_group_count)
{
    char **active_skill_ids = NULL;
    size_t active_skill_count = 0;
    char **group_ids = NULL;
    size_t group_count = 0;
    esp_err_t err;
    size_t i;
    size_t j;

    if (!out_group_ids || !out_group_count) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_group_ids = NULL;
    *out_group_count = 0;

    err = load_active_skill_ids_from_disk(session_id, &active_skill_ids, &active_skill_count);
    if (err != ESP_OK) {
        return err;
    }

    for (i = 0; i < active_skill_count; i++) {
        const claw_skill_registry_entry_t *entry = claw_skill_find_entry(active_skill_ids[i]);

        if (!entry) {
            continue;
        }

        for (j = 0; j < entry->cap_group_count; j++) {
            err = push_unique_string(&group_ids, &group_count, entry->cap_groups[j]);
            if (err != ESP_OK) {
                free_string_array(active_skill_ids, active_skill_count);
                free_string_array(group_ids, group_count);
                return err;
            }
        }
    }

    free_string_array(active_skill_ids, active_skill_count);
    if (group_count == 0) {
        free_string_array(group_ids, group_count);
        return ESP_ERR_NOT_FOUND;
    }

    *out_group_ids = group_ids;
    *out_group_count = group_count;
    return ESP_OK;
}

esp_err_t claw_skill_activate_for_session(const char *session_id, const char *skill_id)
{
    char **active = NULL;
    size_t active_count = 0;
    esp_err_t err;

    if (!s_skill || !s_skill->initialized || !session_id || !session_id[0] || !skill_id || !skill_id[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!claw_skill_find_entry(skill_id)) {
        return ESP_ERR_NOT_FOUND;
    }

    err = load_active_skill_ids_from_disk(session_id, &active, &active_count);
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
        return err;
    }

    err = push_unique_string(&active, &active_count, skill_id);
    if (err != ESP_OK) {
        free_string_array(active, active_count);
        return err;
    }

    err = save_active_skill_ids_to_disk(session_id, (const char *const *)active, active_count);
    free_string_array(active, active_count);
    return err;
}

esp_err_t claw_skill_deactivate_for_session(const char *session_id, const char *skill_id)
{
    char **active = NULL;
    size_t active_count = 0;
    size_t write_count = 0;
    size_t i;
    esp_err_t err;

    if (!s_skill || !s_skill->initialized || !session_id || !session_id[0] || !skill_id || !skill_id[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    err = load_active_skill_ids_from_disk(session_id, &active, &active_count);
    if (err == ESP_ERR_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    for (i = 0; i < active_count; i++) {
        if (active[i] && strcmp(active[i], skill_id) != 0) {
            active[write_count++] = active[i];
        } else {
            free(active[i]);
            active[i] = NULL;
        }
    }

    err = save_active_skill_ids_to_disk(session_id, (const char *const *)active, write_count);
    for (i = 0; i < write_count; i++) {
        free(active[i]);
    }
    free(active);
    return err;
}

esp_err_t claw_skill_clear_active_for_session(const char *session_id)
{
    if (!s_skill || !s_skill->initialized || !session_id || !session_id[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    return save_active_skill_ids_to_disk(session_id, NULL, 0);
}

esp_err_t claw_skill_register_deactivate_guard(const char *skill_id,
                                               claw_skill_deactivate_guard_t guard)
{
    if (!skill_id || !skill_id[0] || !guard) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_guards) {
        return ESP_ERR_INVALID_STATE;
    }

    for (size_t i = 0; i < s_guard_count; i++) {
        if (strcmp(s_guards[i].skill_id, skill_id) == 0) {
            s_guards[i].guard = guard;
            ESP_LOGI(TAG, "Replaced deactivate guard for skill '%s'", skill_id);
            return ESP_OK;
        }
    }

    if (s_guard_count >= CLAW_SKILL_MAX_GUARDS) {
        ESP_LOGE(TAG, "Cannot register guard for '%s': table full (max %d)",
                 skill_id, CLAW_SKILL_MAX_GUARDS);
        return ESP_ERR_NO_MEM;
    }

    safe_copy(s_guards[s_guard_count].skill_id,
              sizeof(s_guards[s_guard_count].skill_id),
              skill_id);
    s_guards[s_guard_count].guard = guard;
    s_guard_count++;
    ESP_LOGI(TAG, "Registered deactivate guard for skill '%s'", skill_id);
    return ESP_OK;
}

esp_err_t claw_skill_check_deactivate_allowed(const char *session_id,
                                              const char *skill_id,
                                              char *reason_out,
                                              size_t reason_size)
{
    if (reason_out && reason_size > 0) {
        reason_out[0] = '\0';
    }
    if (!skill_id || !skill_id[0]) {
        return ESP_OK;
    }
    if (!s_guards) {
        return ESP_OK;
    }

    for (size_t i = 0; i < s_guard_count; i++) {
        if (strcmp(s_guards[i].skill_id, skill_id) != 0) {
            continue;
        }
        esp_err_t err = s_guards[i].guard(session_id, skill_id, reason_out, reason_size);
        if (err != ESP_OK && reason_out && reason_size > 0 && reason_out[0] == '\0') {
            snprintf(reason_out, reason_size,
                     "deactivation refused by skill '%s' guard", skill_id);
        }
        return err;
    }
    return ESP_OK;
}

static esp_err_t claw_skill_skills_list_collect(const claw_core_request_t *request,
                                                claw_core_context_t *out_context,
                                                void *user_ctx)
{
    char *content = NULL;
    size_t content_size;
    esp_err_t err;

    (void)request;
    (void)user_ctx;

    if (!out_context || !s_skill || !s_skill->initialized) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out_context, 0, sizeof(*out_context));

    content_size = 64;
    for (size_t i = 0; i < s_skill->entry_count; i++) {
        content_size += strlen(s_skill->entries[i].id ? s_skill->entries[i].id : "");
        content_size += strlen(s_skill->entries[i].summary ? s_skill->entries[i].summary : "");
        content_size += 16;
    }

    content = calloc(1, content_size + 1);
    if (!content) {
        return ESP_ERR_NO_MEM;
    }

    err = claw_skill_render_skills_list(content, content_size + 1);
    if (err != ESP_OK) {
        free(content);
        return err;
    }
    if (!content[0]) {
        free(content);
        return ESP_ERR_NOT_FOUND;
    }

    out_context->kind = CLAW_CORE_CONTEXT_KIND_SYSTEM_PROMPT;
    out_context->content = content;
    return ESP_OK;
}

static esp_err_t claw_skill_active_docs_collect(const claw_core_request_t *request,
                                                claw_core_context_t *out_context,
                                                void *user_ctx)
{
    char **active_skill_ids = NULL;
    size_t active_skill_count = 0;
    char *content = NULL;
    size_t content_size;
    esp_err_t err;

    (void)user_ctx;

    if (!request || !out_context || !request->session_id || !request->session_id[0]) {
        return ESP_ERR_NOT_FOUND;
    }
    memset(out_context, 0, sizeof(*out_context));

    err = load_active_skill_ids_from_disk(request->session_id, &active_skill_ids, &active_skill_count);
    if (err != ESP_OK) {
        return err;
    }
    if (active_skill_count == 0) {
        free_string_array(active_skill_ids, active_skill_count);
        return ESP_ERR_NOT_FOUND;
    }

    content_size = (active_skill_count * (s_skill->max_file_bytes + 128)) + 1;
    content = calloc(1, content_size);
    if (!content) {
        free_string_array(active_skill_ids, active_skill_count);
        return ESP_ERR_NO_MEM;
    }

    err = claw_skill_build_prompt_block((const char *const *)active_skill_ids,
                                        active_skill_count,
                                        content,
                                        content_size);
    free_string_array(active_skill_ids, active_skill_count);
    if (err != ESP_OK) {
        free(content);
        return err;
    }
    if (!content[0]) {
        free(content);
        return ESP_ERR_NOT_FOUND;
    }

    out_context->kind = CLAW_CORE_CONTEXT_KIND_SYSTEM_PROMPT;
    out_context->content = content;
    return ESP_OK;
}

const claw_core_context_provider_t claw_skill_skills_list_provider = {
    .name = "Skills List",
    .collect = claw_skill_skills_list_collect,
    .user_ctx = NULL,
};

const claw_core_context_provider_t claw_skill_active_skill_docs_provider = {
    .name = "Active Skill Docs",
    .collect = claw_skill_active_docs_collect,
    .user_ctx = NULL,
};
