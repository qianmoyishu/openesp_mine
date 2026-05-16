/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "lua_module_imu.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "cap_lua.h"
#include "driver/gpio.h"
#include "esp_board_manager.h"
#include "esp_board_periph.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "i2c_bus.h"
#include "lauxlib.h"

#if CONFIG_LUA_MODULE_IMU_CHIP_BMI270
#include "bmi270_api.h"
#elif CONFIG_LUA_MODULE_IMU_CHIP_ICM42670
#include "icm42670.h"
#elif CONFIG_LUA_MODULE_IMU_CHIP_MPU6050
#error "CONFIG_LUA_MODULE_IMU_CHIP_MPU6050 is reserved but not implemented yet"
#else
#error "Unsupported IMU chip selection"
#endif

#define LUA_MODULE_IMU_METATABLE          "imu.device"
#define LUA_MODULE_IMU_DEFAULT_NAME       "imu_sensor"
#if CONFIG_LUA_MODULE_IMU_CHIP_BMI270
#define LUA_MODULE_IMU_LEGACY_NAME        "bmi270_sensor"
#elif CONFIG_LUA_MODULE_IMU_CHIP_ICM42670
#define LUA_MODULE_IMU_LEGACY_NAME        "icm42670_sensor"
#endif
#define LUA_MODULE_IMU_MAX_NAME_LEN       64

typedef struct {
#if CONFIG_LUA_MODULE_IMU_CHIP_BMI270
    bmi270_handle_t sensor_handle;
#elif CONFIG_LUA_MODULE_IMU_CHIP_ICM42670
    icm42670_handle_t sensor_handle;
#endif
    i2c_bus_handle_t i2c_bus_handle;
    char peripheral_name[LUA_MODULE_IMU_MAX_NAME_LEN];
    bool peripheral_ref_held;
    gpio_num_t int_gpio_num;
    gpio_num_t sdo_gpio_num;
} lua_module_imu_handle_t;

typedef struct {
    lua_module_imu_handle_t *handle;
    char device_name[LUA_MODULE_IMU_MAX_NAME_LEN];
} lua_module_imu_ud_t;

/*
 * Local mirror of the dev_custom_imu_sensor_config_t struct that the ESP
 * Board Manager auto-generates from the board's board_devices.yaml.
 *
 * We mirror it (instead of #including gen_board_device_custom.h) so this
 * component compiles uniformly on every board, including boards that do
 * not declare the imu_sensor custom device. The board manager exposes the
 * parsed struct via esp_board_manager_get_device_config(), and we cast
 * the void* to this mirror type to read defaults.
 *
 * KEEP THE FIELDS IN SYNC with the generator output for the imu_sensor
 * YAML schema (single peripheral, fields below).
 */
typedef struct {
    const char *name;
    const char *type;
    const char *chip;
    int8_t      i2c_addr;
    int32_t     frequency;
    int8_t      int_gpio_num;
    int8_t      sdo_gpio_num;
    uint8_t     peripheral_count;
    const char *peripheral_name;
} lua_imu_board_cfg_t;

typedef struct {
    char        peripheral_name[LUA_MODULE_IMU_MAX_NAME_LEN];
    int         i2c_addr;
    int         frequency;
    int         int_gpio_num;
    int         sdo_gpio_num;
    bool        has_peripheral;
    bool        has_i2c_addr;
    bool        has_frequency;
    bool        has_int_gpio;
    bool        has_sdo_gpio;
} lua_imu_resolved_cfg_t;

static const char *TAG = "lua_module_imu";

static void lua_module_imu_destroy_handle(lua_module_imu_handle_t *handle);

static esp_err_t lua_module_imu_configure_interrupt_pin(int int_gpio_num)
{
    if (int_gpio_num < 0) {
        return ESP_OK;
    }

    const gpio_config_t int_pin_cfg = {
        .pin_bit_mask = 1ULL << int_gpio_num,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    return gpio_config(&int_pin_cfg);
}

static esp_err_t lua_module_imu_configure_sdo_pin(int sdo_gpio_num)
{
    if (sdo_gpio_num < 0) {
        return ESP_OK;
    }

    const gpio_config_t sdo_pin_cfg = {
        .pin_bit_mask = 1ULL << sdo_gpio_num,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t err = gpio_config(&sdo_pin_cfg);
    if (err != ESP_OK) {
        return err;
    }

    return gpio_set_level((gpio_num_t)sdo_gpio_num, 0);
}

static esp_err_t lua_module_imu_open_i2c_bus(const char *peripheral_name,
                                             int frequency,
                                             i2c_bus_handle_t *i2c_bus_handle,
                                             bool *peripheral_ref_held)
{
    i2c_master_bus_handle_t i2c_master_handle = NULL;
    i2c_master_bus_config_t *i2c_master_cfg = NULL;

    *peripheral_ref_held = false;

    ESP_RETURN_ON_ERROR(esp_board_periph_ref_handle(peripheral_name, (void **)&i2c_master_handle),
                        TAG, "Failed to reference board I2C bus '%s'", peripheral_name);
    *peripheral_ref_held = true;

    esp_err_t err = esp_board_periph_get_config(peripheral_name, (void **)&i2c_master_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get board I2C config '%s': %s", peripheral_name, esp_err_to_name(err));
        esp_board_periph_unref_handle(peripheral_name);
        *peripheral_ref_held = false;
        return err;
    }

    const i2c_config_t i2c_bus_cfg = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = i2c_master_cfg->sda_io_num,
        .scl_io_num = i2c_master_cfg->scl_io_num,
        .sda_pullup_en = i2c_master_cfg->flags.enable_internal_pullup,
        .scl_pullup_en = i2c_master_cfg->flags.enable_internal_pullup,
        .master.clk_speed = (uint32_t)frequency,
        .clk_flags = 0,
    };

    (void)i2c_master_handle;
    *i2c_bus_handle = i2c_bus_create(i2c_master_cfg->i2c_port, &i2c_bus_cfg);
    if (*i2c_bus_handle == NULL) {
        esp_board_periph_unref_handle(peripheral_name);
        *peripheral_ref_held = false;
        return ESP_FAIL;
    }

    return ESP_OK;
}

#if CONFIG_LUA_MODULE_IMU_CHIP_BMI270

static esp_err_t lua_module_imu_apply_default_runtime_config(bmi270_handle_t sensor_handle)
{
    int8_t rslt;
    uint8_t sens_list[2] = { BMI2_ACCEL, BMI2_GYRO };
    struct bmi2_sens_config config[2] = { 0 };

    config[BMI2_ACCEL].type = BMI2_ACCEL;
    config[BMI2_GYRO].type = BMI2_GYRO;

    rslt = bmi2_set_adv_power_save(BMI2_DISABLE, sensor_handle);
    if (rslt != BMI2_OK) {
        ESP_LOGE(TAG, "Failed to disable BMI270 APS mode: %d", rslt);
        return ESP_FAIL;
    }

    rslt = bmi2_get_sensor_config(config, 2, sensor_handle);
    if (rslt != BMI2_OK) {
        ESP_LOGE(TAG, "Failed to get BMI270 sensor config: %d", rslt);
        return ESP_FAIL;
    }

    config[BMI2_ACCEL].cfg.acc.odr = BMI2_ACC_ODR_200HZ;
    config[BMI2_ACCEL].cfg.acc.range = BMI2_ACC_RANGE_16G;
    config[BMI2_ACCEL].cfg.acc.bwp = BMI2_ACC_NORMAL_AVG4;
    config[BMI2_ACCEL].cfg.acc.filter_perf = BMI2_PERF_OPT_MODE;

    config[BMI2_GYRO].cfg.gyr.odr = BMI2_GYR_ODR_200HZ;
    config[BMI2_GYRO].cfg.gyr.range = BMI2_GYR_RANGE_2000;
    config[BMI2_GYRO].cfg.gyr.bwp = BMI2_GYR_NORMAL_MODE;
    config[BMI2_GYRO].cfg.gyr.noise_perf = BMI2_PERF_OPT_MODE;
    config[BMI2_GYRO].cfg.gyr.filter_perf = BMI2_PERF_OPT_MODE;

    rslt = bmi2_set_sensor_config(config, 2, sensor_handle);
    if (rslt != BMI2_OK) {
        ESP_LOGE(TAG, "Failed to set BMI270 sensor config: %d", rslt);
        return ESP_FAIL;
    }

    rslt = bmi270_sensor_enable(sens_list, 2, sensor_handle);
    if (rslt != BMI2_OK) {
        ESP_LOGE(TAG, "Failed to enable BMI270 accel/gyro: %d", rslt);
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t lua_module_imu_create_handle(const lua_imu_resolved_cfg_t *cfg,
                                              lua_module_imu_handle_t **out_handle)
{
    lua_module_imu_handle_t *handle = calloc(1, sizeof(lua_module_imu_handle_t));
    if (handle == NULL) {
        return ESP_ERR_NO_MEM;
    }

    snprintf(handle->peripheral_name, sizeof(handle->peripheral_name), "%s", cfg->peripheral_name);
    handle->int_gpio_num = (gpio_num_t)cfg->int_gpio_num;
    handle->sdo_gpio_num = (gpio_num_t)cfg->sdo_gpio_num;

    esp_err_t err = lua_module_imu_configure_sdo_pin(cfg->sdo_gpio_num);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure SDO pin GPIO%d: %s", cfg->sdo_gpio_num, esp_err_to_name(err));
        free(handle);
        return err;
    }

    err = lua_module_imu_configure_interrupt_pin(cfg->int_gpio_num);
    if (err != ESP_OK) {
        free(handle);
        return err;
    }

    err = lua_module_imu_open_i2c_bus(cfg->peripheral_name, cfg->frequency,
                                      &handle->i2c_bus_handle, &handle->peripheral_ref_held);
    if (err != ESP_OK) {
        free(handle);
        return err;
    }

    err = bmi270_sensor_create(handle->i2c_bus_handle, &handle->sensor_handle,
                               bmi270_config_file, BMI2_GYRO_CROSS_SENS_ENABLE | BMI2_CRT_RTOSK_ENABLE);
    if (err != ESP_OK || handle->sensor_handle == NULL) {
        ESP_LOGE(TAG, "Failed to create BMI270 sensor: %s", esp_err_to_name(err));
        if (handle->peripheral_ref_held) {
            esp_board_periph_unref_handle(handle->peripheral_name);
        }
        free(handle);
        return err != ESP_OK ? err : ESP_FAIL;
    }

    struct bmi2_int_pin_config int_pin_cfg = { 0 };
    int_pin_cfg.pin_type = BMI2_INT1;
    int_pin_cfg.pin_cfg[0].input_en = BMI2_INT_INPUT_DISABLE;
    int_pin_cfg.pin_cfg[0].lvl = BMI2_INT_ACTIVE_HIGH;
    int_pin_cfg.pin_cfg[0].od = BMI2_INT_PUSH_PULL;
    int_pin_cfg.pin_cfg[0].output_en = BMI2_INT_OUTPUT_ENABLE;
    int_pin_cfg.int_latch = BMI2_INT_NON_LATCH;

    int8_t rslt = bmi2_set_int_pin_config(&int_pin_cfg, handle->sensor_handle);
    if (rslt != BMI2_OK) {
        ESP_LOGE(TAG, "Failed to configure BMI270 INT1 pin: %d", rslt);
        bmi270_sensor_del(&handle->sensor_handle);
        if (handle->peripheral_ref_held) {
            esp_board_periph_unref_handle(handle->peripheral_name);
        }
        free(handle);
        return ESP_FAIL;
    }

    err = lua_module_imu_apply_default_runtime_config(handle->sensor_handle);
    if (err != ESP_OK) {
        bmi270_sensor_del(&handle->sensor_handle);
        if (handle->peripheral_ref_held) {
            esp_board_periph_unref_handle(handle->peripheral_name);
        }
        free(handle);
        return err;
    }

    *out_handle = handle;
    ESP_LOGI(TAG, "BMI270 IMU initialized on %s, INT GPIO%d, addr 0x%02x, freq %d Hz",
             cfg->peripheral_name, cfg->int_gpio_num, cfg->i2c_addr, cfg->frequency);
    return ESP_OK;
}

#elif CONFIG_LUA_MODULE_IMU_CHIP_ICM42670

static esp_err_t lua_module_imu_apply_default_runtime_config(icm42670_handle_t sensor_handle)
{
    const icm42670_cfg_t imu_cfg = {
        .acce_fs = ACCE_FS_16G,
        .acce_odr = ACCE_ODR_200HZ,
        .gyro_fs = GYRO_FS_2000DPS,
        .gyro_odr = GYRO_ODR_200HZ,
    };

    ESP_RETURN_ON_ERROR(icm42670_config(sensor_handle, &imu_cfg), TAG,
                        "Failed to configure ICM42670 accel/gyro");
    ESP_RETURN_ON_ERROR(icm42670_acce_set_pwr(sensor_handle, ACCE_PWR_LOWNOISE), TAG,
                        "Failed to enable ICM42670 accelerometer");
    ESP_RETURN_ON_ERROR(icm42670_gyro_set_pwr(sensor_handle, GYRO_PWR_LOWNOISE), TAG,
                        "Failed to enable ICM42670 gyroscope");

    return ESP_OK;
}

static esp_err_t lua_module_imu_create_handle(const lua_imu_resolved_cfg_t *cfg,
                                              lua_module_imu_handle_t **out_handle)
{
    lua_module_imu_handle_t *handle = calloc(1, sizeof(lua_module_imu_handle_t));
    if (handle == NULL) {
        return ESP_ERR_NO_MEM;
    }

    snprintf(handle->peripheral_name, sizeof(handle->peripheral_name), "%s", cfg->peripheral_name);
    handle->int_gpio_num = (gpio_num_t)cfg->int_gpio_num;
    handle->sdo_gpio_num = (gpio_num_t)cfg->sdo_gpio_num;

    esp_err_t err = lua_module_imu_configure_sdo_pin(cfg->sdo_gpio_num);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure SDO pin GPIO%d: %s", cfg->sdo_gpio_num, esp_err_to_name(err));
        free(handle);
        return err;
    }

    err = lua_module_imu_configure_interrupt_pin(cfg->int_gpio_num);
    if (err != ESP_OK) {
        free(handle);
        return err;
    }

    err = lua_module_imu_open_i2c_bus(cfg->peripheral_name, cfg->frequency,
                                      &handle->i2c_bus_handle, &handle->peripheral_ref_held);
    if (err != ESP_OK) {
        free(handle);
        return err;
    }

    i2c_master_bus_handle_t i2c_master_handle =
        i2c_bus_get_internal_bus_handle(handle->i2c_bus_handle);
    if (i2c_master_handle == NULL) {
        ESP_LOGE(TAG, "Failed to resolve internal I2C handle for '%s'", cfg->peripheral_name);
        if (handle->peripheral_ref_held) {
            esp_board_periph_unref_handle(handle->peripheral_name);
        }
        free(handle);
        return ESP_FAIL;
    }

    err = icm42670_create(i2c_master_handle, (uint8_t)cfg->i2c_addr, &handle->sensor_handle);
    if (err != ESP_OK || handle->sensor_handle == NULL) {
        ESP_LOGE(TAG, "Failed to create ICM42670 sensor: %s", esp_err_to_name(err));
        if (handle->peripheral_ref_held) {
            esp_board_periph_unref_handle(handle->peripheral_name);
        }
        free(handle);
        return err != ESP_OK ? err : ESP_FAIL;
    }

    err = lua_module_imu_apply_default_runtime_config(handle->sensor_handle);
    if (err != ESP_OK) {
        icm42670_delete(handle->sensor_handle);
        if (handle->peripheral_ref_held) {
            esp_board_periph_unref_handle(handle->peripheral_name);
        }
        free(handle);
        return err;
    }

    *out_handle = handle;
    if (cfg->int_gpio_num >= 0) {
        ESP_LOGI(TAG, "ICM42670 IMU initialized on %s, INT GPIO%d, addr 0x%02x, freq %d Hz",
                 cfg->peripheral_name, cfg->int_gpio_num, cfg->i2c_addr, cfg->frequency);
    } else {
        ESP_LOGI(TAG, "ICM42670 IMU initialized on %s, addr 0x%02x, freq %d Hz",
                 cfg->peripheral_name, cfg->i2c_addr, cfg->frequency);
    }
    return ESP_OK;
}

#endif // CONFIG_LUA_MODULE_IMU_CHIP_BMI270 || CONFIG_LUA_MODULE_IMU_CHIP_ICM42670

static void lua_module_imu_destroy_handle(lua_module_imu_handle_t *handle)
{
    if (handle == NULL) {
        return;
    }
    if (handle->sensor_handle != NULL) {
#if CONFIG_LUA_MODULE_IMU_CHIP_BMI270
        bmi270_sensor_del(&handle->sensor_handle);
#elif CONFIG_LUA_MODULE_IMU_CHIP_ICM42670
        icm42670_delete(handle->sensor_handle);
        handle->sensor_handle = NULL;
#endif
    }
    if (handle->int_gpio_num >= 0) {
        gpio_reset_pin(handle->int_gpio_num);
    }
    if (handle->sdo_gpio_num >= 0) {
        gpio_reset_pin(handle->sdo_gpio_num);
    }
    if (handle->peripheral_ref_held && handle->peripheral_name[0] != '\0') {
        esp_board_periph_unref_handle(handle->peripheral_name);
    }
    free(handle);
}

static lua_module_imu_ud_t *lua_module_imu_get_ud(lua_State *L, int idx)
{
    lua_module_imu_ud_t *ud =
        (lua_module_imu_ud_t *)luaL_checkudata(L, idx, LUA_MODULE_IMU_METATABLE);
    if (!ud || !ud->handle || !ud->handle->sensor_handle) {
        luaL_error(L, "imu: invalid or closed handle");
    }
    return ud;
}

static void lua_module_imu_push_axes_table(lua_State *L, int x, int y, int z)
{
    lua_newtable(L);
    lua_pushinteger(L, x);
    lua_setfield(L, -2, "x");
    lua_pushinteger(L, y);
    lua_setfield(L, -2, "y");
    lua_pushinteger(L, z);
    lua_setfield(L, -2, "z");
}

static int lua_module_imu_close_impl(lua_State *L, lua_module_imu_ud_t *ud)
{
    (void)L;
    if (ud->handle != NULL) {
        lua_module_imu_destroy_handle(ud->handle);
        ud->handle = NULL;
    }
    ud->device_name[0] = '\0';
    return 0;
}

static int lua_module_imu_gc(lua_State *L)
{
    lua_module_imu_ud_t *ud =
        (lua_module_imu_ud_t *)luaL_testudata(L, 1, LUA_MODULE_IMU_METATABLE);
    if (ud && ud->handle) {
        return lua_module_imu_close_impl(L, ud);
    }
    return 0;
}

static int lua_module_imu_close(lua_State *L)
{
    lua_module_imu_ud_t *ud =
        (lua_module_imu_ud_t *)luaL_checkudata(L, 1, LUA_MODULE_IMU_METATABLE);
    if (ud->handle) {
        return lua_module_imu_close_impl(L, ud);
    }
    return 0;
}

static int lua_module_imu_name(lua_State *L)
{
    lua_module_imu_ud_t *ud = lua_module_imu_get_ud(L, 1);
    lua_pushstring(L, ud->device_name);
    return 1;
}

static int lua_module_imu_read(lua_State *L)
{
    lua_module_imu_ud_t *ud = lua_module_imu_get_ud(L, 1);
#if CONFIG_LUA_MODULE_IMU_CHIP_BMI270
    struct bmi2_sens_data data = { 0 };
    int8_t rslt = bmi2_get_sensor_data(&data, ud->handle->sensor_handle);

    if (rslt != BMI2_OK) {
        return luaL_error(L, "imu read failed: %d", rslt);
    }

    lua_newtable(L);
    lua_module_imu_push_axes_table(L, data.acc.x, data.acc.y, data.acc.z);
    lua_setfield(L, -2, "accel");
    lua_module_imu_push_axes_table(L, data.gyr.x, data.gyr.y, data.gyr.z);
    lua_setfield(L, -2, "gyro");
    lua_pushinteger(L, data.sens_time);
    lua_setfield(L, -2, "sens_time");
    lua_pushinteger(L, data.status);
    lua_setfield(L, -2, "status");
#elif CONFIG_LUA_MODULE_IMU_CHIP_ICM42670
    icm42670_raw_value_t acc = { 0 };
    icm42670_raw_value_t gyro = { 0 };
    uint8_t int_status = 0;
    esp_err_t err = icm42670_get_acce_raw_value(ud->handle->sensor_handle, &acc);
    if (err != ESP_OK) {
        return luaL_error(L, "imu read accel failed: %s", esp_err_to_name(err));
    }
    err = icm42670_get_gyro_raw_value(ud->handle->sensor_handle, &gyro);
    if (err != ESP_OK) {
        return luaL_error(L, "imu read gyro failed: %s", esp_err_to_name(err));
    }
    err = icm42670_read_register(ud->handle->sensor_handle, ICM42670_INT_STATUS, &int_status);
    if (err != ESP_OK) {
        return luaL_error(L, "imu read status failed: %s", esp_err_to_name(err));
    }

    lua_newtable(L);
    lua_module_imu_push_axes_table(L, acc.x, acc.y, acc.z);
    lua_setfield(L, -2, "accel");
    lua_module_imu_push_axes_table(L, gyro.x, gyro.y, gyro.z);
    lua_setfield(L, -2, "gyro");
    lua_pushinteger(L, (lua_Integer)esp_timer_get_time());
    lua_setfield(L, -2, "sens_time");
    lua_pushinteger(L, int_status);
    lua_setfield(L, -2, "status");
#endif
    return 1;
}

static int lua_module_imu_read_temperature(lua_State *L)
{
    lua_module_imu_ud_t *ud = lua_module_imu_get_ud(L, 1);
#if CONFIG_LUA_MODULE_IMU_CHIP_BMI270
    int16_t temp = 0;
    int8_t rslt = bmi2_get_temperature_data(&temp, ud->handle->sensor_handle);

    if (rslt != BMI2_OK) {
        return luaL_error(L, "imu read_temperature failed: %d", rslt);
    }

    lua_pushinteger(L, temp);
#elif CONFIG_LUA_MODULE_IMU_CHIP_ICM42670
    uint16_t temp = 0;
    esp_err_t err = icm42670_get_temp_raw_value(ud->handle->sensor_handle, &temp);
    if (err != ESP_OK) {
        return luaL_error(L, "imu read_temperature failed: %s", esp_err_to_name(err));
    }

    lua_pushinteger(L, temp);
#endif
    return 1;
}

static int lua_module_imu_read_int_status(lua_State *L)
{
    lua_module_imu_ud_t *ud = lua_module_imu_get_ud(L, 1);
#if CONFIG_LUA_MODULE_IMU_CHIP_BMI270
    uint16_t int_status = 0;
    int8_t rslt = bmi2_get_int_status(&int_status, ud->handle->sensor_handle);

    if (rslt != BMI2_OK) {
        return luaL_error(L, "imu read_int_status failed: %d", rslt);
    }

    lua_pushinteger(L, int_status);
#elif CONFIG_LUA_MODULE_IMU_CHIP_ICM42670
    uint8_t int_status0 = 0;
    uint8_t int_status2 = 0;
    uint8_t int_status3 = 0;
    esp_err_t err = icm42670_read_register(ud->handle->sensor_handle, ICM42670_INT_STATUS, &int_status0);
    if (err == ESP_OK) {
        err = icm42670_read_register(ud->handle->sensor_handle, ICM42670_INT_STATUS2, &int_status2);
    }
    if (err == ESP_OK) {
        err = icm42670_read_register(ud->handle->sensor_handle, ICM42670_INT_STATUS3, &int_status3);
    }
    if (err != ESP_OK) {
        return luaL_error(L, "imu read_int_status failed: %s", esp_err_to_name(err));
    }

    lua_pushinteger(L, ((lua_Integer)int_status3 << 16) |
                         ((lua_Integer)int_status2 << 8) |
                         int_status0);
#endif
    return 1;
}

/*
 * Pull defaults for the named device from the board manager (parsed from
 * board_devices.yaml). Returns ESP_OK if the device exists; ESP_ERR_NOT_FOUND
 * if the active board does not declare this device. In both cases `out`
 * is left in a defined state and the caller can still override fields
 * from Lua opts.
 */
static esp_err_t lua_module_imu_load_board_defaults(const char *device_name,
                                                    lua_imu_resolved_cfg_t *out)
{
    void *raw = NULL;
    esp_err_t err = esp_board_manager_get_device_config(device_name, &raw);
    if (err != ESP_OK || raw == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    const lua_imu_board_cfg_t *board = (const lua_imu_board_cfg_t *)raw;

    if (board->chip != NULL && strcmp(board->chip, LUA_MODULE_IMU_SELECTED_CHIP_NAME) != 0) {
        ESP_LOGW(TAG, "Board device '%s' chip='%s' does not match %s backend",
                 device_name, board->chip, LUA_MODULE_IMU_SELECTED_CHIP_NAME);
    }

    if (board->peripheral_name != NULL && board->peripheral_name[0] != '\0') {
        snprintf(out->peripheral_name, sizeof(out->peripheral_name), "%s", board->peripheral_name);
        out->has_peripheral = true;
    }
    if (board->i2c_addr != 0) {
        out->i2c_addr = board->i2c_addr;
        out->has_i2c_addr = true;
    }
    if (board->frequency > 0) {
        out->frequency = board->frequency;
        out->has_frequency = true;
    }
    out->int_gpio_num = board->int_gpio_num;
    out->has_int_gpio = true;
    out->sdo_gpio_num = board->sdo_gpio_num;
    out->has_sdo_gpio = true;

    return ESP_OK;
}

static void lua_module_imu_apply_lua_overrides(lua_State *L, int opts_idx,
                                               lua_imu_resolved_cfg_t *cfg)
{
    if (opts_idx == 0 || lua_type(L, opts_idx) != LUA_TTABLE) {
        return;
    }

    lua_getfield(L, opts_idx, "peripheral");
    if (lua_isstring(L, -1)) {
        const char *p = lua_tostring(L, -1);
        snprintf(cfg->peripheral_name, sizeof(cfg->peripheral_name), "%s", p);
        cfg->has_peripheral = true;
    }
    lua_pop(L, 1);

    lua_getfield(L, opts_idx, "i2c_addr");
    if (lua_isnumber(L, -1)) {
        cfg->i2c_addr = (int)lua_tointeger(L, -1);
        cfg->has_i2c_addr = true;
    }
    lua_pop(L, 1);

    lua_getfield(L, opts_idx, "frequency");
    if (lua_isnumber(L, -1)) {
        cfg->frequency = (int)lua_tointeger(L, -1);
        cfg->has_frequency = true;
    }
    lua_pop(L, 1);

    lua_getfield(L, opts_idx, "int_gpio");
    if (lua_isnumber(L, -1)) {
        cfg->int_gpio_num = (int)lua_tointeger(L, -1);
        cfg->has_int_gpio = true;
    }
    lua_pop(L, 1);

    lua_getfield(L, opts_idx, "sdo_gpio");
    if (lua_isnumber(L, -1)) {
        cfg->sdo_gpio_num = (int)lua_tointeger(L, -1);
        cfg->has_sdo_gpio = true;
    }
    lua_pop(L, 1);
}

static int lua_module_imu_new(lua_State *L)
{
    const char *device_name = LUA_MODULE_IMU_DEFAULT_NAME;
    int opts_idx = 0;

    /*
     * Accepted call shapes:
     *   imu.new()
     *   imu.new("imu_sensor")              -- device name only
     *   imu.new({ ... })                   -- opts only, default device name
     *   imu.new("imu_sensor", { ... })     -- device name + opts
     */
    if (lua_isstring(L, 1)) {
        device_name = lua_tostring(L, 1);
        if (lua_istable(L, 2)) {
            opts_idx = 2;
        }
    } else if (lua_istable(L, 1)) {
        opts_idx = 1;
        lua_getfield(L, 1, "device");
        if (lua_isstring(L, -1)) {
            device_name = lua_tostring(L, -1);
        }
        lua_pop(L, 1);
    }

    if (strlen(device_name) >= LUA_MODULE_IMU_MAX_NAME_LEN) {
        return luaL_error(L, "imu device name too long");
    }

    lua_imu_resolved_cfg_t cfg = { 0 };
    cfg.int_gpio_num = -1;
    cfg.sdo_gpio_num = -1;

    /* Defaults from board manager (if device declared on this board). Falls
     * back to the legacy "bmi270_sensor" name when the user did not request
     * a specific device, mirroring the previous behaviour. */
    esp_err_t err = lua_module_imu_load_board_defaults(device_name, &cfg);
    const char *opened_device_name = device_name;
    if (err != ESP_OK && strcmp(device_name, LUA_MODULE_IMU_DEFAULT_NAME) == 0) {
        if (lua_module_imu_load_board_defaults(LUA_MODULE_IMU_LEGACY_NAME, &cfg) == ESP_OK) {
            opened_device_name = LUA_MODULE_IMU_LEGACY_NAME;
            ESP_LOGW(TAG, "Default device '%s' not declared, using legacy '%s'",
                     LUA_MODULE_IMU_DEFAULT_NAME, LUA_MODULE_IMU_LEGACY_NAME);
        }
    }

    lua_module_imu_apply_lua_overrides(L, opts_idx, &cfg);

    if (!cfg.has_peripheral) {
        return luaL_error(L, "imu.new: missing 'peripheral' (board declares no '%s', "
                              "and no override given)", device_name);
    }
    if (!cfg.has_i2c_addr) {
#if CONFIG_LUA_MODULE_IMU_CHIP_BMI270
        cfg.i2c_addr = BMI270_I2C_ADDRESS;
#elif CONFIG_LUA_MODULE_IMU_CHIP_ICM42670
        cfg.i2c_addr = ICM42670_I2C_ADDRESS;
#endif
        cfg.has_i2c_addr = true;
    }
    if (!cfg.has_frequency) {
        cfg.frequency = 400000;
        cfg.has_frequency = true;
    }
#if CONFIG_LUA_MODULE_IMU_CHIP_BMI270
    if (!cfg.has_int_gpio) {
        cfg.int_gpio_num = -1;
    }
    if (cfg.i2c_addr != BMI270_I2C_ADDRESS) {
        return luaL_error(L, "imu.new: unsupported BMI270 I2C address 0x%02x", cfg.i2c_addr);
    }
#elif CONFIG_LUA_MODULE_IMU_CHIP_ICM42670
    if (!cfg.has_int_gpio) {
        cfg.int_gpio_num = -1;
    }
    if (cfg.i2c_addr != ICM42670_I2C_ADDRESS && cfg.i2c_addr != ICM42670_I2C_ADDRESS_1) {
        return luaL_error(L, "imu.new: unsupported ICM42670 I2C address 0x%02x", cfg.i2c_addr);
    }
#endif

    lua_module_imu_handle_t *handle = NULL;
    err = lua_module_imu_create_handle(&cfg, &handle);
    if (err != ESP_OK || handle == NULL) {
        return luaL_error(L, "imu.new failed: %s",
                          esp_err_to_name(err != ESP_OK ? err : ESP_FAIL));
    }

    lua_module_imu_ud_t *ud =
        (lua_module_imu_ud_t *)lua_newuserdata(L, sizeof(*ud));
    memset(ud, 0, sizeof(*ud));
    ud->handle = handle;
    snprintf(ud->device_name, sizeof(ud->device_name), "%s", opened_device_name);

    luaL_getmetatable(L, LUA_MODULE_IMU_METATABLE);
    lua_setmetatable(L, -2);
    return 1;
}

int luaopen_imu(lua_State *L)
{
    if (luaL_newmetatable(L, LUA_MODULE_IMU_METATABLE)) {
        lua_pushcfunction(L, lua_module_imu_gc);
        lua_setfield(L, -2, "__gc");
        lua_pushvalue(L, -1);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, lua_module_imu_read);
        lua_setfield(L, -2, "read");
        lua_pushcfunction(L, lua_module_imu_read_temperature);
        lua_setfield(L, -2, "read_temperature");
        lua_pushcfunction(L, lua_module_imu_read_int_status);
        lua_setfield(L, -2, "read_int_status");
        lua_pushcfunction(L, lua_module_imu_name);
        lua_setfield(L, -2, "name");
        lua_pushcfunction(L, lua_module_imu_close);
        lua_setfield(L, -2, "close");
    }
    lua_pop(L, 1);

    lua_newtable(L);
    lua_pushcfunction(L, lua_module_imu_new);
    lua_setfield(L, -2, "new");
    return 1;
}

esp_err_t lua_module_imu_register(void)
{
    return cap_lua_register_module("imu", luaopen_imu);
}
