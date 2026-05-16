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

typedef struct {
    const char *endpoint;
    const char *bucket;
    const char *access_key;
    const char *secret_key;
    const char *security_token;
} cap_obs_config_t;

esp_err_t cap_obs_set_config(const cap_obs_config_t *config);
esp_err_t cap_obs_register_group(void);

#ifdef __cplusplus
}
#endif
