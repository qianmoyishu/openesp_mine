/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef esp_err_t (*cap_at_mqtt_command_handler_t)(const char *command,
                                                   char *response,
                                                   size_t response_size,
                                                   void *user_ctx);
typedef esp_err_t (*cap_at_mqtt_payload_handler_t)(const uint8_t *payload,
                                                   size_t payload_len,
                                                   char *response,
                                                   size_t response_size,
                                                   void *user_ctx);

esp_err_t cap_at_mqtt_uart1_configure(int tx_pin, int rx_pin, int baud_rate);
esp_err_t cap_at_mqtt_uart1_write_line(const char *line);
esp_err_t cap_at_mqtt_set_command_handler(cap_at_mqtt_command_handler_t handler, void *user_ctx);
esp_err_t cap_at_mqtt_expect_payload(size_t payload_len,
                                     cap_at_mqtt_payload_handler_t handler,
                                     void *user_ctx);
esp_err_t cap_at_mqtt_notify_wifi_status(bool connected,
                                         const char *sta_ip,
                                         const char *mode,
                                         const char *ap_ssid);
esp_err_t cap_at_mqtt_register_group(void);

#ifdef __cplusplus
}
#endif
