/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "claw_core.h"
#include "esp_err.h"
#include "lua.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *name;
    lua_CFunction open_fn;
} cap_lua_module_t;

typedef void (*cap_lua_runtime_cleanup_fn_t)(void);

esp_err_t cap_lua_register_group(const char *base_dir);
esp_err_t cap_lua_add_package_path_dir(const char *dir);
esp_err_t cap_lua_register_module(const char *name, lua_CFunction open_fn);
esp_err_t cap_lua_register_modules(const cap_lua_module_t *modules, size_t count);
esp_err_t cap_lua_register_runtime_cleanup(cap_lua_runtime_cleanup_fn_t cleanup_fn);
esp_err_t cap_lua_list_scripts(const char *prefix, const char *keyword, char *output, size_t output_size);
esp_err_t cap_lua_write_script(const char *path,
                               const char *content,
                               bool overwrite,
                               char *output,
                               size_t output_size);
esp_err_t cap_lua_run_script(const char *path,
                             const char *args_json,
                             uint32_t timeout_ms,
                             char *output,
                             size_t output_size);

esp_err_t cap_lua_run_script_async(const char *path,
                                   const char *args_json,
                                   uint32_t timeout_ms,
                                   const char *name,
                                   const char *exclusive,
                                   bool replace,
                                   char *output,
                                   size_t output_size);
esp_err_t cap_lua_list_jobs(const char *status, char *output, size_t output_size);
esp_err_t cap_lua_get_job(const char *id_or_name, char *output, size_t output_size);
esp_err_t cap_lua_stop_job(const char *id_or_name,
                           uint32_t wait_ms,
                           char *output,
                           size_t output_size);
esp_err_t cap_lua_stop_all_jobs(const char *exclusive_filter,
                                uint32_t wait_ms,
                                char *output,
                                size_t output_size);

extern const claw_core_context_provider_t cap_lua_async_jobs_provider;

size_t cap_lua_get_active_async_job_count(void);

void cap_lua_honesty_observe_completion(const claw_core_completion_summary_t *summary,
                                        void *user_ctx);

#ifdef __cplusplus
}
#endif
