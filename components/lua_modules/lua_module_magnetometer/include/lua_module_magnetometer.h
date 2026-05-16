/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "sdkconfig.h"
#include "esp_err.h"
#include "lua.h"

#ifdef __cplusplus
extern "C" {
#endif

#if CONFIG_LUA_MODULE_MAGNETOMETER_CHIP_BMM350
#define LUA_MODULE_MAGNETOMETER_SELECTED_CHIP_NAME "bmm350"
#elif CONFIG_LUA_MODULE_MAGNETOMETER_CHIP_BMM150
#define LUA_MODULE_MAGNETOMETER_SELECTED_CHIP_NAME "bmm150"
#elif CONFIG_LUA_MODULE_MAGNETOMETER_CHIP_MMC5603NJ
#define LUA_MODULE_MAGNETOMETER_SELECTED_CHIP_NAME "mmc5603nj"
#else
#error "No chip model selected for lua_module_magnetometer"
#endif

int luaopen_magnetometer(lua_State *L);
esp_err_t lua_module_magnetometer_register(void);

#ifdef __cplusplus
}
#endif
