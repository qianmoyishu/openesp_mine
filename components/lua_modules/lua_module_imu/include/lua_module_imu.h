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

#if CONFIG_LUA_MODULE_IMU_CHIP_BMI270
#define LUA_MODULE_IMU_SELECTED_CHIP_NAME "bmi270"
#elif CONFIG_LUA_MODULE_IMU_CHIP_ICM42670
#define LUA_MODULE_IMU_SELECTED_CHIP_NAME "icm42670"
#elif CONFIG_LUA_MODULE_IMU_CHIP_MPU6050
#define LUA_MODULE_IMU_SELECTED_CHIP_NAME "mpu6050"
#else
#error "No IMU chip model selected for lua_module_imu"
#endif

int luaopen_imu(lua_State *L);
esp_err_t lua_module_imu_register(void);

#ifdef __cplusplus
}
#endif
