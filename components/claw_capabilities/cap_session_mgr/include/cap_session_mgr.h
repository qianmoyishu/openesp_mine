/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stddef.h>

#include "claw_event_router.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t cap_session_mgr_register_group(void);
esp_err_t cap_session_mgr_set_session_root_dir(const char *session_root_dir);
size_t cap_session_mgr_build_session_id(const claw_event_t *event, char *buf, size_t buf_size, void *user_ctx);

#ifdef __cplusplus
}
#endif
