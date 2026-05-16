/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t pixel_format;
    char pixel_format_str[5];
} camera_stream_info_t;

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t pixel_format;
    char pixel_format_str[5];
    size_t frame_bytes;
    int64_t timestamp_us;
} camera_frame_info_t;

esp_err_t camera_open(const char *dev_path);
esp_err_t camera_get_stream_info(camera_stream_info_t *out_info);
esp_err_t camera_capture_frame(int timeout_ms, uint8_t **frame_data, size_t *frame_bytes,
                               camera_frame_info_t *out_info);
esp_err_t camera_release_frame(void *frame_data);
esp_err_t camera_capture_jpeg(int timeout_ms, uint8_t **jpeg_data, size_t *jpeg_bytes,
                              camera_frame_info_t *out_info);
esp_err_t camera_close(void);
void camera_free_buffer(void *buffer);

#ifdef __cplusplus
}
#endif
