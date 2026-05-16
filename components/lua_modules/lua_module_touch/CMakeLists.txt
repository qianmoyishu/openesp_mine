idf_component_register(
    SRCS
        "src/lua_module_touch.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        cap_lua
        esp_board_manager
        esp_driver_touch_sens
)

idf_component_set_property(${COMPONENT_NAME} WHOLE_ARCHIVE TRUE)
