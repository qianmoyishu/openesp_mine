/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Lua "display" module.
 *
 * Ported from esp-claw lua_module_halo_display.c.
 * HAL calls are routed through display_hal.h which the board layer must
 * implement.  PNG decode is done here using libpng; JPEG decode is delegated
 * to the HAL.
 */
#include "lua_module_display.h"

#include "cap_lua.h"
#include "display_arbiter.h"
#include "display_hal.h"
#include "esp_err.h"
#include "esp_log.h"
#include "lauxlib.h"
#include "png.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "lua_display";

/* -------------------------------------------------------------------------
 * Argument helpers (mirrors the reference implementation)
 * ---------------------------------------------------------------------- */

static int lua_display_check_integer_arg(lua_State *L, int index, const char *name)
{
    if (!lua_isinteger(L, index)) {
        return luaL_error(L, "display %s must be an integer", name);
    }
    return (int)lua_tointeger(L, index);
}

static float lua_display_check_number_arg(lua_State *L, int index, const char *name)
{
    if (!lua_isnumber(L, index)) {
        luaL_error(L, "display %s must be a number", name);
        return 0.0f;
    }
    return (float)lua_tonumber(L, index);
}

static bool lua_display_is_ascii_text(const char *text)
{
    if (!text) {
        return false;
    }
    while (*text) {
        unsigned char ch = (unsigned char)*text++;
        if ((ch < 32 || ch > 126) && ch != '\n' && ch != '\r' && ch != '\t') {
            return false;
        }
    }
    return true;
}

/* Pack three consecutive integer arguments into an RGB565 colour. */
static uint16_t lua_display_color(lua_State *L, int start_index)
{
    int red   = lua_display_check_integer_arg(L, start_index,     "color component");
    int green = lua_display_check_integer_arg(L, start_index + 1, "color component");
    int blue  = lua_display_check_integer_arg(L, start_index + 2, "color component");
    return (uint16_t)(((red & 0xF8) << 8) | ((green & 0xFC) << 3) | ((blue & 0xF8) >> 3));
}

static uint16_t lua_display_rgb888_to_rgb565(uint8_t red, uint8_t green, uint8_t blue)
{
    return (uint16_t)(((red & 0xF8) << 8) | ((green & 0xFC) << 3) | ((blue & 0xF8) >> 3));
}

static void *lua_display_check_lightuserdata_arg(lua_State *L, int index, const char *name)
{
    void *ptr = lua_touserdata(L, index);

    luaL_argcheck(L, ptr != NULL, index, name);
    return ptr;
}

static const uint8_t *lua_display_check_buffer_arg(lua_State *L, int index, size_t expected,
                                                   size_t *out_len)
{
    if (lua_islightuserdata(L, index)) {
        const void *ptr = lua_touserdata(L, index);
        luaL_argcheck(L, ptr != NULL, index, "display buffer lightuserdata expected");
        if (out_len != NULL) {
            *out_len = expected;
        }
        return (const uint8_t *)ptr;
    }

    size_t data_len = 0;
    const uint8_t *data = (const uint8_t *)luaL_checklstring(L, index, &data_len);
    if (out_len != NULL) {
        *out_len = data_len;
    }
    return data;
}

static void *lua_display_opt_lightuserdata_arg(lua_State *L, int index)
{
    if (lua_isnoneornil(L, index)) {
        return NULL;
    }

    return lua_display_check_lightuserdata_arg(L, index,
                                               "display io_handle lightuserdata expected");
}

static display_hal_panel_if_t lua_display_parse_panel_if(lua_State *L, int index)
{
    if (lua_isnoneornil(L, index)) {
        return DISPLAY_HAL_PANEL_IF_IO;
    }

    if (!lua_isinteger(L, index)) {
        luaL_error(L, "display panel_if must be an interface constant");
        return DISPLAY_HAL_PANEL_IF_IO;
    }

    lua_Integer value = lua_tointeger(L, index);

    if (value >= DISPLAY_HAL_PANEL_IF_IO && value <= DISPLAY_HAL_PANEL_IF_MIPI_DSI) {
        return (display_hal_panel_if_t)value;
    }

    luaL_error(L, "display panel_if integer is out of range");
    return DISPLAY_HAL_PANEL_IF_IO;
}

/* -------------------------------------------------------------------------
 * Screen lifecycle
 * ---------------------------------------------------------------------- */

static void lua_display_runtime_cleanup(void)
{
    if (!display_arbiter_is_owner(DISPLAY_ARBITER_OWNER_LUA)) {
        return;
    }
    ESP_LOGI(TAG, "Lua runtime cleanup: display still owned by Lua, releasing");

    if (display_hal_destroy() == ESP_OK) {
        display_arbiter_release(DISPLAY_ARBITER_OWNER_LUA);
    }
}

static int lua_display_init(lua_State *L)
{
    esp_lcd_panel_handle_t panel_handle =
        (esp_lcd_panel_handle_t)lua_display_check_lightuserdata_arg(
            L, 1, "display panel_handle lightuserdata expected");
    esp_lcd_panel_io_handle_t io_handle =
        (esp_lcd_panel_io_handle_t)lua_display_opt_lightuserdata_arg(L, 2);
    int lcd_width = lua_display_check_integer_arg(L, 3, "lcd_width");
    int lcd_height = lua_display_check_integer_arg(L, 4, "lcd_height");
    display_hal_panel_if_t panel_if = lua_display_parse_panel_if(L, 5);

    esp_err_t err = display_arbiter_acquire(DISPLAY_ARBITER_OWNER_LUA);
    if (err != ESP_OK) {
        return luaL_error(L, "display init acquire failed: %s", esp_err_to_name(err));
    }

    err = display_hal_create(panel_handle, io_handle, panel_if, lcd_width, lcd_height);
    if (err != ESP_OK) {
        display_arbiter_release(DISPLAY_ARBITER_OWNER_LUA);
        return luaL_error(L, "display init failed: %s", esp_err_to_name(err));
    }

    lua_pushboolean(L, 1);
    return 1;
}

static int lua_display_deinit(lua_State *L)
{
    (void)L;
    esp_err_t err = display_hal_destroy();
    if (err != ESP_OK) {
        return luaL_error(L, "display deinit failed: %s", esp_err_to_name(err));
    }

    err = display_arbiter_release(DISPLAY_ARBITER_OWNER_LUA);
    if (err != ESP_OK) {
        return luaL_error(L, "display release failed: %s", esp_err_to_name(err));
    }

    lua_pushboolean(L, 1);
    return 1;
}

/* -------------------------------------------------------------------------
 * Text option helpers
 * ---------------------------------------------------------------------- */

static display_hal_text_align_t lua_display_parse_align(lua_State *L, int index,
                                                        display_hal_text_align_t default_align)
{
    const char *value = lua_tostring(L, index);
    if (!value || strcmp(value, "left") == 0) {
        return DISPLAY_HAL_TEXT_ALIGN_LEFT;
    }
    if (strcmp(value, "center") == 0 || strcmp(value, "centre") == 0) {
        return DISPLAY_HAL_TEXT_ALIGN_CENTER;
    }
    if (strcmp(value, "right") == 0) {
        return DISPLAY_HAL_TEXT_ALIGN_RIGHT;
    }
    luaL_error(L, "display align must be left, center, or right");
    return default_align;
}

static display_hal_text_valign_t lua_display_parse_valign(lua_State *L, int index,
                                                          display_hal_text_valign_t default_valign)
{
    const char *value = lua_tostring(L, index);
    if (!value || strcmp(value, "top") == 0) {
        return DISPLAY_HAL_TEXT_VALIGN_TOP;
    }
    if (strcmp(value, "middle") == 0 || strcmp(value, "center") == 0) {
        return DISPLAY_HAL_TEXT_VALIGN_MIDDLE;
    }
    if (strcmp(value, "bottom") == 0) {
        return DISPLAY_HAL_TEXT_VALIGN_BOTTOM;
    }
    luaL_error(L, "display valign must be top, middle, or bottom");
    return default_valign;
}

/* Parse an optional table at `index` for text-rendering options.
 * NULL output pointers are silently skipped. */
static void lua_display_parse_text_options(lua_State *L, int index,
                                           uint8_t *text_r, uint8_t *text_g, uint8_t *text_b,
                                           uint8_t *font_size,
                                           uint8_t *bg_r, uint8_t *bg_g, uint8_t *bg_b,
                                           bool *has_bg,
                                           display_hal_text_align_t *align,
                                           display_hal_text_valign_t *valign)
{
    if (lua_isnoneornil(L, index)) {
        return;
    }
    luaL_checktype(L, index, LUA_TTABLE);

    lua_getfield(L, index, "r");
    if (!lua_isnil(L, -1) && text_r) {
        *text_r = (uint8_t)lua_display_check_integer_arg(L, -1, "text color component");
    }
    lua_pop(L, 1);

    lua_getfield(L, index, "g");
    if (!lua_isnil(L, -1) && text_g) {
        *text_g = (uint8_t)lua_display_check_integer_arg(L, -1, "text color component");
    }
    lua_pop(L, 1);

    lua_getfield(L, index, "b");
    if (!lua_isnil(L, -1) && text_b) {
        *text_b = (uint8_t)lua_display_check_integer_arg(L, -1, "text color component");
    }
    lua_pop(L, 1);

    lua_getfield(L, index, "font_size");
    if (!lua_isnil(L, -1) && font_size) {
        *font_size = (uint8_t)lua_display_check_integer_arg(L, -1, "font_size");
    }
    lua_pop(L, 1);

    lua_getfield(L, index, "bg_r");
    if (!lua_isnil(L, -1) && bg_r) {
        *bg_r = (uint8_t)lua_display_check_integer_arg(L, -1, "background color component");
        if (has_bg) { *has_bg = true; }
    }
    lua_pop(L, 1);

    lua_getfield(L, index, "bg_g");
    if (!lua_isnil(L, -1) && bg_g) {
        *bg_g = (uint8_t)lua_display_check_integer_arg(L, -1, "background color component");
        if (has_bg) { *has_bg = true; }
    }
    lua_pop(L, 1);

    lua_getfield(L, index, "bg_b");
    if (!lua_isnil(L, -1) && bg_b) {
        *bg_b = (uint8_t)lua_display_check_integer_arg(L, -1, "background color component");
        if (has_bg) { *has_bg = true; }
    }
    lua_pop(L, 1);

    lua_getfield(L, index, "align");
    if (!lua_isnil(L, -1) && align) {
        *align = lua_display_parse_align(L, -1, *align);
    }
    lua_pop(L, 1);

    lua_getfield(L, index, "valign");
    if (!lua_isnil(L, -1) && valign) {
        *valign = lua_display_parse_valign(L, -1, *valign);
    }
    lua_pop(L, 1);
}

/* -------------------------------------------------------------------------
 * Text drawing
 * ---------------------------------------------------------------------- */

static int lua_display_draw_text(lua_State *L)
{
    int x = lua_display_check_integer_arg(L, 1, "x");
    int y = lua_display_check_integer_arg(L, 2, "y");
    const char *text = luaL_checkstring(L, 3);
    uint8_t text_r = 255, text_g = 255, text_b = 255;
    uint8_t bg_r = 0, bg_g = 0, bg_b = 0;
    uint8_t font_size = 24;
    bool has_bg = false;

    if (!lua_display_is_ascii_text(text)) {
        return luaL_error(L,
                          "display draw_text only supports ASCII text; "
                          "use draw_png_file/draw_jpeg_file for Chinese or other Unicode text");
    }

    lua_display_parse_text_options(L, 4,
                                   &text_r, &text_g, &text_b, &font_size,
                                   &bg_r, &bg_g, &bg_b, &has_bg, NULL, NULL);

    esp_err_t err = display_hal_draw_text(x, y, text, font_size,
                                          lua_display_rgb888_to_rgb565(text_r, text_g, text_b),
                                          has_bg,
                                          lua_display_rgb888_to_rgb565(bg_r, bg_g, bg_b));
    if (err != ESP_OK) {
        return luaL_error(L, "display draw_text failed: %s", esp_err_to_name(err));
    }
    return 0;
}

static int lua_display_measure_text(lua_State *L)
{
    const char *text = luaL_checkstring(L, 1);
    uint8_t font_size = 24;
    uint16_t width = 0, height = 0;

    lua_display_parse_text_options(L, 2,
                                   NULL, NULL, NULL, &font_size,
                                   NULL, NULL, NULL, NULL, NULL, NULL);

    esp_err_t err = display_hal_measure_text(text, font_size, &width, &height);
    if (err != ESP_OK) {
        return luaL_error(L, "display measure_text failed: %s", esp_err_to_name(err));
    }
    lua_pushinteger(L, width);
    lua_pushinteger(L, height);
    return 2;
}

static int lua_display_draw_text_aligned(lua_State *L)
{
    int x      = lua_display_check_integer_arg(L, 1, "x");
    int y      = lua_display_check_integer_arg(L, 2, "y");
    int width  = lua_display_check_integer_arg(L, 3, "width");
    int height = lua_display_check_integer_arg(L, 4, "height");
    const char *text = luaL_checkstring(L, 5);
    uint8_t text_r = 255, text_g = 255, text_b = 255;
    uint8_t bg_r = 0, bg_g = 0, bg_b = 0;
    uint8_t font_size = 24;
    bool has_bg = false;
    display_hal_text_align_t  align  = DISPLAY_HAL_TEXT_ALIGN_LEFT;
    display_hal_text_valign_t valign = DISPLAY_HAL_TEXT_VALIGN_TOP;

    if (!lua_display_is_ascii_text(text)) {
        return luaL_error(L,
                          "display draw_text_aligned only supports ASCII text; "
                          "use draw_png_file/draw_jpeg_file for Chinese or other Unicode text");
    }

    lua_display_parse_text_options(L, 6,
                                   &text_r, &text_g, &text_b, &font_size,
                                   &bg_r, &bg_g, &bg_b, &has_bg, &align, &valign);

    esp_err_t err = display_hal_draw_text_aligned(
        x, y, width, height, text, font_size,
        lua_display_rgb888_to_rgb565(text_r, text_g, text_b),
        has_bg,
        lua_display_rgb888_to_rgb565(bg_r, bg_g, bg_b),
        align, valign);
    if (err != ESP_OK) {
        return luaL_error(L, "display draw_text_aligned failed: %s", esp_err_to_name(err));
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * Basic drawing
 * ---------------------------------------------------------------------- */

static int lua_display_clear(lua_State *L)
{
    uint16_t color = lua_display_color(L, 1);
    esp_err_t err = display_hal_clear(color);
    if (err != ESP_OK) {
        return luaL_error(L, "display clear failed: %s", esp_err_to_name(err));
    }
    return 0;
}

static int lua_display_set_clip_rect(lua_State *L)
{
    int x      = lua_display_check_integer_arg(L, 1, "x");
    int y      = lua_display_check_integer_arg(L, 2, "y");
    int width  = lua_display_check_integer_arg(L, 3, "width");
    int height = lua_display_check_integer_arg(L, 4, "height");
    esp_err_t err = display_hal_set_clip_rect(x, y, width, height);
    if (err != ESP_OK) {
        return luaL_error(L, "display set_clip_rect failed: %s", esp_err_to_name(err));
    }
    return 0;
}

static int lua_display_clear_clip_rect(lua_State *L)
{
    (void)L;
    esp_err_t err = display_hal_clear_clip_rect();
    if (err != ESP_OK) {
        return luaL_error(L, "display clear_clip_rect failed: %s", esp_err_to_name(err));
    }
    return 0;
}

static int lua_display_fill_rect(lua_State *L)
{
    int x      = lua_display_check_integer_arg(L, 1, "x");
    int y      = lua_display_check_integer_arg(L, 2, "y");
    int width  = lua_display_check_integer_arg(L, 3, "width");
    int height = lua_display_check_integer_arg(L, 4, "height");
    uint16_t color = lua_display_color(L, 5);
    esp_err_t err = display_hal_fill_rect(x, y, width, height, color);
    if (err != ESP_OK) {
        return luaL_error(L, "display fill_rect failed: %s", esp_err_to_name(err));
    }
    return 0;
}

static int lua_display_draw_pixel(lua_State *L)
{
    int x = lua_display_check_integer_arg(L, 1, "x");
    int y = lua_display_check_integer_arg(L, 2, "y");
    uint16_t color = lua_display_color(L, 3);
    esp_err_t err = display_hal_draw_pixel(x, y, color);
    if (err != ESP_OK) {
        return luaL_error(L, "display draw_pixel failed: %s", esp_err_to_name(err));
    }
    return 0;
}

static int lua_display_draw_line(lua_State *L)
{
    int x0 = lua_display_check_integer_arg(L, 1, "x0");
    int y0 = lua_display_check_integer_arg(L, 2, "y0");
    int x1 = lua_display_check_integer_arg(L, 3, "x1");
    int y1 = lua_display_check_integer_arg(L, 4, "y1");
    uint16_t color = lua_display_color(L, 5);
    esp_err_t err = display_hal_draw_line(x0, y0, x1, y1, color);
    if (err != ESP_OK) {
        return luaL_error(L, "display draw_line failed: %s", esp_err_to_name(err));
    }
    return 0;
}

static int lua_display_draw_rect(lua_State *L)
{
    int x      = lua_display_check_integer_arg(L, 1, "x");
    int y      = lua_display_check_integer_arg(L, 2, "y");
    int width  = lua_display_check_integer_arg(L, 3, "width");
    int height = lua_display_check_integer_arg(L, 4, "height");
    uint16_t color = lua_display_color(L, 5);
    esp_err_t err = display_hal_draw_rect(x, y, width, height, color);
    if (err != ESP_OK) {
        return luaL_error(L, "display draw_rect failed: %s", esp_err_to_name(err));
    }
    return 0;
}

static int lua_display_backlight(lua_State *L)
{
    int on = lua_toboolean(L, 1);
    esp_err_t err = display_hal_set_backlight(on != 0);
    if (err != ESP_OK) {
        return luaL_error(L, "display backlight failed: %s", esp_err_to_name(err));
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * Frame management
 * ---------------------------------------------------------------------- */

static void lua_display_parse_frame_options(lua_State *L, int index,
                                            bool *clear, uint8_t *red,
                                            uint8_t *green, uint8_t *blue)
{
    if (clear)  { *clear = true; }
    if (red)    { *red   = 0; }
    if (green)  { *green = 0; }
    if (blue)   { *blue  = 0; }

    if (lua_isnoneornil(L, index)) {
        return;
    }
    luaL_checktype(L, index, LUA_TTABLE);

    lua_getfield(L, index, "clear");
    if (!lua_isnil(L, -1) && clear) {
        *clear = lua_toboolean(L, -1);
    }
    lua_pop(L, 1);

    lua_getfield(L, index, "r");
    if (!lua_isnil(L, -1) && red) {
        *red = (uint8_t)lua_display_check_integer_arg(L, -1, "frame color component");
    }
    lua_pop(L, 1);

    lua_getfield(L, index, "g");
    if (!lua_isnil(L, -1) && green) {
        *green = (uint8_t)lua_display_check_integer_arg(L, -1, "frame color component");
    }
    lua_pop(L, 1);

    lua_getfield(L, index, "b");
    if (!lua_isnil(L, -1) && blue) {
        *blue = (uint8_t)lua_display_check_integer_arg(L, -1, "frame color component");
    }
    lua_pop(L, 1);
}

static int lua_display_begin_frame(lua_State *L)
{
    bool clear = true;
    uint8_t red = 0, green = 0, blue = 0;
    lua_display_parse_frame_options(L, 1, &clear, &red, &green, &blue);
    esp_err_t err = display_hal_begin_frame(
        clear, lua_display_rgb888_to_rgb565(red, green, blue));
    if (err != ESP_OK) {
        return luaL_error(L, "display begin_frame failed: %s", esp_err_to_name(err));
    }
    return 0;
}

static int lua_display_present(lua_State *L)
{
    (void)L;
    esp_err_t err = display_hal_present();
    if (err != ESP_OK) {
        return luaL_error(L, "display present failed: %s", esp_err_to_name(err));
    }
    return 0;
}

static int lua_display_present_rect(lua_State *L)
{
    int x = lua_display_check_integer_arg(L, 1, "x");
    int y = lua_display_check_integer_arg(L, 2, "y");
    int width = lua_display_check_integer_arg(L, 3, "width");
    int height = lua_display_check_integer_arg(L, 4, "height");
    esp_err_t err = display_hal_present_rect(x, y, width, height);
    if (err != ESP_OK) {
        return luaL_error(L, "display present_rect failed: %s", esp_err_to_name(err));
    }
    return 0;
}

static int lua_display_end_frame(lua_State *L)
{
    (void)L;
    esp_err_t err = display_hal_end_frame();
    if (err != ESP_OK) {
        return luaL_error(L, "display end_frame failed: %s", esp_err_to_name(err));
    }
    return 0;
}

static int lua_display_frame_active(lua_State *L)
{
    lua_pushboolean(L, display_hal_is_frame_active());
    return 1;
}

static int lua_display_animation_info(lua_State *L)
{
    display_hal_animation_info_t info = {0};
    esp_err_t err = display_hal_get_animation_info(&info);
    if (err != ESP_OK) {
        return luaL_error(L, "display animation_info failed: %s", esp_err_to_name(err));
    }
    lua_newtable(L);
    lua_pushinteger(L, info.framebuffer_count);
    lua_setfield(L, -2, "framebuffer_count");
    lua_pushboolean(L, info.double_buffered);
    lua_setfield(L, -2, "double_buffered");
    lua_pushboolean(L, info.frame_active);
    lua_setfield(L, -2, "frame_active");
    lua_pushboolean(L, info.flush_in_flight);
    lua_setfield(L, -2, "flush_in_flight");
    return 1;
}

static int lua_display_width(lua_State *L)
{
    lua_pushinteger(L, display_hal_width());
    return 1;
}

static int lua_display_height(lua_State *L)
{
    lua_pushinteger(L, display_hal_height());
    return 1;
}

/* -------------------------------------------------------------------------
 * Bitmap
 * ---------------------------------------------------------------------- */

static int lua_display_draw_bitmap(lua_State *L)
{
    int x = (int)luaL_checkinteger(L, 1);
    int y = (int)luaL_checkinteger(L, 2);
    int w = (int)luaL_checkinteger(L, 3);
    int h = (int)luaL_checkinteger(L, 4);
    if (w <= 0 || h <= 0) {
        return luaL_error(L, "draw_bitmap: invalid size (%d x %d)", w, h);
    }

    size_t expected = (size_t)w * (size_t)h * 2;
    size_t data_len = 0;
    const uint8_t *data = lua_display_check_buffer_arg(L, 5, expected, &data_len);
    if (data_len < expected) {
        return luaL_error(L, "draw_bitmap: data too short (%d bytes, need %d)",
                          (int)data_len, (int)expected);
    }

    esp_err_t err = display_hal_draw_bitmap(x, y, w, h, (const uint16_t *)data);
    if (err != ESP_OK) {
        return luaL_error(L, "display draw_bitmap failed: %s", esp_err_to_name(err));
    }
    return 0;
}

static int lua_display_draw_bitmap_crop(lua_State *L)
{
    int x          = lua_display_check_integer_arg(L, 1, "x");
    int y          = lua_display_check_integer_arg(L, 2, "y");
    int src_x      = lua_display_check_integer_arg(L, 3, "src_x");
    int src_y      = lua_display_check_integer_arg(L, 4, "src_y");
    int w          = lua_display_check_integer_arg(L, 5, "width");
    int h          = lua_display_check_integer_arg(L, 6, "height");
    int src_width  = lua_display_check_integer_arg(L, 7, "src_width");
    int src_height = lua_display_check_integer_arg(L, 8, "src_height");

    if (src_width <= 0 || src_height <= 0) {
        return luaL_error(L, "draw_bitmap_crop: invalid source size (%d x %d)",
                          src_width, src_height);
    }

    size_t expected = (size_t)src_width * (size_t)src_height * 2;
    size_t data_len = 0;
    const uint8_t *data = lua_display_check_buffer_arg(L, 9, expected, &data_len);
    if (data_len < expected) {
        return luaL_error(L, "draw_bitmap_crop: data too short (%d bytes, need %d)",
                          (int)data_len, (int)expected);
    }

    esp_err_t err = display_hal_draw_bitmap_crop(x, y, src_x, src_y, w, h,
                                                  src_width, src_height,
                                                  (const uint16_t *)data);
    if (err != ESP_OK) {
        return luaL_error(L, "display draw_bitmap_crop failed: %s", esp_err_to_name(err));
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * File helpers
 * ---------------------------------------------------------------------- */

static bool lua_display_path_is_valid(const char *path)
{
    size_t len;

    if (!path || path[0] != '/') {
        return false;
    }
    /* Reject path traversal */
    if (strstr(path, "..")) {
        return false;
    }
    len = strlen(path);
    if (len <= 4) {
        return false;
    }
    /* Must end with a supported image extension */
    return strcmp(path + len - 4, ".jpg")  == 0 ||
           (len > 5 && strcmp(path + len - 5, ".jpeg") == 0) ||
           strcmp(path + len - 4, ".png")  == 0;
}

static esp_err_t lua_display_read_file(const char *path, uint8_t **data_out, size_t *size_out)
{
    FILE *f = NULL;
    long size = 0;
    uint8_t *data = NULL;

    if (!data_out || !size_out || !lua_display_path_is_valid(path)) {
        return ESP_ERR_INVALID_ARG;
    }
    *data_out = NULL;
    *size_out = 0;

    f = fopen(path, "rb");
    if (!f) {
        return ESP_ERR_NOT_FOUND;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return ESP_FAIL;
    }
    size = ftell(f);
    if (size <= 0) {
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return ESP_FAIL;
    }

    data = malloc((size_t)size);
    if (!data) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    if (fread(data, 1, (size_t)size, f) != (size_t)size) {
        fclose(f);
        free(data);
        return ESP_FAIL;
    }
    fclose(f);

    *data_out = data;
    *size_out = (size_t)size;
    return ESP_OK;
}

/* -------------------------------------------------------------------------
 * PNG decode (RGBA → RGB565, alpha pre-multiplied against black)
 * ---------------------------------------------------------------------- */

static esp_err_t lua_display_decode_png(const uint8_t *png_data, size_t png_len,
                                        uint8_t **pixels_out,
                                        uint32_t *width_out, uint32_t *height_out)
{
    png_image image;
    png_bytep rgba = NULL;
    uint8_t *rgb565 = NULL;
    size_t stride = 0;

    if (!png_data || png_len == 0 || !pixels_out || !width_out || !height_out) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(&image, 0, sizeof(image));
    image.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_memory(&image, png_data, png_len)) {
        return ESP_FAIL;
    }

    image.format = PNG_FORMAT_RGBA;
    stride = PNG_IMAGE_ROW_STRIDE(image);
    rgba = malloc(PNG_IMAGE_SIZE(image));
    if (!rgba) {
        png_image_free(&image);
        return ESP_ERR_NO_MEM;
    }

    if (!png_image_finish_read(&image, NULL, rgba, (png_int_32)stride, NULL)) {
        free(rgba);
        png_image_free(&image);
        return ESP_FAIL;
    }

    rgb565 = malloc((size_t)image.width * (size_t)image.height * 2);
    if (!rgb565) {
        free(rgba);
        png_image_free(&image);
        return ESP_ERR_NO_MEM;
    }

    /* Convert RGBA to RGB565, alpha pre-multiplied against black */
    for (uint32_t row = 0; row < image.height; row++) {
        const uint8_t *src_row = rgba + row * stride;
        uint8_t *dst_row = rgb565 + (size_t)row * image.width * 2;
        for (uint32_t col = 0; col < image.width; col++) {
            const uint8_t *src = src_row + col * 4;
            uint8_t alpha = src[3];
            uint8_t r = (uint8_t)((src[0] * alpha + 127) / 255);
            uint8_t g = (uint8_t)((src[1] * alpha + 127) / 255);
            uint8_t b = (uint8_t)((src[2] * alpha + 127) / 255);
            uint16_t c = lua_display_rgb888_to_rgb565(r, g, b);
            dst_row[col * 2]     = (uint8_t)(c & 0xFF);
            dst_row[col * 2 + 1] = (uint8_t)(c >> 8);
        }
    }

    free(rgba);
    png_image_free(&image);

    *pixels_out = rgb565;
    *width_out  = image.width;
    *height_out = image.height;
    return ESP_OK;
}

/* -------------------------------------------------------------------------
 * JPEG drawing
 * ---------------------------------------------------------------------- */

/*
 * draw_jpeg(x, y, jpeg_data)
 *   jpeg_data: Lua string containing raw JPEG bytes.
 * Returns: width, height
 */
static int lua_display_draw_jpeg(lua_State *L)
{
    int x = (int)luaL_checkinteger(L, 1);
    int y = (int)luaL_checkinteger(L, 2);
    size_t jpeg_len = 0;
    const char *jpeg_data = luaL_checklstring(L, 3, &jpeg_len);
    int img_w = 0, img_h = 0;

    esp_err_t err = display_hal_draw_jpeg(x, y,
                                          (const uint8_t *)jpeg_data, jpeg_len,
                                          &img_w, &img_h);
    if (err != ESP_OK) {
        return luaL_error(L, "display draw_jpeg failed: %s", esp_err_to_name(err));
    }
    lua_pushinteger(L, img_w);
    lua_pushinteger(L, img_h);
    return 2;
}

static int lua_display_draw_jpeg_file(lua_State *L)
{
    int x = (int)luaL_checkinteger(L, 1);
    int y = (int)luaL_checkinteger(L, 2);
    const char *path = luaL_checkstring(L, 3);
    uint8_t *jpeg_data = NULL;
    size_t jpeg_len = 0;
    int img_w = 0, img_h = 0;

    esp_err_t err = lua_display_read_file(path, &jpeg_data, &jpeg_len);
    if (err != ESP_OK) {
        return luaL_error(L, "display draw_jpeg_file failed: %s", esp_err_to_name(err));
    }

    err = display_hal_draw_jpeg(x, y, jpeg_data, jpeg_len, &img_w, &img_h);
    free(jpeg_data);
    if (err != ESP_OK) {
        return luaL_error(L, "display draw_jpeg_file failed: %s", esp_err_to_name(err));
    }
    lua_pushinteger(L, img_w);
    lua_pushinteger(L, img_h);
    return 2;
}

static int lua_display_draw_png_file(lua_State *L)
{
    int x = (int)luaL_checkinteger(L, 1);
    int y = (int)luaL_checkinteger(L, 2);
    const char *path = luaL_checkstring(L, 3);
    uint8_t *png_data = NULL;
    size_t png_len = 0;
    uint8_t *rgb565 = NULL;
    uint32_t img_w = 0, img_h = 0;

    esp_err_t err = lua_display_read_file(path, &png_data, &png_len);
    if (err != ESP_OK) {
        return luaL_error(L, "display draw_png_file failed: %s", esp_err_to_name(err));
    }

    err = lua_display_decode_png(png_data, png_len, &rgb565, &img_w, &img_h);
    free(png_data);
    if (err != ESP_OK) {
        free(rgb565);
        return luaL_error(L, "display draw_png_file failed: %s", esp_err_to_name(err));
    }

    err = display_hal_draw_bitmap(x, y, (int)img_w, (int)img_h, (const uint16_t *)rgb565);
    free(rgb565);
    if (err != ESP_OK) {
        return luaL_error(L, "display draw_png_file failed: %s", esp_err_to_name(err));
    }
    lua_pushinteger(L, (lua_Integer)img_w);
    lua_pushinteger(L, (lua_Integer)img_h);
    return 2;
}

static int lua_display_draw_jpeg_crop(lua_State *L)
{
    int x     = (int)luaL_checkinteger(L, 1);
    int y     = (int)luaL_checkinteger(L, 2);
    int src_x = (int)luaL_checkinteger(L, 3);
    int src_y = (int)luaL_checkinteger(L, 4);
    int w     = (int)luaL_checkinteger(L, 5);
    int h     = (int)luaL_checkinteger(L, 6);
    size_t jpeg_len = 0;
    const char *jpeg_data = luaL_checklstring(L, 7, &jpeg_len);
    int img_w = 0, img_h = 0;

    esp_err_t err = display_hal_draw_jpeg_crop(x, y, src_x, src_y, w, h,
                                               (const uint8_t *)jpeg_data, jpeg_len,
                                               &img_w, &img_h);
    if (err != ESP_OK) {
        return luaL_error(L, "display draw_jpeg_crop failed: %s", esp_err_to_name(err));
    }
    lua_pushinteger(L, img_w);
    lua_pushinteger(L, img_h);
    return 2;
}

static int lua_display_draw_jpeg_file_crop(lua_State *L)
{
    int x     = (int)luaL_checkinteger(L, 1);
    int y     = (int)luaL_checkinteger(L, 2);
    int src_x = (int)luaL_checkinteger(L, 3);
    int src_y = (int)luaL_checkinteger(L, 4);
    int w     = (int)luaL_checkinteger(L, 5);
    int h     = (int)luaL_checkinteger(L, 6);
    const char *path = luaL_checkstring(L, 7);
    uint8_t *jpeg_data = NULL;
    size_t jpeg_len = 0;
    int img_w = 0, img_h = 0;

    esp_err_t err = lua_display_read_file(path, &jpeg_data, &jpeg_len);
    if (err != ESP_OK) {
        return luaL_error(L, "display draw_jpeg_file_crop failed: %s", esp_err_to_name(err));
    }

    err = display_hal_draw_jpeg_crop(x, y, src_x, src_y, w, h,
                                     jpeg_data, jpeg_len, &img_w, &img_h);
    free(jpeg_data);
    if (err != ESP_OK) {
        return luaL_error(L, "display draw_jpeg_file_crop failed: %s", esp_err_to_name(err));
    }
    lua_pushinteger(L, img_w);
    lua_pushinteger(L, img_h);
    return 2;
}

static int lua_align_down_8(int v) { return v & ~7; }

static int lua_display_align_down(int value, int align)
{
    return value - (value % align);
}

static int lua_display_draw_rgb565_crop(lua_State *L)
{
    int x = lua_display_check_integer_arg(L, 1, "x");
    int y = lua_display_check_integer_arg(L, 2, "y");
    int src_x = lua_display_check_integer_arg(L, 3, "src_x");
    int src_y = lua_display_check_integer_arg(L, 4, "src_y");
    int w = lua_display_check_integer_arg(L, 5, "width");
    int h = lua_display_check_integer_arg(L, 6, "height");
    int src_width = lua_display_check_integer_arg(L, 7, "src_width");
    int src_height = lua_display_check_integer_arg(L, 8, "src_height");
    size_t expected = 0;
    if (src_width <= 0 || src_height <= 0) {
        return luaL_error(L, "draw_rgb565_crop: invalid source size (%d x %d)", src_width, src_height);
    }
    expected = (size_t)src_width * (size_t)src_height * 2;
    size_t data_len = 0;
    const uint8_t *data = lua_display_check_buffer_arg(L, 9, expected, &data_len);
    if (data_len < expected) {
        return luaL_error(L, "draw_rgb565_crop: data too short (%d bytes, need %d)",
                          (int)data_len, (int)expected);
    }

    esp_err_t err = display_hal_draw_bitmap_crop(x, y, src_x, src_y, w, h,
                                                 src_width, src_height,
                                                 (const uint16_t *)data);
    if (err != ESP_OK) {
        return luaL_error(L, "display draw_rgb565_crop failed: %s", esp_err_to_name(err));
    }
    lua_pushinteger(L, w);
    lua_pushinteger(L, h);
    return 2;
}

static int lua_display_draw_rgb565_scaled(lua_State *L)
{
    int x = lua_display_check_integer_arg(L, 1, "x");
    int y = lua_display_check_integer_arg(L, 2, "y");
    int src_width = lua_display_check_integer_arg(L, 3, "src_width");
    int src_height = lua_display_check_integer_arg(L, 4, "src_height");
    int scale_w = lua_display_check_integer_arg(L, 5, "scale_w");
    int scale_h = lua_display_check_integer_arg(L, 6, "scale_h");
    size_t expected = 0;
    int out_w = 0;
    int out_h = 0;

    if (src_width <= 0 || src_height <= 0) {
        return luaL_error(L, "draw_rgb565_scaled: invalid source size (%d x %d)", src_width, src_height);
    }
    if (scale_w <= 0 || scale_h <= 0) {
        return luaL_error(L, "draw_rgb565_scaled: invalid scale size (%d x %d)", scale_w, scale_h);
    }
    expected = (size_t)src_width * (size_t)src_height * 2;
    size_t data_len = 0;
    const uint8_t *data = lua_display_check_buffer_arg(L, 7, expected, &data_len);
    if (data_len < expected) {
        return luaL_error(L, "draw_rgb565_scaled: data too short (%d bytes, need %d)",
                          (int)data_len, (int)expected);
    }

    esp_err_t err = display_hal_draw_bitmap_scaled(x, y,
                                                   (const uint16_t *)data,
                                                   src_width, src_height,
                                                   scale_w, scale_h,
                                                   &out_w, &out_h);
    if (err != ESP_OK) {
        return luaL_error(L, "display draw_rgb565_scaled failed: %s", esp_err_to_name(err));
    }
    lua_pushinteger(L, out_w);
    lua_pushinteger(L, out_h);
    return 2;
}

static int lua_display_draw_rgb565_fit(lua_State *L)
{
    int x = lua_display_check_integer_arg(L, 1, "x");
    int y = lua_display_check_integer_arg(L, 2, "y");
    int src_width = lua_display_check_integer_arg(L, 3, "src_width");
    int src_height = lua_display_check_integer_arg(L, 4, "src_height");
    int max_w = lua_display_check_integer_arg(L, 5, "max_w");
    int max_h = lua_display_check_integer_arg(L, 6, "max_h");
    size_t expected = 0;
    int out_w = 0;
    int out_h = 0;

    if (src_width <= 0 || src_height <= 0) {
        return luaL_error(L, "draw_rgb565_fit: invalid source size (%d x %d)", src_width, src_height);
    }
    if (max_w <= 0 || max_h <= 0) {
        return luaL_error(L, "draw_rgb565_fit: max_w and max_h must be positive");
    }
    expected = (size_t)src_width * (size_t)src_height * 2;
    size_t data_len = 0;
    const uint8_t *data = lua_display_check_buffer_arg(L, 7, expected, &data_len);
    if (data_len < expected) {
        return luaL_error(L, "draw_rgb565_fit: data too short (%d bytes, need %d)",
                          (int)data_len, (int)expected);
    }

    if (src_width <= max_w && src_height <= max_h) {
        esp_err_t err = display_hal_draw_bitmap(x, y, src_width, src_height, (const uint16_t *)data);
        if (err != ESP_OK) {
            return luaL_error(L, "display draw_rgb565_fit failed: %s", esp_err_to_name(err));
        }
        lua_pushinteger(L, src_width);
        lua_pushinteger(L, src_height);
        return 2;
    }

    double ratio_w = (double)max_w / src_width;
    double ratio_h = (double)max_h / src_height;
    double ratio = (ratio_w < ratio_h) ? ratio_w : ratio_h;
    int scale_w = (int)(src_width * ratio);
    int scale_h = (int)(src_height * ratio);

    if (scale_w <= 0) {
        scale_w = 1;
    }
    if (scale_h <= 0) {
        scale_h = 1;
    }
    if (scale_w >= 8) {
        scale_w = lua_display_align_down(scale_w, 8);
        if (scale_w == 0) {
            scale_w = 8;
        }
    }
    if (scale_h >= 8) {
        scale_h = lua_display_align_down(scale_h, 8);
        if (scale_h == 0) {
            scale_h = 8;
        }
    }

    esp_err_t err = display_hal_draw_bitmap_scaled(x, y,
                                                   (const uint16_t *)data,
                                                   src_width, src_height,
                                                   scale_w, scale_h,
                                                   &out_w, &out_h);
    if (err != ESP_OK) {
        return luaL_error(L, "display draw_rgb565_fit failed: %s", esp_err_to_name(err));
    }
    lua_pushinteger(L, out_w);
    lua_pushinteger(L, out_h);
    return 2;
}

/*
 * draw_jpeg_file_scaled(x, y, scale_w, scale_h, path)
 *   scale_w, scale_h must be positive multiples of 8; max downscale 1/8.
 * Returns: output_w, output_h
 */
static int lua_display_draw_jpeg_file_scaled(lua_State *L)
{
    int x       = (int)luaL_checkinteger(L, 1);
    int y       = (int)luaL_checkinteger(L, 2);
    int scale_w = (int)luaL_checkinteger(L, 3);
    int scale_h = (int)luaL_checkinteger(L, 4);
    const char *path = luaL_checkstring(L, 5);
    uint8_t *jpeg_data = NULL;
    size_t jpeg_len = 0;
    int out_w = 0, out_h = 0;

    if ((scale_w & 7) != 0 || (scale_h & 7) != 0 || scale_w <= 0 || scale_h <= 0) {
        return luaL_error(L,
            "draw_jpeg_file_scaled: scale_w and scale_h must be positive multiples of 8");
    }

    esp_err_t err = lua_display_read_file(path, &jpeg_data, &jpeg_len);
    if (err != ESP_OK) {
        return luaL_error(L, "display draw_jpeg_file_scaled failed: %s", esp_err_to_name(err));
    }

    err = display_hal_draw_jpeg_scaled(x, y, jpeg_data, jpeg_len,
                                       scale_w, scale_h, &out_w, &out_h);
    free(jpeg_data);
    if (err != ESP_OK) {
        return luaL_error(L, "display draw_jpeg_file_scaled failed: %s", esp_err_to_name(err));
    }
    lua_pushinteger(L, out_w);
    lua_pushinteger(L, out_h);
    return 2;
}

/*
 * draw_jpeg_file_fit(x, y, max_w, max_h, path)
 *   Scales JPEG to fit within max_w x max_h preserving aspect ratio.
 *   Output size is aligned to multiples of 8 (JPEG HW decode requirement).
 * Returns: output_w, output_h
 */
static int lua_display_draw_jpeg_file_fit(lua_State *L)
{
    int x     = (int)luaL_checkinteger(L, 1);
    int y     = (int)luaL_checkinteger(L, 2);
    int max_w = (int)luaL_checkinteger(L, 3);
    int max_h = (int)luaL_checkinteger(L, 4);
    const char *path = luaL_checkstring(L, 5);
    uint8_t *jpeg_data = NULL;
    size_t jpeg_len = 0;
    int orig_w = 0, orig_h = 0;
    int out_w = 0, out_h = 0;

    if (max_w <= 0 || max_h <= 0) {
        return luaL_error(L, "draw_jpeg_file_fit: max_w and max_h must be positive");
    }

    esp_err_t err = lua_display_read_file(path, &jpeg_data, &jpeg_len);
    if (err != ESP_OK) {
        return luaL_error(L, "display draw_jpeg_file_fit failed: %s", esp_err_to_name(err));
    }

    err = display_hal_jpeg_get_size(jpeg_data, jpeg_len, &orig_w, &orig_h);
    if (err != ESP_OK) {
        free(jpeg_data);
        return luaL_error(L, "display draw_jpeg_file_fit: cannot read JPEG header");
    }

    if (orig_w <= max_w && orig_h <= max_h) {
        /* Image already fits: draw at full resolution */
        err = display_hal_draw_jpeg(x, y, jpeg_data, jpeg_len, &out_w, &out_h);
        free(jpeg_data);
        if (err != ESP_OK) {
            return luaL_error(L, "display draw_jpeg_file_fit failed: %s", esp_err_to_name(err));
        }
        lua_pushinteger(L, out_w);
        lua_pushinteger(L, out_h);
        return 2;
    }

    double ratio_w = (double)max_w / orig_w;
    double ratio_h = (double)max_h / orig_h;
    double ratio = (ratio_w < ratio_h) ? ratio_w : ratio_h;
    if (ratio < 0.125) { ratio = 0.125; }   /* max downscale 1/8 */

    int scale_w = lua_align_down_8((int)(orig_w * ratio));
    int scale_h = lua_align_down_8((int)(orig_h * ratio));
    if (scale_w < 8) { scale_w = 8; }
    if (scale_h < 8) { scale_h = 8; }

    err = display_hal_draw_jpeg_scaled(x, y, jpeg_data, jpeg_len,
                                       scale_w, scale_h, &out_w, &out_h);
    free(jpeg_data);
    if (err != ESP_OK) {
        return luaL_error(L, "display draw_jpeg_file_fit failed: %s", esp_err_to_name(err));
    }
    lua_pushinteger(L, out_w);
    lua_pushinteger(L, out_h);
    return 2;
}

/* -------------------------------------------------------------------------
 * Shape drawing
 * ---------------------------------------------------------------------- */

static int lua_display_fill_circle(lua_State *L)
{
    int cx = lua_display_check_integer_arg(L, 1, "cx");
    int cy = lua_display_check_integer_arg(L, 2, "cy");
    int r  = lua_display_check_integer_arg(L, 3, "radius");
    uint16_t color = lua_display_color(L, 4);
    esp_err_t err = display_hal_fill_circle(cx, cy, r, color);
    if (err != ESP_OK) {
        return luaL_error(L, "display fill_circle failed: %s", esp_err_to_name(err));
    }
    return 0;
}

static int lua_display_draw_circle(lua_State *L)
{
    int cx = lua_display_check_integer_arg(L, 1, "cx");
    int cy = lua_display_check_integer_arg(L, 2, "cy");
    int r  = lua_display_check_integer_arg(L, 3, "radius");
    uint16_t color = lua_display_color(L, 4);
    esp_err_t err = display_hal_draw_circle(cx, cy, r, color);
    if (err != ESP_OK) {
        return luaL_error(L, "display draw_circle failed: %s", esp_err_to_name(err));
    }
    return 0;
}

static int lua_display_draw_arc(lua_State *L)
{
    int cx     = lua_display_check_integer_arg(L, 1, "cx");
    int cy     = lua_display_check_integer_arg(L, 2, "cy");
    int radius = lua_display_check_integer_arg(L, 3, "radius");
    float start_deg = lua_display_check_number_arg(L, 4, "start_deg");
    float end_deg   = lua_display_check_number_arg(L, 5, "end_deg");
    uint16_t color = lua_display_color(L, 6);
    esp_err_t err = display_hal_draw_arc(cx, cy, radius, start_deg, end_deg, color);
    if (err != ESP_OK) {
        return luaL_error(L, "display draw_arc failed: %s", esp_err_to_name(err));
    }
    return 0;
}

static int lua_display_fill_arc(lua_State *L)
{
    int cx           = lua_display_check_integer_arg(L, 1, "cx");
    int cy           = lua_display_check_integer_arg(L, 2, "cy");
    int inner_radius = lua_display_check_integer_arg(L, 3, "inner_radius");
    int outer_radius = lua_display_check_integer_arg(L, 4, "outer_radius");
    float start_deg  = lua_display_check_number_arg(L, 5, "start_deg");
    float end_deg    = lua_display_check_number_arg(L, 6, "end_deg");
    uint16_t color = lua_display_color(L, 7);
    esp_err_t err = display_hal_fill_arc(cx, cy, inner_radius, outer_radius,
                                         start_deg, end_deg, color);
    if (err != ESP_OK) {
        return luaL_error(L, "display fill_arc failed: %s", esp_err_to_name(err));
    }
    return 0;
}

static int lua_display_draw_ellipse(lua_State *L)
{
    int cx       = lua_display_check_integer_arg(L, 1, "cx");
    int cy       = lua_display_check_integer_arg(L, 2, "cy");
    int radius_x = lua_display_check_integer_arg(L, 3, "radius_x");
    int radius_y = lua_display_check_integer_arg(L, 4, "radius_y");
    uint16_t color = lua_display_color(L, 5);
    esp_err_t err = display_hal_draw_ellipse(cx, cy, radius_x, radius_y, color);
    if (err != ESP_OK) {
        return luaL_error(L, "display draw_ellipse failed: %s", esp_err_to_name(err));
    }
    return 0;
}

static int lua_display_fill_ellipse(lua_State *L)
{
    int cx       = lua_display_check_integer_arg(L, 1, "cx");
    int cy       = lua_display_check_integer_arg(L, 2, "cy");
    int radius_x = lua_display_check_integer_arg(L, 3, "radius_x");
    int radius_y = lua_display_check_integer_arg(L, 4, "radius_y");
    uint16_t color = lua_display_color(L, 5);
    esp_err_t err = display_hal_fill_ellipse(cx, cy, radius_x, radius_y, color);
    if (err != ESP_OK) {
        return luaL_error(L, "display fill_ellipse failed: %s", esp_err_to_name(err));
    }
    return 0;
}

static int lua_display_draw_round_rect(lua_State *L)
{
    int x      = lua_display_check_integer_arg(L, 1, "x");
    int y      = lua_display_check_integer_arg(L, 2, "y");
    int width  = lua_display_check_integer_arg(L, 3, "width");
    int height = lua_display_check_integer_arg(L, 4, "height");
    int radius = lua_display_check_integer_arg(L, 5, "radius");
    uint16_t color = lua_display_color(L, 6);
    esp_err_t err = display_hal_draw_round_rect(x, y, width, height, radius, color);
    if (err != ESP_OK) {
        return luaL_error(L, "display draw_round_rect failed: %s", esp_err_to_name(err));
    }
    return 0;
}

static int lua_display_fill_round_rect(lua_State *L)
{
    int x      = lua_display_check_integer_arg(L, 1, "x");
    int y      = lua_display_check_integer_arg(L, 2, "y");
    int width  = lua_display_check_integer_arg(L, 3, "width");
    int height = lua_display_check_integer_arg(L, 4, "height");
    int radius = lua_display_check_integer_arg(L, 5, "radius");
    uint16_t color = lua_display_color(L, 6);
    esp_err_t err = display_hal_fill_round_rect(x, y, width, height, radius, color);
    if (err != ESP_OK) {
        return luaL_error(L, "display fill_round_rect failed: %s", esp_err_to_name(err));
    }
    return 0;
}

static int lua_display_draw_triangle(lua_State *L)
{
    int x1 = lua_display_check_integer_arg(L, 1, "x1");
    int y1 = lua_display_check_integer_arg(L, 2, "y1");
    int x2 = lua_display_check_integer_arg(L, 3, "x2");
    int y2 = lua_display_check_integer_arg(L, 4, "y2");
    int x3 = lua_display_check_integer_arg(L, 5, "x3");
    int y3 = lua_display_check_integer_arg(L, 6, "y3");
    uint16_t color = lua_display_color(L, 7);
    esp_err_t err = display_hal_draw_triangle(x1, y1, x2, y2, x3, y3, color);
    if (err != ESP_OK) {
        return luaL_error(L, "display draw_triangle failed: %s", esp_err_to_name(err));
    }
    return 0;
}

static int lua_display_fill_triangle(lua_State *L)
{
    int x1 = lua_display_check_integer_arg(L, 1, "x1");
    int y1 = lua_display_check_integer_arg(L, 2, "y1");
    int x2 = lua_display_check_integer_arg(L, 3, "x2");
    int y2 = lua_display_check_integer_arg(L, 4, "y2");
    int x3 = lua_display_check_integer_arg(L, 5, "x3");
    int y3 = lua_display_check_integer_arg(L, 6, "y3");
    uint16_t color = lua_display_color(L, 7);
    esp_err_t err = display_hal_fill_triangle(x1, y1, x2, y2, x3, y3, color);
    if (err != ESP_OK) {
        return luaL_error(L, "display fill_triangle failed: %s", esp_err_to_name(err));
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * Module registration
 * ---------------------------------------------------------------------- */

int luaopen_display(lua_State *L)
{
    lua_newtable(L);

    lua_pushcfunction(L, lua_display_init);
    lua_setfield(L, -2, "init");
    lua_pushcfunction(L, lua_display_deinit);
    lua_setfield(L, -2, "deinit");
    lua_pushcfunction(L, lua_display_width);
    lua_setfield(L, -2, "width");
    lua_pushcfunction(L, lua_display_height);
    lua_setfield(L, -2, "height");

    lua_pushcfunction(L, lua_display_clear);
    lua_setfield(L, -2, "clear");
    lua_pushcfunction(L, lua_display_set_clip_rect);
    lua_setfield(L, -2, "set_clip_rect");
    lua_pushcfunction(L, lua_display_clear_clip_rect);
    lua_setfield(L, -2, "clear_clip_rect");

    lua_pushcfunction(L, lua_display_fill_rect);
    lua_setfield(L, -2, "fill_rect");
    lua_pushcfunction(L, lua_display_draw_rect);
    lua_setfield(L, -2, "draw_rect");
    lua_pushcfunction(L, lua_display_draw_pixel);
    lua_setfield(L, -2, "draw_pixel");
    lua_pushcfunction(L, lua_display_draw_line);
    lua_setfield(L, -2, "draw_line");

    lua_pushcfunction(L, lua_display_backlight);
    lua_setfield(L, -2, "backlight");

    lua_pushcfunction(L, lua_display_begin_frame);
    lua_setfield(L, -2, "begin_frame");
    lua_pushcfunction(L, lua_display_present);
    lua_setfield(L, -2, "present");
    lua_pushcfunction(L, lua_display_present_rect);
    lua_setfield(L, -2, "present_rect");
    lua_pushcfunction(L, lua_display_end_frame);
    lua_setfield(L, -2, "end_frame");
    lua_pushcfunction(L, lua_display_frame_active);
    lua_setfield(L, -2, "frame_active");
    lua_pushcfunction(L, lua_display_animation_info);
    lua_setfield(L, -2, "animation_info");

    lua_pushcfunction(L, lua_display_measure_text);
    lua_setfield(L, -2, "measure_text");
    lua_pushcfunction(L, lua_display_draw_text);
    lua_setfield(L, -2, "draw_text");
    lua_pushcfunction(L, lua_display_draw_text_aligned);
    lua_setfield(L, -2, "draw_text_aligned");

    lua_pushcfunction(L, lua_display_draw_bitmap);
    lua_setfield(L, -2, "draw_bitmap");
    lua_pushcfunction(L, lua_display_draw_bitmap_crop);
    lua_setfield(L, -2, "draw_bitmap_crop");
    lua_pushcfunction(L, lua_display_draw_rgb565_crop);
    lua_setfield(L, -2, "draw_rgb565_crop");
    lua_pushcfunction(L, lua_display_draw_rgb565_scaled);
    lua_setfield(L, -2, "draw_rgb565_scaled");
    lua_pushcfunction(L, lua_display_draw_rgb565_fit);
    lua_setfield(L, -2, "draw_rgb565_fit");

    lua_pushcfunction(L, lua_display_draw_jpeg);
    lua_setfield(L, -2, "draw_jpeg");
    lua_pushcfunction(L, lua_display_draw_jpeg_file);
    lua_setfield(L, -2, "draw_jpeg_file");
    lua_pushcfunction(L, lua_display_draw_png_file);
    lua_setfield(L, -2, "draw_png_file");
    lua_pushcfunction(L, lua_display_draw_jpeg_crop);
    lua_setfield(L, -2, "draw_jpeg_crop");
    lua_pushcfunction(L, lua_display_draw_jpeg_file_crop);
    lua_setfield(L, -2, "draw_jpeg_file_crop");
    lua_pushcfunction(L, lua_display_draw_jpeg_file_scaled);
    lua_setfield(L, -2, "draw_jpeg_file_scaled");
    lua_pushcfunction(L, lua_display_draw_jpeg_file_fit);
    lua_setfield(L, -2, "draw_jpeg_file_fit");

    lua_pushcfunction(L, lua_display_fill_circle);
    lua_setfield(L, -2, "fill_circle");
    lua_pushcfunction(L, lua_display_draw_circle);
    lua_setfield(L, -2, "draw_circle");
    lua_pushcfunction(L, lua_display_draw_arc);
    lua_setfield(L, -2, "draw_arc");
    lua_pushcfunction(L, lua_display_fill_arc);
    lua_setfield(L, -2, "fill_arc");

    lua_pushcfunction(L, lua_display_draw_ellipse);
    lua_setfield(L, -2, "draw_ellipse");
    lua_pushcfunction(L, lua_display_fill_ellipse);
    lua_setfield(L, -2, "fill_ellipse");

    lua_pushcfunction(L, lua_display_draw_round_rect);
    lua_setfield(L, -2, "draw_round_rect");
    lua_pushcfunction(L, lua_display_fill_round_rect);
    lua_setfield(L, -2, "fill_round_rect");

    lua_pushcfunction(L, lua_display_draw_triangle);
    lua_setfield(L, -2, "draw_triangle");
    lua_pushcfunction(L, lua_display_fill_triangle);
    lua_setfield(L, -2, "fill_triangle");

    return 1;
}

esp_err_t lua_module_display_register(void)
{
    esp_err_t err = cap_lua_register_module("display", luaopen_display);
    if (err != ESP_OK) {
        return err;
    }
    return cap_lua_register_runtime_cleanup(lua_display_runtime_cleanup);
}
