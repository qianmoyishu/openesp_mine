idf_component_register(
    SRCS
        "src/lua_module_imu.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        cap_lua
        esp_board_manager
    PRIV_REQUIRES
        espressif__i2c_bus
)

if(CONFIG_LUA_MODULE_IMU_CHIP_BMI270)
    idf_component_optional_requires(PRIVATE espressif__bmi270_sensor)
endif()

if(CONFIG_LUA_MODULE_IMU_CHIP_ICM42670)
    idf_component_optional_requires(PRIVATE espressif__icm42670)
endif()

idf_component_set_property(${COMPONENT_NAME} WHOLE_ARCHIVE TRUE)
