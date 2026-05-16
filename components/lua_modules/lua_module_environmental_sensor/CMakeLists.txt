idf_component_register(
    SRCS
        "src/lua_module_environmental_sensor.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        cap_lua
        esp_board_manager
    PRIV_REQUIRES
        espressif__bme690
        espressif__i2c_bus
)

idf_component_set_property(${COMPONENT_NAME} WHOLE_ARCHIVE TRUE)
