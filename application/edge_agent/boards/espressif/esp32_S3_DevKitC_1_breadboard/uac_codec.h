/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include "usb/uac_host.h"
#include "dev_audio_codec.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t addr;
    uint8_t iface_num;
    uint32_t preferred_sample_rate;
    bool is_input;
} uac_codec_config_t;

dev_audio_codec_handles_t *uac_codec_new_handle(const uac_codec_config_t *config);
void uac_codec_delete_handle(dev_audio_codec_handles_t *codec_handles);

#ifdef __cplusplus
}
#endif
