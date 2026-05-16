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
    const char *app_id;
    const char *api_key;
    const char *api_secret;
} cap_xfyun_speech_config_t;

esp_err_t cap_xfyun_speech_set_config(const cap_xfyun_speech_config_t *config);
esp_err_t cap_xfyun_speech_set_base_dir(const char *base_dir);
esp_err_t cap_xfyun_speech_register_group(void);

#ifdef __cplusplus
}
#endif
