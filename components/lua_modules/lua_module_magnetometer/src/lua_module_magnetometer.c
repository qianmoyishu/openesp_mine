/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "lua_module_magnetometer.h"

#include <float.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "bmm350.h"
#include "bmm350_defs.h"
#include "cap_lua.h"
#include "driver/gpio.h"
#include "esp_board_manager.h"
#include "esp_board_periph.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c_bus.h"
#include "lauxlib.h"

#if CONFIG_LUA_MODULE_MAGNETOMETER_CHIP_BMM150
#error "CONFIG_LUA_MODULE_MAGNETOMETER_CHIP_BMM150 is reserved but not implemented yet"
#elif CONFIG_LUA_MODULE_MAGNETOMETER_CHIP_MMC5603NJ
#error "CONFIG_LUA_MODULE_MAGNETOMETER_CHIP_MMC5603NJ is reserved but not implemented yet"
#elif !CONFIG_LUA_MODULE_MAGNETOMETER_CHIP_BMM350
#error "Unsupported magnetometer chip selection"
#endif

#define LUA_MODULE_MAGNETOMETER_METATABLE    "magnetometer.device"
#define LUA_MODULE_MAGNETOMETER_DEFAULT_NAME "magnetometer_sensor"
#define LUA_MODULE_MAGNETOMETER_LEGACY_NAME  "bmm350_sensor"
#define LUA_MODULE_MAGNETOMETER_MAX_NAME_LEN 64
#define LUA_MODULE_MAGNETOMETER_NVS_KEY_HARD_IRON  "hard_iron"
#define LUA_MODULE_MAGNETOMETER_NVS_KEY_SOFT_IRON  "soft_iron"
#define LUA_MODULE_MAGNETOMETER_NVS_KEY_CALIBRATED "calibrated"

typedef struct {
    float hard_iron[3];
    float soft_iron[3][3];
    float mag_min[3];
    float mag_max[3];
    uint32_t sample_count;
    bool calibrated;
    bool collecting;
} lua_module_magnetometer_calibration_t;

typedef struct {
    struct bmm350_dev sensor_handle;
    i2c_bus_handle_t i2c_bus_handle;
    i2c_bus_device_handle_t i2c_dev_handle;
    char peripheral_name[LUA_MODULE_MAGNETOMETER_MAX_NAME_LEN];
    bool peripheral_ref_held;
    gpio_num_t int_gpio_num;
    gpio_num_t sdo_gpio_num;
    uint8_t i2c_addr;
    bool sensor_initialized;
    lua_module_magnetometer_calibration_t calibration;
} lua_module_magnetometer_handle_t;

typedef struct {
    lua_module_magnetometer_handle_t *handle;
    char device_name[LUA_MODULE_MAGNETOMETER_MAX_NAME_LEN];
} lua_module_magnetometer_ud_t;

typedef struct {
    const char *name;
    const char *type;
    const char *chip;
    int8_t i2c_addr;
    int32_t frequency;
    int8_t int_gpio_num;
    int8_t sdo_gpio_num;
    uint8_t peripheral_count;
    const char *peripheral_name;
} lua_magnetometer_board_cfg_t;

typedef struct {
    char peripheral_name[LUA_MODULE_MAGNETOMETER_MAX_NAME_LEN];
    int i2c_addr;
    int frequency;
    int int_gpio_num;
    int sdo_gpio_num;
    bool has_peripheral;
    bool has_i2c_addr;
    bool has_frequency;
    bool has_int_gpio;
    bool has_sdo_gpio;
    bool try_alt_i2c_addr;
} lua_magnetometer_resolved_cfg_t;

static const char *TAG = "lua_module_magnetometer";

static void lua_module_magnetometer_destroy_handle(lua_module_magnetometer_handle_t *handle);

static void lua_module_magnetometer_calibration_reset_state(lua_module_magnetometer_handle_t *handle)
{
    for (size_t i = 0; i < 3; i++) {
        handle->calibration.hard_iron[i] = 0.0f;
        handle->calibration.mag_min[i] = FLT_MAX;
        handle->calibration.mag_max[i] = -FLT_MAX;
        for (size_t j = 0; j < 3; j++) {
            handle->calibration.soft_iron[i][j] = (i == j) ? 1.0f : 0.0f;
        }
    }
    handle->calibration.sample_count = 0;
    handle->calibration.calibrated = false;
    handle->calibration.collecting = false;
}

static uint32_t lua_module_magnetometer_hash_name(const char *name)
{
    uint32_t hash = 2166136261u;

    while (name != NULL && *name != '\0') {
        hash ^= (uint8_t)*name++;
        hash *= 16777619u;
    }

    return hash;
}

static esp_err_t lua_module_magnetometer_open_nvs(const char *device_name, nvs_handle_t *nvs_handle)
{
    char namespace_name[16];
    esp_err_t err;

    snprintf(namespace_name, sizeof(namespace_name), "mag%08" PRIx32,
             lua_module_magnetometer_hash_name(device_name));

    err = nvs_open(namespace_name, NVS_READWRITE, nvs_handle);
    if (err == ESP_OK) {
        return ESP_OK;
    }

    if (err == ESP_ERR_NVS_NOT_INITIALIZED) {
        err = nvs_flash_init();
        if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "Failed to erase NVS partition");
            err = nvs_flash_init();
        }
        if (err != ESP_OK) {
            return err;
        }
        return nvs_open(namespace_name, NVS_READWRITE, nvs_handle);
    }

    return err;
}

static esp_err_t lua_module_magnetometer_save_calibration(const char *device_name,
                                                          const lua_module_magnetometer_calibration_t *cal)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = lua_module_magnetometer_open_nvs(device_name, &nvs_handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_blob(nvs_handle, LUA_MODULE_MAGNETOMETER_NVS_KEY_HARD_IRON,
                       cal->hard_iron, sizeof(cal->hard_iron));
    if (err == ESP_OK) {
        err = nvs_set_blob(nvs_handle, LUA_MODULE_MAGNETOMETER_NVS_KEY_SOFT_IRON,
                           cal->soft_iron, sizeof(cal->soft_iron));
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(nvs_handle, LUA_MODULE_MAGNETOMETER_NVS_KEY_CALIBRATED,
                         cal->calibrated ? 1 : 0);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }

    nvs_close(nvs_handle);
    return err;
}

static esp_err_t lua_module_magnetometer_load_calibration(const char *device_name,
                                                          lua_module_magnetometer_calibration_t *cal)
{
    nvs_handle_t nvs_handle;
    size_t hard_iron_size = sizeof(cal->hard_iron);
    size_t soft_iron_size = sizeof(cal->soft_iron);
    uint8_t calibrated = 0;
    esp_err_t err = lua_module_magnetometer_open_nvs(device_name, &nvs_handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_get_blob(nvs_handle, LUA_MODULE_MAGNETOMETER_NVS_KEY_HARD_IRON,
                       cal->hard_iron, &hard_iron_size);
    if (err == ESP_OK) {
        err = nvs_get_blob(nvs_handle, LUA_MODULE_MAGNETOMETER_NVS_KEY_SOFT_IRON,
                           cal->soft_iron, &soft_iron_size);
    }
    if (err == ESP_OK) {
        err = nvs_get_u8(nvs_handle, LUA_MODULE_MAGNETOMETER_NVS_KEY_CALIBRATED, &calibrated);
    }

    nvs_close(nvs_handle);

    if (err != ESP_OK) {
        return err;
    }

    cal->calibrated = (calibrated != 0);
    return ESP_OK;
}

static esp_err_t lua_module_magnetometer_clear_calibration_storage(const char *device_name)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = lua_module_magnetometer_open_nvs(device_name, &nvs_handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_erase_key(nvs_handle, LUA_MODULE_MAGNETOMETER_NVS_KEY_HARD_IRON);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_erase_key(nvs_handle, LUA_MODULE_MAGNETOMETER_NVS_KEY_SOFT_IRON);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
    }
    if (err == ESP_OK) {
        err = nvs_erase_key(nvs_handle, LUA_MODULE_MAGNETOMETER_NVS_KEY_CALIBRATED);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }

    nvs_close(nvs_handle);
    return err;
}

static void lua_module_magnetometer_apply_calibration(const lua_module_magnetometer_calibration_t *cal,
                                                      const float raw[3],
                                                      float corrected[3])
{
    float hard_iron_corrected[3];

    if (!cal->calibrated) {
        corrected[0] = raw[0];
        corrected[1] = raw[1];
        corrected[2] = raw[2];
        return;
    }

    hard_iron_corrected[0] = raw[0] - cal->hard_iron[0];
    hard_iron_corrected[1] = raw[1] - cal->hard_iron[1];
    hard_iron_corrected[2] = raw[2] - cal->hard_iron[2];

    for (size_t row = 0; row < 3; row++) {
        corrected[row] = cal->soft_iron[row][0] * hard_iron_corrected[0] +
                         cal->soft_iron[row][1] * hard_iron_corrected[1] +
                         cal->soft_iron[row][2] * hard_iron_corrected[2];
    }
}

static void lua_module_magnetometer_calibration_record_sample(lua_module_magnetometer_handle_t *handle,
                                                              const float sample[3])
{
    for (size_t i = 0; i < 3; i++) {
        if (sample[i] < handle->calibration.mag_min[i]) {
            handle->calibration.mag_min[i] = sample[i];
        }
        if (sample[i] > handle->calibration.mag_max[i]) {
            handle->calibration.mag_max[i] = sample[i];
        }
    }

    handle->calibration.sample_count++;
    handle->calibration.collecting = true;
}

static esp_err_t lua_module_magnetometer_calibration_finish(lua_module_magnetometer_handle_t *handle)
{
    float avg_delta[3];
    float avg_radius;

    if (handle->calibration.sample_count < 16) {
        return ESP_ERR_INVALID_STATE;
    }

    for (size_t i = 0; i < 3; i++) {
        handle->calibration.hard_iron[i] =
            (handle->calibration.mag_max[i] + handle->calibration.mag_min[i]) * 0.5f;
        avg_delta[i] =
            (handle->calibration.mag_max[i] - handle->calibration.mag_min[i]) * 0.5f;
        if (avg_delta[i] <= 0.0f) {
            return ESP_ERR_INVALID_STATE;
        }
    }

    avg_radius = (avg_delta[0] + avg_delta[1] + avg_delta[2]) / 3.0f;
    if (avg_radius <= 0.0f) {
        return ESP_ERR_INVALID_STATE;
    }

    for (size_t i = 0; i < 3; i++) {
        for (size_t j = 0; j < 3; j++) {
            handle->calibration.soft_iron[i][j] = (i == j) ? (avg_radius / avg_delta[i]) : 0.0f;
        }
    }

    handle->calibration.calibrated = true;
    handle->calibration.collecting = false;
    return ESP_OK;
}

static esp_err_t lua_module_magnetometer_read_sample(lua_module_magnetometer_handle_t *handle,
                                                     struct bmm350_mag_temp_data *mag_data,
                                                     uint8_t *status)
{
    int8_t rslt = bmm350_get_compensated_mag_xyz_temp_data(mag_data, &handle->sensor_handle);
    if (rslt != BMM350_OK) {
        return ESP_FAIL;
    }

    rslt = bmm350_get_regs(BMM350_REG_INT_STATUS, status, 1, &handle->sensor_handle);
    return (rslt == BMM350_OK) ? ESP_OK : ESP_FAIL;
}

static void lua_module_magnetometer_push_calibration_table(lua_State *L,
                                                           const lua_module_magnetometer_calibration_t *cal)
{
    lua_newtable(L);

    lua_pushboolean(L, cal->calibrated);
    lua_setfield(L, -2, "calibrated");
    lua_pushboolean(L, cal->collecting);
    lua_setfield(L, -2, "collecting");
    lua_pushinteger(L, (lua_Integer)cal->sample_count);
    lua_setfield(L, -2, "sample_count");

    lua_newtable(L);
    for (size_t i = 0; i < 3; i++) {
        lua_pushnumber(L, cal->hard_iron[i]);
        lua_rawseti(L, -2, (int)i + 1);
    }
    lua_setfield(L, -2, "hard_iron");

    lua_newtable(L);
    for (size_t row = 0; row < 3; row++) {
        lua_newtable(L);
        for (size_t col = 0; col < 3; col++) {
            lua_pushnumber(L, cal->soft_iron[row][col]);
            lua_rawseti(L, -2, (int)col + 1);
        }
        lua_rawseti(L, -2, (int)row + 1);
    }
    lua_setfield(L, -2, "soft_iron");
}

static bool lua_module_magnetometer_read_vec3(lua_State *L, int idx, float out[3])
{
    bool ok = true;

    for (int i = 0; i < 3; i++) {
        lua_rawgeti(L, idx, i + 1);
        if (!lua_isnumber(L, -1)) {
            ok = false;
        } else {
            out[i] = (float)lua_tonumber(L, -1);
        }
        lua_pop(L, 1);
    }

    return ok;
}

static bool lua_module_magnetometer_read_mat3(lua_State *L, int idx, float out[3][3])
{
    bool ok = true;

    for (int row = 0; row < 3 && ok; row++) {
        lua_rawgeti(L, idx, row + 1);
        if (!lua_istable(L, -1)) {
            ok = false;
        } else {
            for (int col = 0; col < 3; col++) {
                lua_rawgeti(L, -1, col + 1);
                if (!lua_isnumber(L, -1)) {
                    ok = false;
                } else {
                    out[row][col] = (float)lua_tonumber(L, -1);
                }
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);
    }

    return ok;
}

static esp_err_t lua_module_magnetometer_configure_interrupt_pin(int int_gpio_num)
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

static esp_err_t lua_module_magnetometer_configure_sdo_pin(int sdo_gpio_num)
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

static esp_err_t lua_module_magnetometer_open_i2c_bus(const char *peripheral_name,
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

    if (!i2c_master_cfg->flags.enable_internal_pullup) {
        ESP_LOGW(TAG, "Board I2C '%s' has internal pull-ups disabled; enabling them for BMM350 stability",
                 peripheral_name);
        ESP_RETURN_ON_ERROR(gpio_pullup_en(i2c_master_cfg->sda_io_num), TAG,
                            "Failed to enable SDA pull-up on GPIO%d", i2c_master_cfg->sda_io_num);
        ESP_RETURN_ON_ERROR(gpio_pullup_en(i2c_master_cfg->scl_io_num), TAG,
                            "Failed to enable SCL pull-up on GPIO%d", i2c_master_cfg->scl_io_num);
    }

    const i2c_config_t i2c_bus_cfg = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = i2c_master_cfg->sda_io_num,
        .scl_io_num = i2c_master_cfg->scl_io_num,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
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

static esp_err_t lua_module_magnetometer_select_addr(lua_module_magnetometer_handle_t *handle,
                                                     uint8_t i2c_addr)
{
    if (handle->i2c_dev_handle != NULL && handle->i2c_addr == i2c_addr) {
        return ESP_OK;
    }

    if (handle->i2c_dev_handle != NULL) {
        i2c_bus_device_delete(&handle->i2c_dev_handle);
        handle->i2c_dev_handle = NULL;
    }

    handle->i2c_dev_handle = i2c_bus_device_create(handle->i2c_bus_handle, i2c_addr, 0);
    if (handle->i2c_dev_handle == NULL) {
        ESP_LOGE(TAG, "Failed to create BMM350 I2C device for address 0x%02x", i2c_addr);
        return ESP_FAIL;
    }

    handle->i2c_addr = i2c_addr;
    return ESP_OK;
}

static BMM350_INTF_RET_TYPE lua_module_magnetometer_i2c_read(uint8_t reg_addr,
                                                             uint8_t *reg_data,
                                                             uint32_t length,
                                                             void *intf_ptr)
{
    lua_module_magnetometer_handle_t *handle = (lua_module_magnetometer_handle_t *)intf_ptr;
    if (handle == NULL || handle->i2c_dev_handle == NULL) {
        return BMM350_E_COM_FAIL;
    }

    esp_err_t err = i2c_bus_read_bytes(handle->i2c_dev_handle, reg_addr, (uint16_t)length, reg_data);
    return (err == ESP_OK) ? BMM350_INTF_RET_SUCCESS : BMM350_E_COM_FAIL;
}

static BMM350_INTF_RET_TYPE lua_module_magnetometer_i2c_write(uint8_t reg_addr,
                                                              const uint8_t *reg_data,
                                                              uint32_t length,
                                                              void *intf_ptr)
{
    lua_module_magnetometer_handle_t *handle = (lua_module_magnetometer_handle_t *)intf_ptr;
    if (handle == NULL || handle->i2c_dev_handle == NULL) {
        return BMM350_E_COM_FAIL;
    }

    esp_err_t err = i2c_bus_write_bytes(handle->i2c_dev_handle, reg_addr, (uint16_t)length, reg_data);
    return (err == ESP_OK) ? BMM350_INTF_RET_SUCCESS : BMM350_E_COM_FAIL;
}

static void lua_module_magnetometer_delay_us(uint32_t period_us, void *intf_ptr)
{
    (void)intf_ptr;
    if (period_us < 1000) {
        esp_rom_delay_us(period_us);
    } else {
        vTaskDelay(pdMS_TO_TICKS((period_us + 999) / 1000));
    }
}

static esp_err_t lua_module_magnetometer_apply_default_runtime_config(lua_module_magnetometer_handle_t *handle)
{
    int8_t rslt;

    rslt = bmm350_set_odr_performance(BMM350_DATA_RATE_100HZ, BMM350_AVERAGING_4,
                                      &handle->sensor_handle);
    if (rslt != BMM350_OK) {
        ESP_LOGW(TAG, "bmm350_set_odr_performance returned %d (continuing)", rslt);
    }

    rslt = bmm350_enable_axes(BMM350_X_EN, BMM350_Y_EN, BMM350_Z_EN, &handle->sensor_handle);
    if (rslt != BMM350_OK) {
        ESP_LOGW(TAG, "bmm350_enable_axes returned %d (continuing)", rslt);
    }

    rslt = bmm350_set_powermode(BMM350_NORMAL_MODE, &handle->sensor_handle);
    if (rslt != BMM350_OK) {
        ESP_LOGW(TAG, "bmm350_set_powermode returned %d (continuing)", rslt);
    }

    uint8_t err_reg = 0;
    (void)bmm350_get_regs(BMM350_REG_ERR_REG, &err_reg, 1, &handle->sensor_handle);
    ESP_LOGI(TAG, "BMM350 configured: ERR_REG=0x%02X", err_reg);
    return ESP_OK;
}

static esp_err_t lua_module_magnetometer_probe_addr(lua_module_magnetometer_handle_t *handle,
                                                    uint8_t i2c_addr)
{
    int8_t rslt;

    ESP_RETURN_ON_ERROR(lua_module_magnetometer_select_addr(handle, i2c_addr), TAG,
                        "Failed to select BMM350 I2C address 0x%02x", i2c_addr);

    memset(&handle->sensor_handle, 0, sizeof(handle->sensor_handle));
    handle->sensor_handle.read = lua_module_magnetometer_i2c_read;
    handle->sensor_handle.write = lua_module_magnetometer_i2c_write;
    handle->sensor_handle.delay_us = lua_module_magnetometer_delay_us;
    handle->sensor_handle.intf_ptr = handle;

    rslt = bmm350_init(&handle->sensor_handle);
    ESP_LOGI(TAG, "BMM350 init at 0x%02x -> rslt=%d, chip_id=0x%02x",
             i2c_addr, rslt, handle->sensor_handle.chip_id);

    if (handle->sensor_handle.chip_id != BMM350_CHIP_ID) {
        return ESP_ERR_NOT_FOUND;
    }

    if (rslt != BMM350_OK) {
        ESP_LOGW(TAG, "BMM350 init failed at 0x%02x: %d", i2c_addr, rslt);
        return ESP_FAIL;
    }

    return lua_module_magnetometer_apply_default_runtime_config(handle);
}

static esp_err_t lua_module_magnetometer_create_handle(const lua_magnetometer_resolved_cfg_t *cfg,
                                                       lua_module_magnetometer_handle_t **out_handle)
{
    lua_module_magnetometer_handle_t *handle = calloc(1, sizeof(lua_module_magnetometer_handle_t));
    if (handle == NULL) {
        return ESP_ERR_NO_MEM;
    }

    snprintf(handle->peripheral_name, sizeof(handle->peripheral_name), "%s", cfg->peripheral_name);
    handle->int_gpio_num = (gpio_num_t)cfg->int_gpio_num;
    handle->sdo_gpio_num = (gpio_num_t)cfg->sdo_gpio_num;

    esp_err_t err = lua_module_magnetometer_configure_sdo_pin(cfg->sdo_gpio_num);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure SDO pin GPIO%d: %s", cfg->sdo_gpio_num, esp_err_to_name(err));
        free(handle);
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(10));

    err = lua_module_magnetometer_configure_interrupt_pin(cfg->int_gpio_num);
    if (err != ESP_OK) {
        free(handle);
        return err;
    }

    err = lua_module_magnetometer_open_i2c_bus(cfg->peripheral_name, cfg->frequency,
                                               &handle->i2c_bus_handle, &handle->peripheral_ref_held);
    if (err != ESP_OK) {
        free(handle);
        return err;
    }

    err = lua_module_magnetometer_probe_addr(handle, (uint8_t)cfg->i2c_addr);
    if (err != ESP_OK && cfg->try_alt_i2c_addr) {
        const uint8_t alt_addr = (cfg->i2c_addr == BMM350_I2C_ADSEL_SET_LOW) ?
                                 BMM350_I2C_ADSEL_SET_HIGH : BMM350_I2C_ADSEL_SET_LOW;
        ESP_LOGW(TAG, "Probe 0x%02x failed, retrying BMM350 at 0x%02x", cfg->i2c_addr, alt_addr);
        err = lua_module_magnetometer_probe_addr(handle, alt_addr);
    }
    if (err != ESP_OK) {
        lua_module_magnetometer_destroy_handle(handle);
        return err;
    }

    handle->sensor_initialized = true;
    *out_handle = handle;
    if (cfg->int_gpio_num >= 0) {
        ESP_LOGI(TAG, "BMM350 initialized on %s, INT GPIO%d, addr 0x%02x, freq %d Hz",
                 cfg->peripheral_name, cfg->int_gpio_num, handle->i2c_addr, cfg->frequency);
    } else {
        ESP_LOGI(TAG, "BMM350 initialized on %s, addr 0x%02x, freq %d Hz",
                 cfg->peripheral_name, handle->i2c_addr, cfg->frequency);
    }
    return ESP_OK;
}

static void lua_module_magnetometer_destroy_handle(lua_module_magnetometer_handle_t *handle)
{
    if (handle == NULL) {
        return;
    }

    if (handle->i2c_dev_handle != NULL) {
        i2c_bus_device_delete(&handle->i2c_dev_handle);
        handle->i2c_dev_handle = NULL;
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

static lua_module_magnetometer_ud_t *lua_module_magnetometer_get_ud(lua_State *L, int idx)
{
    lua_module_magnetometer_ud_t *ud =
        (lua_module_magnetometer_ud_t *)luaL_checkudata(L, idx, LUA_MODULE_MAGNETOMETER_METATABLE);
    if (!ud || !ud->handle || !ud->handle->sensor_initialized) {
        luaL_error(L, "magnetometer: invalid or closed handle");
    }
    return ud;
}

static void lua_module_magnetometer_push_axes_table(lua_State *L, float x, float y, float z)
{
    lua_newtable(L);
    lua_pushnumber(L, x);
    lua_setfield(L, -2, "x");
    lua_pushnumber(L, y);
    lua_setfield(L, -2, "y");
    lua_pushnumber(L, z);
    lua_setfield(L, -2, "z");
}

static int lua_module_magnetometer_close_impl(lua_State *L, lua_module_magnetometer_ud_t *ud)
{
    (void)L;
    if (ud->handle != NULL) {
        lua_module_magnetometer_destroy_handle(ud->handle);
        ud->handle = NULL;
    }
    ud->device_name[0] = '\0';
    return 0;
}

static int lua_module_magnetometer_gc(lua_State *L)
{
    lua_module_magnetometer_ud_t *ud =
        (lua_module_magnetometer_ud_t *)luaL_testudata(L, 1, LUA_MODULE_MAGNETOMETER_METATABLE);
    if (ud && ud->handle) {
        return lua_module_magnetometer_close_impl(L, ud);
    }
    return 0;
}

static int lua_module_magnetometer_close(lua_State *L)
{
    lua_module_magnetometer_ud_t *ud =
        (lua_module_magnetometer_ud_t *)luaL_checkudata(L, 1, LUA_MODULE_MAGNETOMETER_METATABLE);
    if (ud->handle) {
        return lua_module_magnetometer_close_impl(L, ud);
    }
    return 0;
}

static int lua_module_magnetometer_name(lua_State *L)
{
    lua_module_magnetometer_ud_t *ud = lua_module_magnetometer_get_ud(L, 1);
    lua_pushstring(L, ud->device_name);
    return 1;
}

static int lua_module_magnetometer_chip_id(lua_State *L)
{
    lua_module_magnetometer_ud_t *ud = lua_module_magnetometer_get_ud(L, 1);
    lua_pushinteger(L, ud->handle->sensor_handle.chip_id);
    return 1;
}

static esp_err_t lua_module_magnetometer_read_status(lua_module_magnetometer_handle_t *handle,
                                                     uint8_t *status)
{
    int8_t rslt = bmm350_get_regs(BMM350_REG_INT_STATUS, status, 1, &handle->sensor_handle);
    return (rslt == BMM350_OK) ? ESP_OK : ESP_FAIL;
}

static int lua_module_magnetometer_read(lua_State *L)
{
    lua_module_magnetometer_ud_t *ud = lua_module_magnetometer_get_ud(L, 1);
    struct bmm350_mag_temp_data mag_data = { 0 };
    uint8_t status = 0;
    float raw[3];
    float corrected[3];

    if (lua_module_magnetometer_read_sample(ud->handle, &mag_data, &status) != ESP_OK) {
        return luaL_error(L, "magnetometer read status failed");
    }

    raw[0] = mag_data.x;
    raw[1] = mag_data.y;
    raw[2] = mag_data.z;
    lua_module_magnetometer_apply_calibration(&ud->handle->calibration, raw, corrected);

    lua_newtable(L);
    lua_module_magnetometer_push_axes_table(L, corrected[0], corrected[1], corrected[2]);
    lua_setfield(L, -2, "magnetic");
    lua_module_magnetometer_push_axes_table(L, raw[0], raw[1], raw[2]);
    lua_setfield(L, -2, "raw_magnetic");
    lua_pushnumber(L, mag_data.temperature);
    lua_setfield(L, -2, "temperature");
    lua_pushinteger(L, status);
    lua_setfield(L, -2, "status");
    lua_pushboolean(L, ud->handle->calibration.calibrated);
    lua_setfield(L, -2, "calibrated");
    return 1;
}

static int lua_module_magnetometer_read_temperature(lua_State *L)
{
    lua_module_magnetometer_ud_t *ud = lua_module_magnetometer_get_ud(L, 1);
    struct bmm350_mag_temp_data mag_data = { 0 };
    uint8_t status = 0;

    if (lua_module_magnetometer_read_sample(ud->handle, &mag_data, &status) != ESP_OK) {
        return luaL_error(L, "magnetometer read_temperature failed");
    }

    lua_pushnumber(L, mag_data.temperature);
    return 1;
}

static int lua_module_magnetometer_read_int_status(lua_State *L)
{
    lua_module_magnetometer_ud_t *ud = lua_module_magnetometer_get_ud(L, 1);
    uint8_t status = 0;

    if (lua_module_magnetometer_read_status(ud->handle, &status) != ESP_OK) {
        return luaL_error(L, "magnetometer read_int_status failed");
    }

    lua_pushinteger(L, status);
    return 1;
}

static int lua_module_magnetometer_calibration_reset(lua_State *L)
{
    lua_module_magnetometer_ud_t *ud = lua_module_magnetometer_get_ud(L, 1);

    lua_module_magnetometer_calibration_reset_state(ud->handle);
    ud->handle->calibration.collecting = true;
    return 0;
}

static int lua_module_magnetometer_calibration_add_sample(lua_State *L)
{
    lua_module_magnetometer_ud_t *ud = lua_module_magnetometer_get_ud(L, 1);
    float sample[3];

    if (lua_istable(L, 2)) {
        if (!lua_module_magnetometer_read_vec3(L, 2, sample)) {
            return luaL_error(L, "magnetometer calibration_add_sample expects {x,y,z} array");
        }
    } else {
        struct bmm350_mag_temp_data mag_data = { 0 };
        uint8_t status = 0;
        if (lua_module_magnetometer_read_sample(ud->handle, &mag_data, &status) != ESP_OK) {
            return luaL_error(L, "magnetometer calibration_add_sample read failed");
        }
        sample[0] = mag_data.x;
        sample[1] = mag_data.y;
        sample[2] = mag_data.z;
    }

    lua_module_magnetometer_calibration_record_sample(ud->handle, sample);
    lua_pushinteger(L, (lua_Integer)ud->handle->calibration.sample_count);
    return 1;
}

static int lua_module_magnetometer_calibration_finish_lua(lua_State *L)
{
    lua_module_magnetometer_ud_t *ud = lua_module_magnetometer_get_ud(L, 1);
    esp_err_t err = lua_module_magnetometer_calibration_finish(ud->handle);
    if (err != ESP_OK) {
        return luaL_error(L, "magnetometer calibration_finish failed: %s",
                          esp_err_to_name(err));
    }

    err = lua_module_magnetometer_save_calibration(ud->device_name, &ud->handle->calibration);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to persist calibration for %s: %s",
                 ud->device_name, esp_err_to_name(err));
    }

    lua_module_magnetometer_push_calibration_table(L, &ud->handle->calibration);
    return 1;
}

static int lua_module_magnetometer_calibration_get(lua_State *L)
{
    lua_module_magnetometer_ud_t *ud = lua_module_magnetometer_get_ud(L, 1);
    lua_module_magnetometer_push_calibration_table(L, &ud->handle->calibration);
    return 1;
}

static int lua_module_magnetometer_calibration_set(lua_State *L)
{
    lua_module_magnetometer_ud_t *ud = lua_module_magnetometer_get_ud(L, 1);
    float hard_iron[3];
    float soft_iron[3][3];
    esp_err_t err;

    luaL_checktype(L, 2, LUA_TTABLE);

    lua_getfield(L, 2, "hard_iron");
    if (!lua_istable(L, -1) || !lua_module_magnetometer_read_vec3(L, lua_gettop(L), hard_iron)) {
        lua_pop(L, 1);
        return luaL_error(L, "magnetometer calibration_set: missing/invalid hard_iron");
    }
    lua_pop(L, 1);

    lua_getfield(L, 2, "soft_iron");
    if (!lua_istable(L, -1) || !lua_module_magnetometer_read_mat3(L, lua_gettop(L), soft_iron)) {
        lua_pop(L, 1);
        return luaL_error(L, "magnetometer calibration_set: missing/invalid soft_iron");
    }
    lua_pop(L, 1);

    memcpy(ud->handle->calibration.hard_iron, hard_iron, sizeof(hard_iron));
    memcpy(ud->handle->calibration.soft_iron, soft_iron, sizeof(soft_iron));
    ud->handle->calibration.calibrated = true;
    ud->handle->calibration.collecting = false;

    err = lua_module_magnetometer_save_calibration(ud->device_name, &ud->handle->calibration);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to persist calibration for %s: %s",
                 ud->device_name, esp_err_to_name(err));
    }

    lua_module_magnetometer_push_calibration_table(L, &ud->handle->calibration);
    return 1;
}

static int lua_module_magnetometer_calibration_clear(lua_State *L)
{
    lua_module_magnetometer_ud_t *ud = lua_module_magnetometer_get_ud(L, 1);
    esp_err_t err;

    lua_module_magnetometer_calibration_reset_state(ud->handle);
    err = lua_module_magnetometer_clear_calibration_storage(ud->device_name);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to clear persisted calibration for %s: %s",
                 ud->device_name, esp_err_to_name(err));
    }

    lua_module_magnetometer_push_calibration_table(L, &ud->handle->calibration);
    return 1;
}

static esp_err_t lua_module_magnetometer_load_board_defaults(const char *device_name,
                                                             lua_magnetometer_resolved_cfg_t *out)
{
    void *raw = NULL;
    esp_err_t err = esp_board_manager_get_device_config(device_name, &raw);
    if (err != ESP_OK || raw == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    const lua_magnetometer_board_cfg_t *board = (const lua_magnetometer_board_cfg_t *)raw;

    if (board->chip != NULL && strcmp(board->chip, LUA_MODULE_MAGNETOMETER_SELECTED_CHIP_NAME) != 0) {
        ESP_LOGW(TAG, "Board device '%s' chip='%s' does not match %s backend",
                 device_name, board->chip, LUA_MODULE_MAGNETOMETER_SELECTED_CHIP_NAME);
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

static void lua_module_magnetometer_apply_lua_overrides(lua_State *L, int opts_idx,
                                                        lua_magnetometer_resolved_cfg_t *cfg)
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
        cfg->try_alt_i2c_addr = false;
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

static int lua_module_magnetometer_new(lua_State *L)
{
    const char *device_name = LUA_MODULE_MAGNETOMETER_DEFAULT_NAME;
    int opts_idx = 0;

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

    if (strlen(device_name) >= LUA_MODULE_MAGNETOMETER_MAX_NAME_LEN) {
        return luaL_error(L, "magnetometer device name too long");
    }

    lua_magnetometer_resolved_cfg_t cfg = { 0 };
    cfg.int_gpio_num = -1;
    cfg.sdo_gpio_num = -1;

    esp_err_t err = lua_module_magnetometer_load_board_defaults(device_name, &cfg);
    const char *opened_device_name = device_name;
    if (err != ESP_OK && strcmp(device_name, LUA_MODULE_MAGNETOMETER_DEFAULT_NAME) == 0) {
        if (lua_module_magnetometer_load_board_defaults(LUA_MODULE_MAGNETOMETER_LEGACY_NAME, &cfg) == ESP_OK) {
            opened_device_name = LUA_MODULE_MAGNETOMETER_LEGACY_NAME;
            ESP_LOGW(TAG, "Default device '%s' not declared, using legacy '%s'",
                     LUA_MODULE_MAGNETOMETER_DEFAULT_NAME, LUA_MODULE_MAGNETOMETER_LEGACY_NAME);
        }
    }

    lua_module_magnetometer_apply_lua_overrides(L, opts_idx, &cfg);

    if (!cfg.has_peripheral) {
        return luaL_error(L, "magnetometer.new: missing 'peripheral' (board declares no '%s', "
                              "and no override given)", device_name);
    }
    if (!cfg.has_i2c_addr) {
        cfg.i2c_addr = BMM350_I2C_ADSEL_SET_LOW;
        cfg.has_i2c_addr = true;
        cfg.try_alt_i2c_addr = true;
    }
    if (!cfg.has_frequency) {
        cfg.frequency = 100000;
        cfg.has_frequency = true;
    }
    if (!cfg.has_int_gpio) {
        cfg.int_gpio_num = -1;
    }
    if (!cfg.has_sdo_gpio) {
        cfg.sdo_gpio_num = -1;
    }

    if (cfg.i2c_addr != BMM350_I2C_ADSEL_SET_LOW && cfg.i2c_addr != BMM350_I2C_ADSEL_SET_HIGH) {
        return luaL_error(L, "magnetometer.new: unsupported BMM350 I2C address 0x%02x", cfg.i2c_addr);
    }

    lua_module_magnetometer_handle_t *handle = NULL;
    err = lua_module_magnetometer_create_handle(&cfg, &handle);
    if (err != ESP_OK || handle == NULL) {
        return luaL_error(L, "magnetometer.new failed: %s",
                          esp_err_to_name(err != ESP_OK ? err : ESP_FAIL));
    }

    lua_module_magnetometer_ud_t *ud =
        (lua_module_magnetometer_ud_t *)lua_newuserdata(L, sizeof(*ud));
    memset(ud, 0, sizeof(*ud));
    ud->handle = handle;
    snprintf(ud->device_name, sizeof(ud->device_name), "%s", opened_device_name);
    lua_module_magnetometer_calibration_reset_state(handle);
    err = lua_module_magnetometer_load_calibration(ud->device_name, &handle->calibration);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Failed to load calibration for %s: %s",
                 ud->device_name, esp_err_to_name(err));
        lua_module_magnetometer_calibration_reset_state(handle);
    }

    luaL_getmetatable(L, LUA_MODULE_MAGNETOMETER_METATABLE);
    lua_setmetatable(L, -2);
    return 1;
}

int luaopen_magnetometer(lua_State *L)
{
    if (luaL_newmetatable(L, LUA_MODULE_MAGNETOMETER_METATABLE)) {
        lua_pushcfunction(L, lua_module_magnetometer_gc);
        lua_setfield(L, -2, "__gc");
        lua_pushvalue(L, -1);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, lua_module_magnetometer_read);
        lua_setfield(L, -2, "read");
        lua_pushcfunction(L, lua_module_magnetometer_read_temperature);
        lua_setfield(L, -2, "read_temperature");
        lua_pushcfunction(L, lua_module_magnetometer_read_int_status);
        lua_setfield(L, -2, "read_int_status");
        lua_pushcfunction(L, lua_module_magnetometer_chip_id);
        lua_setfield(L, -2, "chip_id");
        lua_pushcfunction(L, lua_module_magnetometer_calibration_reset);
        lua_setfield(L, -2, "calibration_reset");
        lua_pushcfunction(L, lua_module_magnetometer_calibration_add_sample);
        lua_setfield(L, -2, "calibration_add_sample");
        lua_pushcfunction(L, lua_module_magnetometer_calibration_finish_lua);
        lua_setfield(L, -2, "calibration_finish");
        lua_pushcfunction(L, lua_module_magnetometer_calibration_get);
        lua_setfield(L, -2, "calibration_get");
        lua_pushcfunction(L, lua_module_magnetometer_calibration_set);
        lua_setfield(L, -2, "calibration_set");
        lua_pushcfunction(L, lua_module_magnetometer_calibration_clear);
        lua_setfield(L, -2, "calibration_clear");
        lua_pushcfunction(L, lua_module_magnetometer_name);
        lua_setfield(L, -2, "name");
        lua_pushcfunction(L, lua_module_magnetometer_close);
        lua_setfield(L, -2, "close");
    }
    lua_pop(L, 1);

    lua_newtable(L);
    lua_pushcfunction(L, lua_module_magnetometer_new);
    lua_setfield(L, -2, "new");
    return 1;
}

esp_err_t lua_module_magnetometer_register(void)
{
    return cap_lua_register_module("magnetometer", luaopen_magnetometer);
}
