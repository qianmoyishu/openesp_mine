/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lua_module_system.h"

#include <sys/time.h>
#include <time.h>

#include "cap_lua.h"
#include "esp_netif.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "lauxlib.h"

static int lua_module_system_time(lua_State *L)
{
    time_t now;

    now = time(NULL);
    if (now < 0) {
        return luaL_error(L, "system clock not set");
    }

    lua_pushnumber(L, (lua_Number)now);
    return 1;
}

static int lua_module_system_date(lua_State *L)
{
    const char *fmt = luaL_optstring(L, 1, "%Y-%m-%d %H:%M:%S");
    char buf[128];
    struct tm local_time;
    time_t now;
    size_t len;

    now = time(NULL);
    localtime_r(&now, &local_time);

    len = strftime(buf, sizeof(buf), fmt, &local_time);
    if (len == 0) {
        return luaL_error(L, "system.date: format too long or produced empty string");
    }

    lua_pushstring(L, buf);
    return 1;
}

static int lua_module_system_millis(lua_State *L)
{
    int64_t us = esp_timer_get_time();

    lua_pushnumber(L, (lua_Number)(us / 1000));
    return 1;
}

static int lua_module_system_uptime(lua_State *L)
{
    int64_t us = esp_timer_get_time();

    lua_pushinteger(L, (lua_Integer)(us / 1000000));
    return 1;
}

static bool lua_module_system_get_sta_ip(char *buf, size_t buf_size)
{
    esp_netif_t *netif = NULL;
    esp_netif_ip_info_t ip_info = {0};

    if (!buf || buf_size == 0) {
        return false;
    }

    netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif || esp_netif_get_ip_info(netif, &ip_info) != ESP_OK || ip_info.ip.addr == 0) {
        return false;
    }

    snprintf(buf, buf_size, IPSTR, IP2STR(&ip_info.ip));
    return true;
}

static int lua_module_system_ip(lua_State *L)
{
    char ip_buf[16];

    if (!lua_module_system_get_sta_ip(ip_buf, sizeof(ip_buf))) {
        lua_pushnil(L);
        return 1;
    }

    lua_pushstring(L, ip_buf);
    return 1;
}

static int lua_module_system_info(lua_State *L)
{
    char date_buf[32];
    struct tm local_time;
    time_t now;
    size_t psram_total;
    wifi_ap_record_t ap_info = {0};
    int64_t us;

    lua_newtable(L);

    us = esp_timer_get_time();
    lua_pushinteger(L, (lua_Integer)(us / 1000000));
    lua_setfield(L, -2, "uptime_s");

    now = time(NULL);
    lua_pushnumber(L, (lua_Number)now);
    lua_setfield(L, -2, "time");

    localtime_r(&now, &local_time);
    if (strftime(date_buf, sizeof(date_buf), "%Y-%m-%d %H:%M:%S", &local_time) > 0) {
        lua_pushstring(L, date_buf);
        lua_setfield(L, -2, "date");
    }

    lua_pushinteger(L, (lua_Integer)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    lua_setfield(L, -2, "sram_free");

    lua_pushinteger(L, (lua_Integer)heap_caps_get_total_size(MALLOC_CAP_INTERNAL));
    lua_setfield(L, -2, "sram_total");

    lua_pushinteger(L, (lua_Integer)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    lua_setfield(L, -2, "sram_largest");

    psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    if (psram_total > 0) {
        lua_pushinteger(L, (lua_Integer)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        lua_setfield(L, -2, "psram_free");

        lua_pushinteger(L, (lua_Integer)psram_total);
        lua_setfield(L, -2, "psram_total");
    }

    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        lua_pushinteger(L, (lua_Integer)ap_info.rssi);
        lua_setfield(L, -2, "wifi_rssi");

        if (ap_info.ssid[0] != '\0') {
            lua_pushstring(L, (const char *)ap_info.ssid);
            lua_setfield(L, -2, "wifi_ssid");
        }
    }

    return 1;
}

int luaopen_system(lua_State *L)
{
    lua_newtable(L);

    lua_pushcfunction(L, lua_module_system_time);
    lua_setfield(L, -2, "time");

    lua_pushcfunction(L, lua_module_system_date);
    lua_setfield(L, -2, "date");

    lua_pushcfunction(L, lua_module_system_millis);
    lua_setfield(L, -2, "millis");

    lua_pushcfunction(L, lua_module_system_uptime);
    lua_setfield(L, -2, "uptime");

    lua_pushcfunction(L, lua_module_system_ip);
    lua_setfield(L, -2, "ip");

    lua_pushcfunction(L, lua_module_system_info);
    lua_setfield(L, -2, "info");

    return 1;
}

esp_err_t lua_module_system_register(void)
{
    return cap_lua_register_module("system", luaopen_system);
}
