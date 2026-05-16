/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t cap_files_register_group(void);
esp_err_t cap_files_set_base_dir(const char *base_dir);

#ifdef __cplusplus
}
#endif
