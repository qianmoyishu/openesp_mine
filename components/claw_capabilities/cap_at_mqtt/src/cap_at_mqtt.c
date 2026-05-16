/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_at_mqtt.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "claw_cap.h"
#include "driver/uart.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "cap_at_mqtt";

#define CAP_AT_UART_NUM UART_NUM_1
#define CAP_AT_DEFAULT_TX_PIN 17
#define CAP_AT_DEFAULT_RX_PIN 18
#define CAP_AT_DEFAULT_BAUD 115200
#define CAP_AT_RX_BUF_SIZE 2048
#define CAP_AT_TX_BUF_SIZE 1024
#define CAP_AT_CMD_BUF_SIZE 768
#define CAP_AT_RESP_BUF_SIZE 2048
#define CAP_AT_LINE_BUF_SIZE 256

static SemaphoreHandle_t s_at_lock;
static TaskHandle_t s_at_rx_task;
static bool s_uart_ready;
static int s_tx_pin = CAP_AT_DEFAULT_TX_PIN;
static int s_rx_pin = CAP_AT_DEFAULT_RX_PIN;
static int s_baud_rate = CAP_AT_DEFAULT_BAUD;
static cap_at_mqtt_command_handler_t s_command_handler;
static void *s_command_handler_user_ctx;
static cap_at_mqtt_payload_handler_t s_payload_handler;
static void *s_payload_handler_user_ctx;
static uint8_t *s_payload_buf;
static size_t s_payload_expected;
static size_t s_payload_used;

static esp_err_t cap_at_write_raw_locked(const char *data, size_t len);

static esp_err_t cap_at_init_lock(void)
{
    if (!s_at_lock) {
        s_at_lock = xSemaphoreCreateMutex();
        if (!s_at_lock) {
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

static const char *cap_at_json_string(cJSON *root, const char *key, const char *fallback)
{
    if (!root) {
        return fallback;
    }
    const char *value = cJSON_GetStringValue(cJSON_GetObjectItem(root, key));
    return value ? value : fallback;
}

static int cap_at_json_int(cJSON *root, const char *key, int fallback)
{
    if (!root) {
        return fallback;
    }
    cJSON *item = cJSON_GetObjectItem(root, key);
    return cJSON_IsNumber(item) ? item->valueint : fallback;
}

static bool cap_at_valid_baud(int baud_rate)
{
    return baud_rate >= 1200 && baud_rate <= 5000000;
}

static bool cap_at_line_is_blank(const char *line)
{
    if (!line) {
        return true;
    }
    while (*line) {
        if (*line != ' ' && *line != '\t' && *line != '\r' && *line != '\n') {
            return false;
        }
        line++;
    }
    return true;
}

static esp_err_t cap_at_send_response(const char *response, esp_err_t status)
{
    esp_err_t err = ESP_OK;

    if (response && strcmp(response, ">") == 0 && status == ESP_OK) {
        ESP_RETURN_ON_ERROR(cap_at_init_lock(), TAG, "create lock failed");
        xSemaphoreTake(s_at_lock, portMAX_DELAY);
        err = cap_at_write_raw_locked(">", 1);
        xSemaphoreGive(s_at_lock);
        return err;
    }

    if (response && response[0]) {
        err = cap_at_mqtt_uart1_write_line(response);
    }
    if (err == ESP_OK) {
        err = cap_at_mqtt_uart1_write_line(status == ESP_OK ? "OK" : "ERROR");
    }
    return err;
}

static void cap_at_handle_rx_line(char *line)
{
    char response[CAP_AT_RESP_BUF_SIZE] = {0};
    esp_err_t err = ESP_OK;

    if (cap_at_line_is_blank(line)) {
        return;
    }

    if (s_command_handler) {
        err = s_command_handler(line, response, sizeof(response), s_command_handler_user_ctx);
    } else if (strcmp(line, "AT") != 0 && strcmp(line, "ATE0") != 0 && strcmp(line, "ATE1") != 0) {
        snprintf(response, sizeof(response), "Unsupported AT command: %s", line);
        err = ESP_ERR_NOT_SUPPORTED;
    }

    if (cap_at_send_response(response, err) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to send AT response for line: %s", line);
    }
}

static void cap_at_handle_payload_byte(uint8_t byte)
{
    char response[CAP_AT_RESP_BUF_SIZE] = {0};
    esp_err_t err;

    if (!s_payload_buf || s_payload_expected == 0) {
        return;
    }

    s_payload_buf[s_payload_used++] = byte;
    if (s_payload_used < s_payload_expected) {
        return;
    }

    if (!s_payload_handler) {
        err = ESP_ERR_INVALID_STATE;
        snprintf(response, sizeof(response), "No payload handler");
    } else {
        err = s_payload_handler(s_payload_buf,
                                s_payload_used,
                                response,
                                sizeof(response),
                                s_payload_handler_user_ctx);
    }

    free(s_payload_buf);
    s_payload_buf = NULL;
    s_payload_expected = 0;
    s_payload_used = 0;
    s_payload_handler = NULL;
    s_payload_handler_user_ctx = NULL;

    if (cap_at_send_response(response, err) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to send AT payload response");
    }
}

static void cap_at_rx_task(void *arg)
{
    uint8_t rx[64];
    char line[CAP_AT_LINE_BUF_SIZE];
    size_t used = 0;

    (void)arg;
    memset(line, 0, sizeof(line));

    while (1) {
        int got = 0;

        if (!s_uart_ready || !s_at_lock) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if (xSemaphoreTake(s_at_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
            got = uart_read_bytes(CAP_AT_UART_NUM, rx, sizeof(rx), pdMS_TO_TICKS(20));
            xSemaphoreGive(s_at_lock);
        }
        if (got <= 0) {
            continue;
        }

        for (int i = 0; i < got; i++) {
            char ch = (char)rx[i];

            if (s_payload_expected > 0) {
                cap_at_handle_payload_byte(rx[i]);
                continue;
            }

            if (ch == '\r' || ch == '\n') {
                if (used > 0) {
                    line[used] = '\0';
                    cap_at_handle_rx_line(line);
                    used = 0;
                    memset(line, 0, sizeof(line));
                }
                continue;
            }

            if (used + 1 < sizeof(line)) {
                line[used++] = ch;
            } else {
                used = 0;
                memset(line, 0, sizeof(line));
                cap_at_send_response("AT command line too long", ESP_ERR_INVALID_SIZE);
            }
        }
    }
}

static esp_err_t cap_at_start_rx_task(void)
{
    if (s_at_rx_task) {
        return ESP_OK;
    }
    return xTaskCreate(cap_at_rx_task,
                       "at_uart1_rx",
                       4096,
                       NULL,
                       6,
                       &s_at_rx_task) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t cap_at_mqtt_uart1_configure(int tx_pin, int rx_pin, int baud_rate)
{
    uart_config_t uart_config = {
        .baud_rate = baud_rate > 0 ? baud_rate : CAP_AT_DEFAULT_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err;

    if (!cap_at_valid_baud(uart_config.baud_rate)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (tx_pin < 0) {
        tx_pin = CAP_AT_DEFAULT_TX_PIN;
    }
    if (rx_pin < 0) {
        rx_pin = CAP_AT_DEFAULT_RX_PIN;
    }

    ESP_RETURN_ON_ERROR(cap_at_init_lock(), TAG, "create lock failed");
    xSemaphoreTake(s_at_lock, portMAX_DELAY);

    if (s_uart_ready) {
        uart_driver_delete(CAP_AT_UART_NUM);
        s_uart_ready = false;
    }

    err = uart_param_config(CAP_AT_UART_NUM, &uart_config);
    if (err == ESP_OK) {
        err = uart_set_pin(CAP_AT_UART_NUM, tx_pin, rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }
    if (err == ESP_OK) {
        err = uart_driver_install(CAP_AT_UART_NUM, CAP_AT_RX_BUF_SIZE, CAP_AT_TX_BUF_SIZE, 0, NULL, 0);
    }
    if (err == ESP_OK) {
        s_tx_pin = tx_pin;
        s_rx_pin = rx_pin;
        s_baud_rate = uart_config.baud_rate;
        s_uart_ready = true;
        err = cap_at_start_rx_task();
    }
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "UART1 AT ready tx=%d rx=%d baud=%d", s_tx_pin, s_rx_pin, s_baud_rate);
    }

    xSemaphoreGive(s_at_lock);
    return err;
}

esp_err_t cap_at_mqtt_set_command_handler(cap_at_mqtt_command_handler_t handler, void *user_ctx)
{
    ESP_RETURN_ON_ERROR(cap_at_init_lock(), TAG, "create lock failed");
    xSemaphoreTake(s_at_lock, portMAX_DELAY);
    s_command_handler = handler;
    s_command_handler_user_ctx = user_ctx;
    xSemaphoreGive(s_at_lock);
    return ESP_OK;
}

esp_err_t cap_at_mqtt_expect_payload(size_t payload_len,
                                     cap_at_mqtt_payload_handler_t handler,
                                     void *user_ctx)
{
    uint8_t *payload = NULL;

    if (payload_len == 0 || !handler) {
        return ESP_ERR_INVALID_ARG;
    }
    if (payload_len > 8192) {
        return ESP_ERR_INVALID_SIZE;
    }

    payload = calloc(1, payload_len + 1);
    if (!payload) {
        return ESP_ERR_NO_MEM;
    }

    free(s_payload_buf);
    s_payload_buf = payload;
    s_payload_expected = payload_len;
    s_payload_used = 0;
    s_payload_handler = handler;
    s_payload_handler_user_ctx = user_ctx;
    return ESP_OK;
}

static esp_err_t cap_at_ensure_uart(void)
{
    if (s_uart_ready) {
        return ESP_OK;
    }
    return cap_at_mqtt_uart1_configure(s_tx_pin, s_rx_pin, s_baud_rate);
}

static int cap_at_read_response(char *out, size_t out_size, uint32_t timeout_ms)
{
    uint8_t tmp[128];
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    size_t used = 0;

    if (!out || out_size == 0) {
        return 0;
    }
    out[0] = '\0';

    while ((int32_t)(deadline - xTaskGetTickCount()) > 0 && used + 1 < out_size) {
        int got = uart_read_bytes(CAP_AT_UART_NUM,
                                  tmp,
                                  sizeof(tmp),
                                  pdMS_TO_TICKS(50));
        if (got <= 0) {
            continue;
        }
        if (used + (size_t)got >= out_size) {
            got = (int)(out_size - used - 1);
        }
        memcpy(out + used, tmp, (size_t)got);
        used += (size_t)got;
        out[used] = '\0';
        if (strstr(out, "\r\nOK\r\n") || strstr(out, "\r\nERROR\r\n") || strstr(out, ">")) {
            break;
        }
    }

    return (int)used;
}

static esp_err_t cap_at_write_raw_locked(const char *data, size_t len)
{
    int written;

    if (!data) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(cap_at_ensure_uart(), TAG, "uart not ready");
    written = uart_write_bytes(CAP_AT_UART_NUM, data, len);
    return written == (int)len ? ESP_OK : ESP_FAIL;
}

esp_err_t cap_at_mqtt_uart1_write_line(const char *line)
{
    esp_err_t err;

    if (!line) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(cap_at_init_lock(), TAG, "create lock failed");
    xSemaphoreTake(s_at_lock, portMAX_DELAY);
    err = cap_at_write_raw_locked(line, strlen(line));
    if (err == ESP_OK) {
        err = cap_at_write_raw_locked("\r\n", 2);
    }
    xSemaphoreGive(s_at_lock);
    return err;
}

static esp_err_t cap_at_transact(const char *command, uint32_t timeout_ms, char *response, size_t response_size)
{
    esp_err_t err;

    if (!command || !command[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(cap_at_init_lock(), TAG, "create lock failed");
    xSemaphoreTake(s_at_lock, portMAX_DELAY);
    err = cap_at_ensure_uart();
    if (err == ESP_OK) {
        uart_flush_input(CAP_AT_UART_NUM);
        err = cap_at_write_raw_locked(command, strlen(command));
    }
    if (err == ESP_OK) {
        err = cap_at_write_raw_locked("\r\n", 2);
    }
    if (err == ESP_OK && response && response_size > 0) {
        cap_at_read_response(response, response_size, timeout_ms ? timeout_ms : 3000);
    }
    xSemaphoreGive(s_at_lock);
    return err;
}

static esp_err_t cap_at_copy_quoted(char *dst, size_t dst_size, const char *src)
{
    size_t used = 0;

    if (!dst || dst_size < 3 || !src) {
        return ESP_ERR_INVALID_ARG;
    }
    dst[used++] = '"';
    for (size_t i = 0; src[i]; i++) {
        if (used + 3 >= dst_size) {
            return ESP_ERR_INVALID_SIZE;
        }
        if (src[i] == '"' || src[i] == '\\') {
            dst[used++] = '\\';
        }
        dst[used++] = src[i];
    }
    dst[used++] = '"';
    dst[used] = '\0';
    return ESP_OK;
}

static esp_err_t cap_at_write_response(char *output, size_t output_size, const char *prefix, const char *resp)
{
    if (!output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    snprintf(output, output_size, "%s%s%s", prefix ? prefix : "OK", resp && resp[0] ? "\n" : "", resp ? resp : "");
    return ESP_OK;
}

static esp_err_t cap_at_send_command_execute(const char *input_json,
                                             const claw_cap_call_context_t *ctx,
                                             char *output,
                                             size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json && input_json[0] ? input_json : "{}");
    const char *command = cap_at_json_string(root, "command", NULL);
    uint32_t timeout_ms = (uint32_t)cap_at_json_int(root, "timeout_ms", 3000);
    char response[CAP_AT_RESP_BUF_SIZE];
    esp_err_t err;

    (void)ctx;
    if (!root || !command || !command[0]) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: command is required.");
        return ESP_ERR_INVALID_ARG;
    }
    err = cap_at_transact(command, timeout_ms, response, sizeof(response));
    cJSON_Delete(root);
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: AT command failed: %s", esp_err_to_name(err));
        return err;
    }
    return cap_at_write_response(output, output_size, "OK: AT response", response);
}

static esp_err_t cap_at_uart1_configure_execute(const char *input_json,
                                                const claw_cap_call_context_t *ctx,
                                                char *output,
                                                size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json && input_json[0] ? input_json : "{}");
    int tx_pin = cap_at_json_int(root, "tx_pin", CAP_AT_DEFAULT_TX_PIN);
    int rx_pin = cap_at_json_int(root, "rx_pin", CAP_AT_DEFAULT_RX_PIN);
    int baud = cap_at_json_int(root, "baud", CAP_AT_DEFAULT_BAUD);
    esp_err_t err;

    (void)ctx;
    err = cap_at_mqtt_uart1_configure(tx_pin, rx_pin, baud);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: UART1 configure failed: %s", esp_err_to_name(err));
        return err;
    }
    snprintf(output, output_size, "OK: UART1 configured tx=%d rx=%d baud=%d", tx_pin, rx_pin, baud);
    return ESP_OK;
}

static esp_err_t cap_at_wifi_set_mode_execute(const char *input_json,
                                              const claw_cap_call_context_t *ctx,
                                              char *output,
                                              size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json && input_json[0] ? input_json : "{}");
    int mode = cap_at_json_int(root, "mode", 3);
    uint32_t timeout_ms = (uint32_t)cap_at_json_int(root, "timeout_ms", 3000);
    char command[32];
    char response[CAP_AT_RESP_BUF_SIZE];
    esp_err_t err;

    (void)ctx;
    cJSON_Delete(root);
    if (mode < 1 || mode > 3) {
        snprintf(output, output_size, "Error: mode must be 1, 2, or 3.");
        return ESP_ERR_INVALID_ARG;
    }
    snprintf(command, sizeof(command), "AT+CWMODE=%d", mode);
    err = cap_at_transact(command, timeout_ms, response, sizeof(response));
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: AT+CWMODE failed: %s", esp_err_to_name(err));
        return err;
    }
    return cap_at_write_response(output, output_size, "OK: Wi-Fi mode command sent", response);
}

static esp_err_t cap_at_wifi_join_ap_command(cJSON *root, char *command, size_t command_size)
{
    const char *ssid = cap_at_json_string(root, "ssid", NULL);
    const char *password = cap_at_json_string(root, "password", "");
    char q_ssid[96];
    char q_password[128];

    if (!ssid || !ssid[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(cap_at_copy_quoted(q_ssid, sizeof(q_ssid), ssid), TAG, "quote ssid");
    ESP_RETURN_ON_ERROR(cap_at_copy_quoted(q_password, sizeof(q_password), password), TAG, "quote password");
    return snprintf(command, command_size, "AT+CWJAP=%s,%s", q_ssid, q_password) < (int)command_size ?
           ESP_OK : ESP_ERR_INVALID_SIZE;
}

static esp_err_t cap_at_wifi_join_ap_execute(const char *input_json,
                                             const claw_cap_call_context_t *ctx,
                                             char *output,
                                             size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json && input_json[0] ? input_json : "{}");
    uint32_t timeout_ms = (uint32_t)cap_at_json_int(root, "timeout_ms", 20000);
    char command[CAP_AT_CMD_BUF_SIZE];
    char response[CAP_AT_RESP_BUF_SIZE];
    esp_err_t err;

    (void)ctx;
    err = root ? cap_at_wifi_join_ap_command(root, command, sizeof(command)) : ESP_ERR_INVALID_ARG;
    cJSON_Delete(root);
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: ssid is required or command is too long.");
        return err;
    }
    err = cap_at_transact(command, timeout_ms, response, sizeof(response));
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: AT+CWJAP failed: %s", esp_err_to_name(err));
        return err;
    }
    return cap_at_write_response(output, output_size, "OK: Wi-Fi join command sent", response);
}

static esp_err_t cap_at_wifi_provision_sta_execute(const char *input_json,
                                                   const claw_cap_call_context_t *ctx,
                                                   char *output,
                                                   size_t output_size)
{
    char response[CAP_AT_RESP_BUF_SIZE];
    esp_err_t err;

    (void)ctx;
    err = cap_at_transact("AT+CWMODE=1", 3000, response, sizeof(response));
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: AT+CWMODE=1 failed: %s", esp_err_to_name(err));
        return err;
    }
    return cap_at_wifi_join_ap_execute(input_json, ctx, output, output_size);
}

static esp_err_t cap_at_mqtt_usercfg_execute(const char *input_json,
                                             const claw_cap_call_context_t *ctx,
                                             char *output,
                                             size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json && input_json[0] ? input_json : "{}");
    const char *client_id = cap_at_json_string(root, "client_id", NULL);
    const char *username = cap_at_json_string(root, "username", NULL);
    const char *password = cap_at_json_string(root, "password", NULL);
    const char *path = cap_at_json_string(root, "path", "");
    int link_id = cap_at_json_int(root, "link_id", 0);
    int scheme = cap_at_json_int(root, "scheme", 1);
    int cert_key_id = cap_at_json_int(root, "cert_key_id", 0);
    int ca_id = cap_at_json_int(root, "ca_id", 0);
    char q_client_id[160], q_username[160], q_password[160], q_path[96];
    char command[CAP_AT_CMD_BUF_SIZE];
    char response[CAP_AT_RESP_BUF_SIZE];
    esp_err_t err;

    (void)ctx;
    if (!root || !client_id || !username || !password) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: client_id, username, and password are required.");
        return ESP_ERR_INVALID_ARG;
    }
    err = cap_at_copy_quoted(q_client_id, sizeof(q_client_id), client_id);
    if (err == ESP_OK) err = cap_at_copy_quoted(q_username, sizeof(q_username), username);
    if (err == ESP_OK) err = cap_at_copy_quoted(q_password, sizeof(q_password), password);
    if (err == ESP_OK) err = cap_at_copy_quoted(q_path, sizeof(q_path), path);
    if (err == ESP_OK && snprintf(command, sizeof(command),
                                  "AT+MQTTUSERCFG=%d,%d,%s,%s,%s,%d,%d,%s",
                                  link_id, scheme, q_client_id, q_username, q_password,
                                  cert_key_id, ca_id, q_path) >= (int)sizeof(command)) {
        err = ESP_ERR_INVALID_SIZE;
    }
    cJSON_Delete(root);
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: MQTTUSERCFG command too long.");
        return err;
    }
    err = cap_at_transact(command, 5000, response, sizeof(response));
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: AT+MQTTUSERCFG failed: %s", esp_err_to_name(err));
        return err;
    }
    return cap_at_write_response(output, output_size, "OK: MQTT user config sent", response);
}

static esp_err_t cap_at_mqtt_connect_execute(const char *input_json,
                                             const claw_cap_call_context_t *ctx,
                                             char *output,
                                             size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json && input_json[0] ? input_json : "{}");
    const char *host = cap_at_json_string(root, "host", NULL);
    int link_id = cap_at_json_int(root, "link_id", 0);
    int port = cap_at_json_int(root, "port", 1883);
    int reconnect = cap_at_json_int(root, "reconnect", 0);
    char q_host[192];
    char command[CAP_AT_CMD_BUF_SIZE];
    char response[CAP_AT_RESP_BUF_SIZE];
    esp_err_t err;

    (void)ctx;
    if (!root || !host || !host[0]) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: host is required.");
        return ESP_ERR_INVALID_ARG;
    }
    err = cap_at_copy_quoted(q_host, sizeof(q_host), host);
    if (err == ESP_OK && snprintf(command, sizeof(command), "AT+MQTTCONN=%d,%s,%d,%d",
                                  link_id, q_host, port, reconnect) >= (int)sizeof(command)) {
        err = ESP_ERR_INVALID_SIZE;
    }
    cJSON_Delete(root);
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: MQTTCONN command too long.");
        return err;
    }
    err = cap_at_transact(command, 10000, response, sizeof(response));
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: AT+MQTTCONN failed: %s", esp_err_to_name(err));
        return err;
    }
    return cap_at_write_response(output, output_size, "OK: MQTT connect sent", response);
}

static esp_err_t cap_at_mqtt_subscribe_execute(const char *input_json,
                                               const claw_cap_call_context_t *ctx,
                                               char *output,
                                               size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json && input_json[0] ? input_json : "{}");
    const char *topic = cap_at_json_string(root, "topic", NULL);
    int link_id = cap_at_json_int(root, "link_id", 0);
    int qos = cap_at_json_int(root, "qos", 1);
    char q_topic[320];
    char command[CAP_AT_CMD_BUF_SIZE];
    char response[CAP_AT_RESP_BUF_SIZE];
    esp_err_t err;

    (void)ctx;
    if (!root || !topic || !topic[0]) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: topic is required.");
        return ESP_ERR_INVALID_ARG;
    }
    err = cap_at_copy_quoted(q_topic, sizeof(q_topic), topic);
    if (err == ESP_OK && snprintf(command, sizeof(command), "AT+MQTTSUB=%d,%s,%d",
                                  link_id, q_topic, qos) >= (int)sizeof(command)) {
        err = ESP_ERR_INVALID_SIZE;
    }
    cJSON_Delete(root);
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: MQTTSUB command too long.");
        return err;
    }
    err = cap_at_transact(command, 5000, response, sizeof(response));
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: AT+MQTTSUB failed: %s", esp_err_to_name(err));
        return err;
    }
    return cap_at_write_response(output, output_size, "OK: MQTT subscribe sent", response);
}

static esp_err_t cap_at_mqtt_publish_raw_execute(const char *input_json,
                                                 const claw_cap_call_context_t *ctx,
                                                 char *output,
                                                 size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json && input_json[0] ? input_json : "{}");
    const char *topic = cap_at_json_string(root, "topic", NULL);
    const char *data = cap_at_json_string(root, "data", NULL);
    int link_id = cap_at_json_int(root, "link_id", 0);
    int qos = cap_at_json_int(root, "qos", 1);
    int retain = cap_at_json_int(root, "retain", 0);
    char q_topic[320];
    char command[CAP_AT_CMD_BUF_SIZE];
    char response[CAP_AT_RESP_BUF_SIZE];
    esp_err_t err;

    (void)ctx;
    if (!root || !topic || !data) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: topic and data are required.");
        return ESP_ERR_INVALID_ARG;
    }
    err = cap_at_copy_quoted(q_topic, sizeof(q_topic), topic);
    if (err == ESP_OK && snprintf(command, sizeof(command), "AT+MQTTPUBRAW=%d,%s,%u,%d,%d",
                                  link_id, q_topic, (unsigned)strlen(data), qos, retain) >= (int)sizeof(command)) {
        err = ESP_ERR_INVALID_SIZE;
    }
    if (err == ESP_OK) {
        ESP_RETURN_ON_ERROR(cap_at_init_lock(), TAG, "create lock failed");
        xSemaphoreTake(s_at_lock, portMAX_DELAY);
        err = cap_at_ensure_uart();
        if (err == ESP_OK) {
            uart_flush_input(CAP_AT_UART_NUM);
            err = cap_at_write_raw_locked(command, strlen(command));
        }
        if (err == ESP_OK) {
            err = cap_at_write_raw_locked("\r\n", 2);
        }
        if (err == ESP_OK) {
            cap_at_read_response(response, sizeof(response), 3000);
            if (!strstr(response, ">")) {
                err = ESP_FAIL;
            }
        }
        if (err == ESP_OK) {
            err = cap_at_write_raw_locked(data, strlen(data));
            if (err == ESP_OK) {
                cap_at_read_response(response, sizeof(response), 5000);
            }
        }
        xSemaphoreGive(s_at_lock);
    }
    cJSON_Delete(root);
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: AT+MQTTPUBRAW failed: %s", esp_err_to_name(err));
        return err;
    }
    return cap_at_write_response(output, output_size, "OK: MQTT raw publish sent", response);
}

static esp_err_t cap_at_mqtt_huawei_setup_execute(const char *input_json,
                                                  const claw_cap_call_context_t *ctx,
                                                  char *output,
                                                  size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json && input_json[0] ? input_json : "{}");
    const char *response_topic = cap_at_json_string(root, "response_topic", NULL);
    char step_output[CAP_AT_RESP_BUF_SIZE];
    esp_err_t err;

    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input.");
        return ESP_ERR_INVALID_ARG;
    }

    err = cap_at_mqtt_usercfg_execute(input_json, ctx, step_output, sizeof(step_output));
    if (err == ESP_OK) {
        err = cap_at_mqtt_connect_execute(input_json, ctx, step_output, sizeof(step_output));
    }
    if (err == ESP_OK && response_topic && response_topic[0]) {
        cJSON_ReplaceItemInObject(root, "topic", cJSON_CreateString(response_topic));
        char *rendered = cJSON_PrintUnformatted(root);
        if (!rendered) {
            err = ESP_ERR_NO_MEM;
        } else {
            err = cap_at_mqtt_subscribe_execute(rendered, ctx, step_output, sizeof(step_output));
            free(rendered);
        }
    }
    cJSON_Delete(root);
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: Huawei MQTT setup failed: %s", esp_err_to_name(err));
        return err;
    }
    snprintf(output, output_size, "OK: Huawei MQTT user config/connect%s completed",
             response_topic && response_topic[0] ? "/subscribe" : "");
    return ESP_OK;
}

esp_err_t cap_at_mqtt_notify_wifi_status(bool connected,
                                         const char *sta_ip,
                                         const char *mode,
                                         const char *ap_ssid)
{
    char line[192];

    if (connected) {
        snprintf(line, sizeof(line), "+CLAWPROV:DONE,1,\"%s\",\"%s\",\"%s\"",
                 sta_ip && sta_ip[0] ? sta_ip : "0.0.0.0",
                 mode && mode[0] ? mode : "sta_ok",
                 ap_ssid && ap_ssid[0] ? ap_ssid : "");
    } else {
        snprintf(line, sizeof(line), "+CLAWPROV:WAITING,\"%s\",\"%s\"",
                 mode && mode[0] ? mode : "apsta",
                 ap_ssid && ap_ssid[0] ? ap_ssid : "");
    }
    return cap_at_mqtt_uart1_write_line(line);
}

static const claw_cap_descriptor_t s_at_descriptors[] = {
    {
        .id = "at_uart1_configure",
        .name = "at_uart1_configure",
        .family = "at_mqtt",
        .description = "Configure the UART1 AT command channel.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"tx_pin\":{\"type\":\"integer\"},\"rx_pin\":{\"type\":\"integer\"},\"baud\":{\"type\":\"integer\"}}}",
        .execute = cap_at_uart1_configure_execute,
    },
    {
        .id = "at_send_command",
        .name = "at_send_command",
        .family = "at_mqtt",
        .description = "Send an AT command over UART1 and return the response.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\"},\"timeout_ms\":{\"type\":\"integer\"}},\"required\":[\"command\"]}",
        .execute = cap_at_send_command_execute,
    },
    {
        .id = "at_wifi_set_mode",
        .name = "at_wifi_set_mode",
        .family = "at_mqtt",
        .description = "Send AT+CWMODE over UART1. mode=1 STA, 2 AP, 3 STA+AP.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"mode\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":3},\"timeout_ms\":{\"type\":\"integer\"}}}",
        .execute = cap_at_wifi_set_mode_execute,
    },
    {
        .id = "at_wifi_join_ap",
        .name = "at_wifi_join_ap",
        .family = "at_mqtt",
        .description = "Send AT+CWJAP with runtime-provided SSID and password over UART1.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"ssid\":{\"type\":\"string\"},\"password\":{\"type\":\"string\"},\"timeout_ms\":{\"type\":\"integer\"}},\"required\":[\"ssid\"]}",
        .execute = cap_at_wifi_join_ap_execute,
    },
    {
        .id = "at_wifi_provision_sta",
        .name = "at_wifi_provision_sta",
        .family = "at_mqtt",
        .description = "Set AT Wi-Fi mode to STA and join an AP using runtime-provided credentials.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"ssid\":{\"type\":\"string\"},\"password\":{\"type\":\"string\"},\"timeout_ms\":{\"type\":\"integer\"}},\"required\":[\"ssid\"]}",
        .execute = cap_at_wifi_provision_sta_execute,
    },
    {
        .id = "at_mqtt_usercfg",
        .name = "at_mqtt_usercfg",
        .family = "at_mqtt",
        .description = "Send AT+MQTTUSERCFG over UART1.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"link_id\":{\"type\":\"integer\"},\"scheme\":{\"type\":\"integer\"},\"client_id\":{\"type\":\"string\"},\"username\":{\"type\":\"string\"},\"password\":{\"type\":\"string\"},\"cert_key_id\":{\"type\":\"integer\"},\"ca_id\":{\"type\":\"integer\"},\"path\":{\"type\":\"string\"}},\"required\":[\"client_id\",\"username\",\"password\"]}",
        .execute = cap_at_mqtt_usercfg_execute,
    },
    {
        .id = "at_mqtt_connect",
        .name = "at_mqtt_connect",
        .family = "at_mqtt",
        .description = "Send AT+MQTTCONN over UART1.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"link_id\":{\"type\":\"integer\"},\"host\":{\"type\":\"string\"},\"port\":{\"type\":\"integer\"},\"reconnect\":{\"type\":\"integer\"}},\"required\":[\"host\"]}",
        .execute = cap_at_mqtt_connect_execute,
    },
    {
        .id = "at_mqtt_subscribe",
        .name = "at_mqtt_subscribe",
        .family = "at_mqtt",
        .description = "Send AT+MQTTSUB over UART1.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"link_id\":{\"type\":\"integer\"},\"topic\":{\"type\":\"string\"},\"qos\":{\"type\":\"integer\"}},\"required\":[\"topic\"]}",
        .execute = cap_at_mqtt_subscribe_execute,
    },
    {
        .id = "at_mqtt_publish_raw",
        .name = "at_mqtt_publish_raw",
        .family = "at_mqtt",
        .description = "Send AT+MQTTPUBRAW and payload over UART1.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"link_id\":{\"type\":\"integer\"},\"topic\":{\"type\":\"string\"},\"data\":{\"type\":\"string\"},\"qos\":{\"type\":\"integer\"},\"retain\":{\"type\":\"integer\"}},\"required\":[\"topic\",\"data\"]}",
        .execute = cap_at_mqtt_publish_raw_execute,
    },
    {
        .id = "at_mqtt_huawei_setup",
        .name = "at_mqtt_huawei_setup",
        .family = "at_mqtt",
        .description = "Configure and connect Huawei IoTDA MQTT over UART1 using runtime-provided credentials.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"link_id\":{\"type\":\"integer\"},\"scheme\":{\"type\":\"integer\"},\"client_id\":{\"type\":\"string\"},\"username\":{\"type\":\"string\"},\"password\":{\"type\":\"string\"},\"host\":{\"type\":\"string\"},\"port\":{\"type\":\"integer\"},\"reconnect\":{\"type\":\"integer\"},\"response_topic\":{\"type\":\"string\"}},\"required\":[\"client_id\",\"username\",\"password\",\"host\"]}",
        .execute = cap_at_mqtt_huawei_setup_execute,
    },
};

static const claw_cap_group_t s_at_group = {
    .group_id = "cap_at_mqtt",
    .plugin_name = "AT MQTT",
    .version = "1.0.0",
    .descriptors = s_at_descriptors,
    .descriptor_count = sizeof(s_at_descriptors) / sizeof(s_at_descriptors[0]),
};

esp_err_t cap_at_mqtt_register_group(void)
{
    if (claw_cap_group_exists(s_at_group.group_id)) {
        return ESP_OK;
    }
    return claw_cap_register_group(&s_at_group);
}
