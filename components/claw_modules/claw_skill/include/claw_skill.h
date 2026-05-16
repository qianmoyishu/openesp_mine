/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stddef.h>

#include "claw_core.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *skills_root_dir;
    const char *session_state_root_dir;
    size_t max_skill_files;
    size_t max_file_bytes;
} claw_skill_config_t;

esp_err_t claw_skill_init(const claw_skill_config_t *config);

esp_err_t claw_skill_reload_registry(void);
const char *claw_skill_get_skills_root_dir(void);

/* Renders the skill catalog used by the prompt layer. */
esp_err_t claw_skill_read_skills_list(char *buf, size_t size);

/* Loads the active skill ids for one session from persistent state. */
esp_err_t claw_skill_load_active_skill_ids(const char *session_id,
                                           char ***out_skill_ids,
                                           size_t *out_skill_count);

/* Loads the active capability groups implied by active skills for one session. */
esp_err_t claw_skill_load_active_cap_groups(const char *session_id,
                                            char ***out_group_ids,
                                            size_t *out_group_count);

/* Changes only the active skill state for one session. */
esp_err_t claw_skill_activate_for_session(const char *session_id, const char *skill_id);
esp_err_t claw_skill_deactivate_for_session(const char *session_id, const char *skill_id);
esp_err_t claw_skill_clear_active_for_session(const char *session_id);

typedef esp_err_t (*claw_skill_deactivate_guard_t)(const char *session_id,
                                                   const char *skill_id,
                                                   char *reason_out,
                                                   size_t reason_size);

esp_err_t claw_skill_register_deactivate_guard(const char *skill_id,
                                               claw_skill_deactivate_guard_t guard);

esp_err_t claw_skill_check_deactivate_allowed(const char *session_id,
                                              const char *skill_id,
                                              char *reason_out,
                                              size_t reason_size);

/* Prompt providers for the catalog and active skill documents. */
extern const claw_core_context_provider_t claw_skill_skills_list_provider;
extern const claw_core_context_provider_t claw_skill_active_skill_docs_provider;

#ifdef __cplusplus
}
#endif
