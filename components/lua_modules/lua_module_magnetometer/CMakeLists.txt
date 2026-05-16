idf_component_register(
    SRCS
        "src/lua_module_magnetometer.c"
        "src/bmm350.c"
    INCLUDE_DIRS
        "include"
        "src"
    REQUIRES
        cap_lua
        esp_board_manager
        nvs_flash
    PRIV_REQUIRES
        espressif__i2c_bus
)

idf_component_set_property(${COMPONENT_NAME} WHOLE_ARCHIVE TRUE)
