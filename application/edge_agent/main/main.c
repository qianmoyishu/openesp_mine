/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "app_claw.h"
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "wifi_manager.h"
#include "wear_levelling.h"
#include "time.h"
#include "nvs_flash.h"
#include "http_server.h"
#include "esp_vfs_fat.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_system.h"
#include "esp_board_manager_includes.h"
#include "captive_dns.h"
#include "cmd_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#if CONFIG_APP_CLAW_CAP_IM_WECHAT
#include "cap_im_wechat.h"
#endif
#if CONFIG_APP_CLAW_CAP_AT_MQTT
#include "cap_at_mqtt.h"
#include "mqtt_client.h"
#endif
#include "app_config.h"

#define APP_FATFS_PARTITION_LABEL "storage"
#define APP_ENABLE_MEM_LOG        (0)

static const char *TAG = "app";

static app_config_t *s_config;
static app_claw_config_t *s_claw_config;
static app_claw_storage_paths_t *s_claw_paths;

static const char *app_fatfs_base_path = "/fatfs";

static wl_handle_t s_wl_handle = WL_INVALID_HANDLE;

static esp_err_t app_configure_sta_dns(void);

static esp_err_t app_allocate_runtime_state(void)
{
    if (!s_config) {
        s_config = calloc(1, sizeof(*s_config));
    }
    if (!s_claw_config) {
        s_claw_config = calloc(1, sizeof(*s_claw_config));
    }
    if (!s_claw_paths) {
        s_claw_paths = calloc(1, sizeof(*s_claw_paths));
    }

    if (!s_config || !s_claw_config || !s_claw_paths) {
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

static void app_free_runtime_state(void)
{
    free(s_claw_paths);
    s_claw_paths = NULL;

    free(s_claw_config);
    s_claw_config = NULL;

    free(s_config);
    s_config = NULL;
}

#if CONFIG_APP_CLAW_CAP_AT_MQTT && CONFIG_APP_AT_UART1_NOTIFY_ENABLE
static void app_at_trim_in_place(char *value)
{
    char *begin = value;
    char *end;

    if (!value) {
        return;
    }
    while (*begin == ' ' || *begin == '\t') {
        begin++;
    }
    if (begin != value) {
        memmove(value, begin, strlen(begin) + 1);
    }
    end = value + strlen(value);
    while (end > value && (end[-1] == ' ' || end[-1] == '\t')) {
        end--;
    }
    *end = '\0';
}

static esp_err_t app_at_parse_quoted_field(const char **cursor, char *out, size_t out_size)
{
    const char *p;
    size_t used = 0;

    if (!cursor || !*cursor || !out || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    p = *cursor;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p != '"') {
        return ESP_ERR_INVALID_ARG;
    }
    p++;
    while (*p && *p != '"') {
        if (*p == '\\' && p[1]) {
            p++;
        }
        if (used + 1 >= out_size) {
            return ESP_ERR_INVALID_SIZE;
        }
        out[used++] = *p++;
    }
    if (*p != '"') {
        return ESP_ERR_INVALID_ARG;
    }
    out[used] = '\0';
    *cursor = p + 1;
    return ESP_OK;
}

static esp_err_t app_at_parse_cwjap(const char *args,
                                    char *ssid,
                                    size_t ssid_size,
                                    char *password,
                                    size_t password_size)
{
    const char *p = args;
    esp_err_t err;

    if (!p || *p != '=') {
        return ESP_ERR_INVALID_ARG;
    }
    p++;
    err = app_at_parse_quoted_field(&p, ssid, ssid_size);
    if (err != ESP_OK) {
        return err;
    }
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p != ',') {
        return ESP_ERR_INVALID_ARG;
    }
    p++;
    return app_at_parse_quoted_field(&p, password, password_size);
}

#define APP_AT_MQTT_CONNECTED_BIT    BIT0
#define APP_AT_MQTT_DISCONNECTED_BIT BIT1
#define APP_AT_MQTT_FIELD_LEN        256
#define APP_AT_MQTT_URI_LEN          384

static EventGroupHandle_t s_at_mqtt_events;
static esp_mqtt_client_handle_t s_at_mqtt_client;
static char s_at_mqtt_client_id[APP_AT_MQTT_FIELD_LEN];
static char s_at_mqtt_username[APP_AT_MQTT_FIELD_LEN];
static char s_at_mqtt_password[APP_AT_MQTT_FIELD_LEN];
static char s_at_mqtt_pub_topic[APP_AT_MQTT_FIELD_LEN];
static int s_at_mqtt_pub_qos = 1;
static int s_at_mqtt_pub_retain;

static esp_err_t app_at_mqtt_ensure_events(void)
{
    if (!s_at_mqtt_events) {
        s_at_mqtt_events = xEventGroupCreate();
        if (!s_at_mqtt_events) {
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

static esp_err_t app_at_parse_int_field(const char **cursor, int *out)
{
    char *end = NULL;
    long value;

    if (!cursor || !*cursor || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    while (**cursor == ' ' || **cursor == '\t') {
        (*cursor)++;
    }
    value = strtol(*cursor, &end, 10);
    if (end == *cursor) {
        return ESP_ERR_INVALID_ARG;
    }
    *cursor = end;
    *out = (int)value;
    return ESP_OK;
}

static esp_err_t app_at_parse_comma(const char **cursor)
{
    if (!cursor || !*cursor) {
        return ESP_ERR_INVALID_ARG;
    }
    while (**cursor == ' ' || **cursor == '\t') {
        (*cursor)++;
    }
    if (**cursor != ',') {
        return ESP_ERR_INVALID_ARG;
    }
    (*cursor)++;
    return ESP_OK;
}

static bool app_at_mqtt_is_connected(void)
{
    EventBits_t bits;

    if (!s_at_mqtt_events) {
        return false;
    }
    bits = xEventGroupGetBits(s_at_mqtt_events);
    return (bits & APP_AT_MQTT_CONNECTED_BIT) != 0;
}

static void app_at_mqtt_event_handler(void *handler_args,
                                      esp_event_base_t base,
                                      int32_t event_id,
                                      void *event_data)
{
    (void)handler_args;
    (void)base;
    (void)event_data;

    if (!s_at_mqtt_events) {
        return;
    }

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        xEventGroupSetBits(s_at_mqtt_events, APP_AT_MQTT_CONNECTED_BIT);
        xEventGroupClearBits(s_at_mqtt_events, APP_AT_MQTT_DISCONNECTED_BIT);
        cap_at_mqtt_uart1_write_line("+MQTTCONNECTED:0,1");
        break;
    case MQTT_EVENT_DISCONNECTED:
        xEventGroupClearBits(s_at_mqtt_events, APP_AT_MQTT_CONNECTED_BIT);
        xEventGroupSetBits(s_at_mqtt_events, APP_AT_MQTT_DISCONNECTED_BIT);
        cap_at_mqtt_uart1_write_line("+MQTTDISCONNECTED:0");
        break;
    default:
        break;
    }
}

static esp_err_t app_at_mqtt_usercfg_command(const char *line,
                                             char *response,
                                             size_t response_size)
{
    const char *p = strchr(line, '=');
    int link_id = 0;
    int scheme = 1;
    esp_err_t err;

    if (!p || p[1] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    p++;

    err = app_at_parse_int_field(&p, &link_id);
    if (err == ESP_OK) err = app_at_parse_comma(&p);
    if (err == ESP_OK) err = app_at_parse_int_field(&p, &scheme);
    if (err == ESP_OK) err = app_at_parse_comma(&p);
    if (err == ESP_OK) err = app_at_parse_quoted_field(&p, s_at_mqtt_client_id, sizeof(s_at_mqtt_client_id));
    if (err == ESP_OK) err = app_at_parse_comma(&p);
    if (err == ESP_OK) err = app_at_parse_quoted_field(&p, s_at_mqtt_username, sizeof(s_at_mqtt_username));
    if (err == ESP_OK) err = app_at_parse_comma(&p);
    if (err == ESP_OK) err = app_at_parse_quoted_field(&p, s_at_mqtt_password, sizeof(s_at_mqtt_password));
    if (err != ESP_OK || link_id != 0) {
        snprintf(response, response_size, "+MQTTUSERCFG:BAD_ARGS");
        return err != ESP_OK ? err : ESP_ERR_NOT_SUPPORTED;
    }
    if (scheme != 1) {
        snprintf(response, response_size, "+MQTTUSERCFG:SCHEME_UNSUPPORTED");
        return ESP_ERR_NOT_SUPPORTED;
    }
    snprintf(response, response_size, "+MQTTUSERCFG:0");
    return ESP_OK;
}

static esp_err_t app_at_mqtt_conn_command(const char *line,
                                          char *response,
                                          size_t response_size)
{
    const char *p = strchr(line, '=');
    char host[APP_AT_MQTT_FIELD_LEN] = {0};
    char uri[APP_AT_MQTT_URI_LEN] = {0};
    int link_id = 0;
    int port = 1883;
    int reconnect = 0;
    esp_err_t err;

    if (!p || p[1] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    p++;

    err = app_at_parse_int_field(&p, &link_id);
    if (err == ESP_OK) err = app_at_parse_comma(&p);
    if (err == ESP_OK) err = app_at_parse_quoted_field(&p, host, sizeof(host));
    if (err == ESP_OK) err = app_at_parse_comma(&p);
    if (err == ESP_OK) err = app_at_parse_int_field(&p, &port);
    if (err == ESP_OK) err = app_at_parse_comma(&p);
    if (err == ESP_OK) err = app_at_parse_int_field(&p, &reconnect);
    if (err != ESP_OK || link_id != 0 || !host[0] || port <= 0) {
        snprintf(response, response_size, "+MQTTCONN:BAD_ARGS");
        return err != ESP_OK ? err : ESP_ERR_INVALID_ARG;
    }

    err = app_at_mqtt_ensure_events();
    if (err != ESP_OK) {
        return err;
    }
    xEventGroupClearBits(s_at_mqtt_events, APP_AT_MQTT_CONNECTED_BIT | APP_AT_MQTT_DISCONNECTED_BIT);

    if (s_at_mqtt_client) {
        esp_mqtt_client_stop(s_at_mqtt_client);
        esp_mqtt_client_destroy(s_at_mqtt_client);
        s_at_mqtt_client = NULL;
    }

    snprintf(uri, sizeof(uri), "mqtt://%s:%d", host, port);
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = uri,
        .credentials.client_id = s_at_mqtt_client_id,
        .credentials.username = s_at_mqtt_username,
        .credentials.authentication.password = s_at_mqtt_password,
        .network.reconnect_timeout_ms = reconnect ? 10000 : 0,
    };

    s_at_mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    if (!s_at_mqtt_client) {
        snprintf(response, response_size, "+MQTTCONN:INIT_FAILED");
        return ESP_FAIL;
    }
    esp_mqtt_client_register_event(s_at_mqtt_client,
                                   ESP_EVENT_ANY_ID,
                                   app_at_mqtt_event_handler,
                                   NULL);

    err = esp_mqtt_client_start(s_at_mqtt_client);
    if (err != ESP_OK) {
        snprintf(response, response_size, "+MQTTCONN:START_FAILED");
        return err;
    }

    EventBits_t bits = xEventGroupWaitBits(s_at_mqtt_events,
                                           APP_AT_MQTT_CONNECTED_BIT | APP_AT_MQTT_DISCONNECTED_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           pdMS_TO_TICKS(15000));
    if ((bits & APP_AT_MQTT_CONNECTED_BIT) == 0) {
        snprintf(response, response_size, "+MQTTCONN:TIMEOUT");
        return ESP_ERR_TIMEOUT;
    }
    snprintf(response, response_size, "+MQTTCONN:0,0");
    return ESP_OK;
}

static esp_err_t app_at_mqtt_sub_command(const char *line,
                                         char *response,
                                         size_t response_size)
{
    const char *p = strchr(line, '=');
    char topic[APP_AT_MQTT_FIELD_LEN] = {0};
    int link_id = 0;
    int qos = 1;
    int msg_id;
    esp_err_t err;

    if (!p || p[1] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    p++;

    err = app_at_parse_int_field(&p, &link_id);
    if (err == ESP_OK) err = app_at_parse_comma(&p);
    if (err == ESP_OK) err = app_at_parse_quoted_field(&p, topic, sizeof(topic));
    if (err == ESP_OK) err = app_at_parse_comma(&p);
    if (err == ESP_OK) err = app_at_parse_int_field(&p, &qos);
    if (err != ESP_OK || link_id != 0 || !topic[0] || !app_at_mqtt_is_connected()) {
        snprintf(response, response_size, "+MQTTSUB:FAILED");
        return err != ESP_OK ? err : ESP_ERR_INVALID_STATE;
    }

    msg_id = esp_mqtt_client_subscribe(s_at_mqtt_client, topic, qos);
    if (msg_id < 0) {
        snprintf(response, response_size, "+MQTTSUB:FAILED");
        return ESP_FAIL;
    }
    snprintf(response, response_size, "+MQTTSUB:0,%d", msg_id);
    return ESP_OK;
}

static esp_err_t app_at_mqtt_payload_handler(const uint8_t *payload,
                                             size_t payload_len,
                                             char *response,
                                             size_t response_size,
                                             void *user_ctx)
{
    int msg_id;

    (void)user_ctx;

    if (!payload || payload_len == 0 || !app_at_mqtt_is_connected()) {
        snprintf(response, response_size, "+MQTTPUB:FAILED");
        return ESP_ERR_INVALID_STATE;
    }

    msg_id = esp_mqtt_client_publish(s_at_mqtt_client,
                                     s_at_mqtt_pub_topic,
                                     (const char *)payload,
                                     (int)payload_len,
                                     s_at_mqtt_pub_qos,
                                     s_at_mqtt_pub_retain);
    if (msg_id < 0) {
        snprintf(response, response_size, "+MQTTPUB:FAILED");
        return ESP_FAIL;
    }
    snprintf(response, response_size, "+MQTTPUB:0,%d", msg_id);
    return ESP_OK;
}

static esp_err_t app_at_mqtt_pubraw_command(const char *line,
                                            char *response,
                                            size_t response_size)
{
    const char *p = strchr(line, '=');
    int link_id = 0;
    int payload_len = 0;
    int qos = 1;
    int retain = 0;
    esp_err_t err;

    if (!p || p[1] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    p++;

    err = app_at_parse_int_field(&p, &link_id);
    if (err == ESP_OK) err = app_at_parse_comma(&p);
    if (err == ESP_OK) err = app_at_parse_quoted_field(&p, s_at_mqtt_pub_topic, sizeof(s_at_mqtt_pub_topic));
    if (err == ESP_OK) err = app_at_parse_comma(&p);
    if (err == ESP_OK) err = app_at_parse_int_field(&p, &payload_len);
    if (err == ESP_OK) err = app_at_parse_comma(&p);
    if (err == ESP_OK) err = app_at_parse_int_field(&p, &qos);
    if (err == ESP_OK) err = app_at_parse_comma(&p);
    if (err == ESP_OK) err = app_at_parse_int_field(&p, &retain);
    if (err != ESP_OK || link_id != 0 || payload_len <= 0 || !s_at_mqtt_pub_topic[0] ||
            !app_at_mqtt_is_connected()) {
        snprintf(response, response_size, "+MQTTPUBRAW:FAILED");
        return err != ESP_OK ? err : ESP_ERR_INVALID_STATE;
    }

    s_at_mqtt_pub_qos = qos;
    s_at_mqtt_pub_retain = retain;
    err = cap_at_mqtt_expect_payload((size_t)payload_len, app_at_mqtt_payload_handler, NULL);
    if (err != ESP_OK) {
        snprintf(response, response_size, "+MQTTPUBRAW:FAILED");
        return err;
    }
    snprintf(response, response_size, ">");
    return ESP_OK;
}

static void app_at_fill_cwmode_response(char *response, size_t response_size)
{
    wifi_manager_status_t status = {0};
    int mode = 2;

    wifi_manager_get_status(&status);
    if (status.ap_active && status.sta_configured) {
        mode = 3;
    } else if (!status.ap_active && status.sta_configured) {
        mode = 1;
    }
    snprintf(response, response_size, "+CWMODE:%d", mode);
}

static esp_err_t app_at_command_handler(const char *command,
                                        char *response,
                                        size_t response_size,
                                        void *user_ctx)
{
    char line[256];

    (void)user_ctx;

    if (!command || !response || response_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    strlcpy(line, command, sizeof(line));
    app_at_trim_in_place(line);

    if (strcmp(line, "AT") == 0 || strcmp(line, "ATE0") == 0 || strcmp(line, "ATE1") == 0) {
        return ESP_OK;
    }

    if (strcmp(line, "AT+GMR") == 0) {
        snprintf(response, response_size, "ESP-Claw AT UART1");
        return ESP_OK;
    }

    if (strcmp(line, "AT+CWMODE?") == 0) {
        app_at_fill_cwmode_response(response, response_size);
        return ESP_OK;
    }

    if (strncmp(line, "AT+CWMODE=", 10) == 0) {
        int mode = atoi(line + 10);

        if (mode < 1 || mode > 3) {
            snprintf(response, response_size, "+CWMODE:INVALID");
            return ESP_ERR_INVALID_ARG;
        }
        if (mode != 3) {
            snprintf(response,
                     response_size,
                     "+CWMODE:%d accepted; ESP-Claw keeps provisioning AP available",
                     mode);
        }
        return ESP_OK;
    }

    if (strcmp(line, "AT+CWJAP?") == 0) {
        wifi_manager_status_t status = {0};
        wifi_manager_get_status(&status);
        snprintf(response,
                 response_size,
                 "+CWJAP:%s\"%s\"",
                 status.sta_connected ? "" : "DISCONNECTED,",
                 s_config && s_config->wifi_ssid[0] ? s_config->wifi_ssid : "");
        return ESP_OK;
    }

    if (strncmp(line, "AT+CWJAP=", 9) == 0) {
        char ssid[APP_CONFIG_STR_LEN] = {0};
        char password[APP_CONFIG_STR_LEN] = {0};
        esp_err_t err = app_at_parse_cwjap(line + 8, ssid, sizeof(ssid), password, sizeof(password));

        if (err != ESP_OK) {
            snprintf(response, response_size, "+CWJAP:BAD_ARGS");
            return err;
        }
        if (!s_config) {
            snprintf(response, response_size, "+CWJAP:CONFIG_NOT_READY");
            return ESP_ERR_INVALID_STATE;
        }

        strlcpy(s_config->wifi_ssid, ssid, sizeof(s_config->wifi_ssid));
        strlcpy(s_config->wifi_password, password, sizeof(s_config->wifi_password));
        err = app_config_save(s_config);
        if (err != ESP_OK) {
            snprintf(response, response_size, "+CWJAP:SAVE_FAILED");
            return err;
        }

        err = wifi_manager_apply_sta_config(&(wifi_manager_config_t) {
            .sta_ssid = s_config->wifi_ssid,
            .sta_password = s_config->wifi_password,
        });
        if (err != ESP_OK) {
            snprintf(response, response_size, "+CWJAP:APPLY_FAILED");
            return err;
        }

        if (ssid[0]) {
            err = wifi_manager_wait_connected(30000);
            if (err != ESP_OK) {
                snprintf(response, response_size, "+CWJAP:CONNECT_FAILED");
                return err;
            }
            app_configure_sta_dns();
            snprintf(response, response_size, "WIFI CONNECTED\r\nWIFI GOT IP");
        } else {
            snprintf(response, response_size, "WIFI DISCONNECT");
        }
        return ESP_OK;
    }

    if (strncmp(line, "AT+MQTTUSERCFG=", 15) == 0) {
        return app_at_mqtt_usercfg_command(line, response, response_size);
    }

    if (strncmp(line, "AT+MQTTCONN=", 12) == 0) {
        return app_at_mqtt_conn_command(line, response, response_size);
    }

    if (strncmp(line, "AT+MQTTSUB=", 11) == 0) {
        return app_at_mqtt_sub_command(line, response, response_size);
    }

    if (strncmp(line, "AT+MQTTPUBRAW=", 14) == 0) {
        return app_at_mqtt_pubraw_command(line, response, response_size);
    }

    snprintf(response, response_size, "Unsupported AT command: %s", line);
    return ESP_ERR_NOT_SUPPORTED;
}

static void app_notify_host_wifi_status(const wifi_manager_status_t *status)
{
    if (!status) {
        return;
    }

    esp_err_t err = cap_at_mqtt_notify_wifi_status(status->sta_connected,
                                                   status->sta_ip,
                                                   status->mode,
                                                   status->ap_active ? status->ap_ssid : "");
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to notify host over UART1: %s", esp_err_to_name(err));
    }
}

static esp_err_t app_init_at_uart1_notify(void)
{
    esp_err_t err = cap_at_mqtt_set_command_handler(app_at_command_handler, NULL);
    if (err != ESP_OK) {
        return err;
    }

    err = cap_at_mqtt_uart1_configure(CONFIG_APP_AT_UART1_TX_PIN,
                                                CONFIG_APP_AT_UART1_RX_PIN,
                                                CONFIG_APP_AT_UART1_BAUD);
    if (err != ESP_OK) {
        return err;
    }

    wifi_manager_status_t status = {0};
    wifi_manager_get_status(&status);
    app_notify_host_wifi_status(&status);
    return ESP_OK;
}
#endif

static void on_wifi_state_changed(bool connected, void *user_ctx)
{
    (void)user_ctx;

    wifi_manager_status_t status = {0};
    wifi_manager_get_status(&status);
    const char *ap_ssid = status.ap_active ? status.ap_ssid : NULL;

    ESP_LOGI(TAG, "Wi-Fi state: sta_connected=%d ap_active=%d mode=%s ap_ssid=%s",
             connected,
             status.ap_active,
             status.mode ? status.mode : "off",
             ap_ssid ? ap_ssid : "(none)");

    esp_err_t err = app_claw_set_network_status(connected, ap_ssid);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to update network emote: %s", esp_err_to_name(err));
    }

#if CONFIG_APP_CLAW_CAP_AT_MQTT && CONFIG_APP_AT_UART1_NOTIFY_ENABLE
    app_notify_host_wifi_status(&status);
#endif
}

static esp_err_t app_claw_init_storage_paths(app_claw_storage_paths_t *paths)
{
    if (!paths || !app_fatfs_base_path || app_fatfs_base_path[0] != '/') {
        return ESP_ERR_INVALID_ARG;
    }

    memset(paths, 0, sizeof(*paths));

    if (strlcpy(paths->fatfs_base_path, app_fatfs_base_path, sizeof(paths->fatfs_base_path)) >= sizeof(paths->fatfs_base_path) ||
        snprintf(paths->memory_session_root, sizeof(paths->memory_session_root), "%s/sessions", app_fatfs_base_path) >= sizeof(paths->memory_session_root) ||
        snprintf(paths->memory_root_dir, sizeof(paths->memory_root_dir), "%s/memory", app_fatfs_base_path) >= sizeof(paths->memory_root_dir) ||
        snprintf(paths->skills_root_dir, sizeof(paths->skills_root_dir), "%s/skills", app_fatfs_base_path) >= sizeof(paths->skills_root_dir) ||
        snprintf(paths->lua_root_dir, sizeof(paths->lua_root_dir), "%s/scripts", app_fatfs_base_path) >= sizeof(paths->lua_root_dir) ||
        snprintf(paths->router_rules_path, sizeof(paths->router_rules_path), "%s/router_rules/router_rules.json", app_fatfs_base_path) >= sizeof(paths->router_rules_path) ||
        snprintf(paths->scheduler_rules_path, sizeof(paths->scheduler_rules_path), "%s/scheduler/schedules.json", app_fatfs_base_path) >= sizeof(paths->scheduler_rules_path) ||
        snprintf(paths->im_attachment_root, sizeof(paths->im_attachment_root), "%s/inbox", app_fatfs_base_path) >= sizeof(paths->im_attachment_root)) {
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

static esp_err_t main_load_config(app_config_t *config)
{
    return app_config_load(config);
}

static esp_err_t main_save_config(const app_config_t *config)
{
    return app_config_save(config);
}

static esp_err_t main_get_wifi_status(http_server_wifi_status_t *status)
{
    if (!status) {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_manager_status_t wifi_status = {0};
    wifi_manager_get_status(&wifi_status);
    status->wifi_connected = wifi_status.sta_connected;
    status->ip = wifi_status.sta_ip;
    status->ap_active = wifi_status.ap_active;
    status->ap_ssid = wifi_status.ap_ssid;
    status->ap_ip = wifi_status.ap_ip;
    status->wifi_mode = wifi_status.mode;
    return ESP_OK;
}

static void main_restart_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

static esp_err_t main_restart_device(void)
{
    BaseType_t ok = xTaskCreate(main_restart_task, "http_restart", 2048, NULL, 5, NULL);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

#if CONFIG_APP_CLAW_CAP_IM_WECHAT
static esp_err_t main_wechat_login_start(const char *account_id, bool force)
{
    return cap_im_wechat_qr_login_start(account_id, force);
}

static esp_err_t main_wechat_login_get_status(http_server_wechat_login_status_t *status)
{
    cap_im_wechat_qr_login_status_t *raw = NULL;
    esp_err_t err;

    if (!status) {
        return ESP_ERR_INVALID_ARG;
    }

    raw = calloc(1, sizeof(*raw));
    if (!raw) {
        return ESP_ERR_NO_MEM;
    }

    err = cap_im_wechat_qr_login_get_status(raw);
    if (err != ESP_OK) {
        free(raw);
        return err;
    }

    memset(status, 0, sizeof(*status));
    status->active = raw->active;
    status->configured = raw->configured;
    status->completed = raw->completed;
    status->persisted = raw->persisted;
    strlcpy(status->session_key, raw->session_key, sizeof(status->session_key));
    strlcpy(status->status, raw->status, sizeof(status->status));
    strlcpy(status->message, raw->message, sizeof(status->message));
    strlcpy(status->qr_data_url, raw->qr_data_url, sizeof(status->qr_data_url));
    strlcpy(status->account_id, raw->account_id, sizeof(status->account_id));
    strlcpy(status->user_id, raw->user_id, sizeof(status->user_id));
    strlcpy(status->token, raw->token, sizeof(status->token));
    strlcpy(status->base_url, raw->base_url, sizeof(status->base_url));
    free(raw);
    return ESP_OK;
}

static esp_err_t main_wechat_login_cancel(void)
{
    return cap_im_wechat_qr_login_cancel();
}

static esp_err_t main_wechat_login_mark_persisted(void)
{
    return cap_im_wechat_qr_login_mark_persisted();
}
#endif

static esp_err_t init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

static esp_err_t init_fatfs(void)
{
    esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = true,
        .max_files = 8,
        .allocation_unit_size = 4096,
        .disk_status_check_enable = false,
        .use_one_fat = false,
    };
    uint64_t total = 0;
    uint64_t free_bytes = 0;
    esp_err_t err;

    err = esp_vfs_fat_spiflash_mount_rw_wl(app_fatfs_base_path,
                                           APP_FATFS_PARTITION_LABEL,
                                           &mount_config,
                                           &s_wl_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount FATFS: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_vfs_fat_info(app_fatfs_base_path, &total, &free_bytes);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to query FATFS info: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "FATFS mounted total=%u used=%u",
                 (unsigned int)total,
                 (unsigned int)(total - free_bytes));
    }

    return ESP_OK;
}

static esp_err_t init_timezone(const char *timezone)
{
    esp_err_t err = ESP_OK;

    if (!timezone || timezone[0] == '\0') {
        ESP_LOGE(TAG, "Timezone is empty.");
        err = ESP_ERR_INVALID_ARG;
        goto tz_default;
    }

    if (setenv("TZ", timezone, 1) != 0) {
        ESP_LOGE(TAG, "Failed to set TZ env");
        err = ESP_FAIL;
        goto tz_default;
    }
    tzset();
    ESP_LOGI(TAG, "Timezone set to %s", timezone);
    return ESP_OK;

tz_default:
    assert(setenv("TZ", "CST-8", 1) == 0);
    tzset();
    ESP_LOGI(TAG, "Timezone set to default: CST-8");
    return err;
}

static bool app_dns_ip_is_empty(const esp_netif_dns_info_t *dns)
{
    return !dns || dns->ip.type != ESP_IPADDR_TYPE_V4 || dns->ip.u_addr.ip4.addr == 0;
}

static void app_log_dns_info(const char *name, const esp_netif_dns_info_t *dns)
{
    char ip[16] = "0.0.0.0";

    if (!app_dns_ip_is_empty(dns)) {
        esp_ip4addr_ntoa(&dns->ip.u_addr.ip4, ip, sizeof(ip));
    }
    ESP_LOGI(TAG, "DNS %s: %s", name, ip);
}

static void app_make_dns_fallback(esp_netif_dns_info_t *dns)
{
    memset(dns, 0, sizeof(*dns));
    dns->ip.type = ESP_IPADDR_TYPE_V4;
    esp_netif_set_ip4_addr(&dns->ip.u_addr.ip4, 223, 5, 5, 5);
}

static esp_err_t app_configure_sta_dns(void)
{
    esp_netif_t *sta_netif = wifi_manager_get_sta_netif();
    esp_netif_t *ap_netif = wifi_manager_get_ap_netif();
    esp_netif_dns_info_t main_dns = {0};
    esp_netif_dns_info_t backup_dns = {0};
    esp_netif_dns_info_t fallback_dns = {0};
    esp_netif_ip_info_t ap_ip = {0};
    bool main_dns_bad = false;
    esp_err_t err;

    if (!sta_netif) {
        return ESP_ERR_INVALID_STATE;
    }

    app_make_dns_fallback(&fallback_dns);

    err = esp_netif_set_dns_info(sta_netif, ESP_NETIF_DNS_FALLBACK, &fallback_dns);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to set fallback DNS: %s", esp_err_to_name(err));
    }

    err = esp_netif_get_dns_info(sta_netif, ESP_NETIF_DNS_MAIN, &main_dns);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to read main DNS: %s", esp_err_to_name(err));
        main_dns_bad = true;
    } else if (app_dns_ip_is_empty(&main_dns)) {
        main_dns_bad = true;
    }

    if (!main_dns_bad && ap_netif &&
            esp_netif_get_ip_info(ap_netif, &ap_ip) == ESP_OK &&
            ap_ip.ip.addr != 0 &&
            main_dns.ip.u_addr.ip4.addr == ap_ip.ip.addr) {
        main_dns_bad = true;
        ESP_LOGW(TAG, "Main DNS points to provisioning AP; replacing it for STA HTTP access");
    }

    if (main_dns_bad) {
        err = esp_netif_set_dns_info(sta_netif, ESP_NETIF_DNS_MAIN, &fallback_dns);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to set main DNS: %s", esp_err_to_name(err));
        }
    }

    esp_netif_get_dns_info(sta_netif, ESP_NETIF_DNS_MAIN, &main_dns);
    esp_netif_get_dns_info(sta_netif, ESP_NETIF_DNS_BACKUP, &backup_dns);
    esp_netif_get_dns_info(sta_netif, ESP_NETIF_DNS_FALLBACK, &fallback_dns);
    app_log_dns_info("main", &main_dns);
    app_log_dns_info("backup", &backup_dns);
    app_log_dns_info("fallback", &fallback_dns);

    return ESP_OK;
}

#if APP_ENABLE_MEM_LOG

static void print_task_stack_info(void)
{
#ifdef CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
    static TaskStatus_t s_task_status_snapshot[24];
    UBaseType_t count = uxTaskGetSystemState(s_task_status_snapshot,
                                             sizeof(s_task_status_snapshot) / sizeof(s_task_status_snapshot[0]),
                                             NULL);

    for (UBaseType_t i = 0; i < count; i++) {
        ESP_LOGI(TAG,
                 "Task %s  %u",
                 s_task_status_snapshot[i].pcTaskName,
                 s_task_status_snapshot[i].usStackHighWaterMark);
    }
#endif
}

/* Periodic task: print internal free, minimum free, and PSRAM free every 20s */
static void memory_monitor_task(void *arg)
{
    (void)arg;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        size_t internal_min = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
        size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        ESP_LOGI(TAG, "Memory: internal_free=%u bytes, internal_min_free=%u bytes, psram_free=%u bytes",
                 (unsigned)internal_free, (unsigned)internal_min, (unsigned)psram_free);
        print_task_stack_info();
    }
}

#endif

void app_main(void)
{
    esp_log_level_set("esp-x509-crt-bundle", ESP_LOG_WARN);

    ESP_LOGI(TAG, "Starting app");
    ESP_ERROR_CHECK(app_allocate_runtime_state());
    ESP_ERROR_CHECK(init_nvs());
    ESP_ERROR_CHECK(app_config_init());
    ESP_ERROR_CHECK(app_config_load(s_config));
    app_config_to_claw(s_config, s_claw_config);
    init_timezone(app_config_get_timezone(s_config)); // no need to check error
    ESP_ERROR_CHECK(esp_board_manager_init());
    ESP_ERROR_CHECK(app_claw_ui_start());
    ESP_ERROR_CHECK(init_fatfs());
    ESP_ERROR_CHECK(wifi_manager_init());
#if CONFIG_APP_CLAW_CAP_AT_MQTT && CONFIG_APP_AT_UART1_NOTIFY_ENABLE
    esp_err_t at_notify_err = app_init_at_uart1_notify();
    if (at_notify_err != ESP_OK) {
        ESP_LOGW(TAG, "UART1 provisioning notify disabled: %s", esp_err_to_name(at_notify_err));
    }
#endif
    ESP_ERROR_CHECK(http_server_init(&(http_server_config_t) {
        .storage_base_path = app_fatfs_base_path,
        .services = {
            .load_config = main_load_config,
            .save_config = main_save_config,
            .get_wifi_status = main_get_wifi_status,
            .restart_device = main_restart_device,
#if CONFIG_APP_CLAW_CAP_IM_WECHAT
            .wechat_login_start = main_wechat_login_start,
            .wechat_login_get_status = main_wechat_login_get_status,
            .wechat_login_cancel = main_wechat_login_cancel,
            .wechat_login_mark_persisted = main_wechat_login_mark_persisted,
#endif
        },
    }));
    ESP_ERROR_CHECK(wifi_manager_register_state_callback(on_wifi_state_changed, NULL));

    esp_err_t wifi_err = wifi_manager_start(&(wifi_manager_config_t) {
        .sta_ssid = s_config->wifi_ssid,
        .sta_password = s_config->wifi_password,
    });
    if (wifi_err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi start failed: %s", esp_err_to_name(wifi_err));
    } else {
        ESP_ERROR_CHECK(http_server_start());
        if (captive_dns_start(&(captive_dns_config_t) {
                .ap_netif = wifi_manager_get_ap_netif(),
                .configure_dhcp_dns = true,
            }) != ESP_OK) {
            ESP_LOGW(TAG, "Captive DNS could not start, portal pop-up disabled");
        }

        if (s_config->wifi_ssid[0] != '\0') {
            if (wifi_manager_wait_connected(30000) == ESP_OK) {
                wifi_manager_status_t status = {0};
                wifi_manager_get_status(&status);
                ESP_LOGI(TAG, "Wi-Fi STA ready: %s", status.sta_ip);
                app_configure_sta_dns();
            } else {
                ESP_LOGW(TAG, "STA could not connect, dropped to AP fallback");
            }
        }

        wifi_manager_status_t status = {0};
        wifi_manager_get_status(&status);
#if CONFIG_APP_CLAW_CAP_AT_MQTT && CONFIG_APP_AT_UART1_NOTIFY_ENABLE
        app_notify_host_wifi_status(&status);
#endif
        ESP_LOGW(TAG,
                 "*** Provisioning portal: SSID=\"%s\" (open) IP=%s URL=http://%s/ ***",
                 status.ap_ssid,
                 status.ap_ip,
                 status.ap_ip);
    }

    ESP_ERROR_CHECK(app_claw_init_storage_paths(s_claw_paths));
    ESP_ERROR_CHECK(app_claw_start(s_claw_config, s_claw_paths));
#if CONFIG_APP_CLAW_CAP_IM_LOCAL
    ESP_ERROR_CHECK(http_server_webim_bind_im());
#endif

    register_wifi_command();

#if APP_ENABLE_MEM_LOG
    /* Start memory monitor: print internal free, min free, PSRAM free every 20s */
    xTaskCreate(memory_monitor_task, "mem_mon", 4096, NULL, 1, NULL);
#endif

    app_free_runtime_state();
}
