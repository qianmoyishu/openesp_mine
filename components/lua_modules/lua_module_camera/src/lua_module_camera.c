/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lua_module_camera.h"

#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "cap_lua.h"
#include "lauxlib.h"
#include "lua.h"
#include "lua_module_camera_service.h"

#define LUA_MODULE_CAMERA_NAME "camera"
#define LUA_MODULE_CAMERA_FRAME_MT "camera.frame"

typedef struct {
    uint8_t *data;
    size_t bytes;
    camera_frame_info_t info;
    bool released;
} lua_camera_frame_t;

static lua_camera_frame_t *lua_module_camera_check_frame(lua_State *L, int index)
{
    return (lua_camera_frame_t *)luaL_checkudata(L, index, LUA_MODULE_CAMERA_FRAME_MT);
}

static void lua_module_camera_release_frame_buffer(lua_camera_frame_t *frame)
{
    if (frame != NULL && !frame->released && frame->data != NULL) {
        camera_release_frame(frame->data);
        frame->data = NULL;
        frame->bytes = 0;
        frame->released = true;
    }
}

static void lua_module_camera_push_frame_info(lua_State *L, const lua_camera_frame_t *frame)
{
    lua_pushinteger(L, (lua_Integer)frame->bytes);
    lua_setfield(L, -2, "bytes");
    lua_pushinteger(L, frame->info.width);
    lua_setfield(L, -2, "width");
    lua_pushinteger(L, frame->info.height);
    lua_setfield(L, -2, "height");
    lua_pushinteger(L, (lua_Integer)frame->info.pixel_format);
    lua_setfield(L, -2, "pixel_format_raw");
    lua_pushstring(L, frame->info.pixel_format_str);
    lua_setfield(L, -2, "pixel_format");
    lua_pushinteger(L, (lua_Integer)frame->info.frame_bytes);
    lua_setfield(L, -2, "frame_bytes");
    lua_pushinteger(L, (lua_Integer)frame->info.timestamp_us);
    lua_setfield(L, -2, "timestamp_us");
    lua_pushboolean(L, !frame->released);
    lua_setfield(L, -2, "valid");
}

static int lua_module_camera_frame_data(lua_State *L)
{
    lua_camera_frame_t *frame = lua_module_camera_check_frame(L, 1);

    if (frame->released || frame->data == NULL) {
        return luaL_error(L, "camera frame is already released");
    }

    lua_pushlstring(L, (const char *)frame->data, frame->bytes);
    return 1;
}

static int lua_module_camera_frame_ptr(lua_State *L)
{
    lua_camera_frame_t *frame = lua_module_camera_check_frame(L, 1);

    if (frame->released || frame->data == NULL) {
        return luaL_error(L, "camera frame is already released");
    }

    lua_pushlightuserdata(L, frame->data);
    return 1;
}

static int lua_module_camera_frame_bytes(lua_State *L)
{
    lua_camera_frame_t *frame = lua_module_camera_check_frame(L, 1);

    lua_pushinteger(L, (lua_Integer)frame->bytes);
    return 1;
}

static int lua_module_camera_frame_release(lua_State *L)
{
    lua_camera_frame_t *frame = lua_module_camera_check_frame(L, 1);
    lua_module_camera_release_frame_buffer(frame);
    return 0;
}

static int lua_module_camera_frame_info(lua_State *L)
{
    lua_camera_frame_t *frame = lua_module_camera_check_frame(L, 1);

    lua_newtable(L);
    lua_module_camera_push_frame_info(L, frame);
    return 1;
}

static int lua_module_camera_frame_gc(lua_State *L)
{
    lua_camera_frame_t *frame = lua_module_camera_check_frame(L, 1);
    lua_module_camera_release_frame_buffer(frame);
    return 0;
}

static int lua_module_camera_frame_tostring(lua_State *L)
{
    lua_camera_frame_t *frame = lua_module_camera_check_frame(L, 1);

    lua_pushfstring(L,
                    "camera.frame(%s, bytes=%d, %dx%d, format=%s)",
                    frame->released ? "released" : "valid",
                    (int)frame->bytes,
                    (int)frame->info.width,
                    (int)frame->info.height,
                    frame->info.pixel_format_str);
    return 1;
}

static void lua_module_camera_create_frame_metatable(lua_State *L)
{
    if (luaL_newmetatable(L, LUA_MODULE_CAMERA_FRAME_MT) == 0) {
        lua_pop(L, 1);
        return;
    }

    lua_newtable(L);
    lua_pushcfunction(L, lua_module_camera_frame_data);
    lua_setfield(L, -2, "data");
    lua_pushcfunction(L, lua_module_camera_frame_ptr);
    lua_setfield(L, -2, "ptr");
    lua_pushcfunction(L, lua_module_camera_frame_bytes);
    lua_setfield(L, -2, "bytes");
    lua_pushcfunction(L, lua_module_camera_frame_release);
    lua_setfield(L, -2, "release");
    lua_pushcfunction(L, lua_module_camera_frame_info);
    lua_setfield(L, -2, "info");
    lua_setfield(L, -2, "__index");

    lua_pushcfunction(L, lua_module_camera_frame_gc);
    lua_setfield(L, -2, "__gc");
    lua_pushcfunction(L, lua_module_camera_frame_tostring);
    lua_setfield(L, -2, "__tostring");
    lua_pop(L, 1);
}

static bool lua_module_camera_has_suffix(const char *path, const char *suffix)
{
    size_t path_len;
    size_t suffix_len;

    if (path == NULL || suffix == NULL) {
        return false;
    }

    path_len = strlen(path);
    suffix_len = strlen(suffix);
    if (path_len < suffix_len) {
        return false;
    }

    path += path_len - suffix_len;
    for (size_t i = 0; i < suffix_len; ++i) {
        if (tolower((unsigned char)path[i]) != tolower((unsigned char)suffix[i])) {
            return false;
        }
    }
    return true;
}

static bool lua_module_camera_save_path_is_valid(const char *path)
{
    if (path == NULL || path[0] == '\0') {
        return false;
    }
    if (strstr(path, "..") != NULL) {
        return false;
    }

    return lua_module_camera_has_suffix(path, ".jpg") ||
           lua_module_camera_has_suffix(path, ".jpeg");
}

/* camera.open(dev_path)
 * Opens the camera device. Must be called before info() or capture().
 * Returns true on success, or raises an error. */
static int lua_module_camera_open(lua_State *L)
{
    const char *dev_path = luaL_checkstring(L, 1);
    esp_err_t err = camera_open(dev_path);

    if (err != ESP_OK) {
        return luaL_error(L, "camera open failed: %s", esp_err_to_name(err));
    }

    lua_pushboolean(L, 1);
    return 1;
}

/* camera.info()
 * Returns a table with stream info: width, height, pixel_format, pixel_format_raw.
 * Requires the camera to be opened first. */
static int lua_module_camera_info(lua_State *L)
{
    camera_stream_info_t info = {0};
    esp_err_t err = camera_get_stream_info(&info);

    if (err == ESP_ERR_INVALID_STATE) {
        return luaL_error(L, "camera info failed: camera not opened, call camera.open() first");
    }
    if (err != ESP_OK) {
        return luaL_error(L, "camera info failed: %s", esp_err_to_name(err));
    }

    lua_newtable(L);
    lua_pushinteger(L, info.width);
    lua_setfield(L, -2, "width");
    lua_pushinteger(L, info.height);
    lua_setfield(L, -2, "height");
    lua_pushinteger(L, (lua_Integer)info.pixel_format);
    lua_setfield(L, -2, "pixel_format_raw");
    lua_pushstring(L, info.pixel_format_str);
    lua_setfield(L, -2, "pixel_format");
    return 1;
}

/* camera.capture(save_path [, timeout_ms])
 * Captures a JPEG frame and writes it to save_path.
 * save_path must be a .jpg/.jpeg file and must not contain "..".
 * Returns a table with: path, bytes, width, height, pixel_format, pixel_format_raw,
 *                        frame_bytes, timestamp_us. */
static int lua_module_camera_capture(lua_State *L)
{
    camera_frame_info_t frame_info = {0};
    const char *path = luaL_checkstring(L, 1);
    int timeout_ms = (int)luaL_optinteger(L, 2, 0);
    uint8_t *jpeg_data = NULL;
    size_t jpeg_bytes = 0;
    FILE *file = NULL;
    esp_err_t err;

    if (!lua_module_camera_save_path_is_valid(path)) {
        return luaL_error(L, "save path must be a .jpg/.jpeg file and must not contain '..'");
    }
    if (timeout_ms < 0) {
        return luaL_error(L, "timeout_ms must be non-negative");
    }

    err = camera_capture_jpeg(timeout_ms, &jpeg_data, &jpeg_bytes, &frame_info);
    if (err == ESP_ERR_INVALID_STATE) {
        return luaL_error(L, "camera capture failed: camera not opened, call camera.open() first");
    }
    if (err != ESP_OK) {
        return luaL_error(L, "camera capture failed: %s", esp_err_to_name(err));
    }

    file = fopen(path, "wb");
    if (file == NULL) {
        camera_free_buffer(jpeg_data);
        return luaL_error(L, "camera capture failed: cannot open %s (errno=%d)", path, errno);
    }

    if (fwrite(jpeg_data, 1, jpeg_bytes, file) != jpeg_bytes) {
        fclose(file);
        unlink(path);
        camera_free_buffer(jpeg_data);
        return luaL_error(L, "camera capture failed: short write to %s", path);
    }

    fclose(file);
    camera_free_buffer(jpeg_data);

    lua_newtable(L);
    lua_pushstring(L, path);
    lua_setfield(L, -2, "path");
    lua_pushinteger(L, (lua_Integer)jpeg_bytes);
    lua_setfield(L, -2, "bytes");
    lua_pushinteger(L, frame_info.width);
    lua_setfield(L, -2, "width");
    lua_pushinteger(L, frame_info.height);
    lua_setfield(L, -2, "height");
    lua_pushinteger(L, (lua_Integer)frame_info.pixel_format);
    lua_setfield(L, -2, "pixel_format_raw");
    lua_pushstring(L, frame_info.pixel_format_str);
    lua_setfield(L, -2, "pixel_format");
    lua_pushinteger(L, (lua_Integer)frame_info.frame_bytes);
    lua_setfield(L, -2, "frame_bytes");
    lua_pushinteger(L, (lua_Integer)frame_info.timestamp_us);
    lua_setfield(L, -2, "timestamp_us");
    return 1;
}

/* camera.get_frame([timeout_ms])
 * Captures one raw frame into a native frame object.
 * Returns a camera.frame userdata with methods:
 *   frame:data(), frame:info(), frame:release() */
static int lua_module_camera_get_frame(lua_State *L)
{
    camera_frame_info_t frame_info = {0};
    int timeout_ms = (int)luaL_optinteger(L, 1, 0);
    uint8_t *frame_data = NULL;
    size_t frame_bytes = 0;
    lua_camera_frame_t *frame = NULL;
    esp_err_t err;

    if (timeout_ms < 0) {
        return luaL_error(L, "timeout_ms must be non-negative");
    }

    err = camera_capture_frame(timeout_ms, &frame_data, &frame_bytes, &frame_info);
    if (err == ESP_ERR_INVALID_STATE) {
        return luaL_error(L, "camera get_frame failed: camera not opened or previous frame not released");
    }
    if (err != ESP_OK) {
        return luaL_error(L, "camera get_frame failed: %s", esp_err_to_name(err));
    }

    frame = (lua_camera_frame_t *)lua_newuserdata(L, sizeof(*frame));
    memset(frame, 0, sizeof(*frame));
    frame->data = frame_data;
    frame->bytes = frame_bytes;
    frame->info = frame_info;
    frame->released = false;

    luaL_getmetatable(L, LUA_MODULE_CAMERA_FRAME_MT);
    lua_setmetatable(L, -2);
    return 1;
}

/* camera.release_frame(frame)
 * Releases a frame previously returned by camera.get_frame(). */
static int lua_module_camera_release_frame(lua_State *L)
{
    lua_camera_frame_t *frame = lua_module_camera_check_frame(L, 1);
    lua_module_camera_release_frame_buffer(frame);
    return 0;
}

/* camera.close()
 * Closes the camera device and releases all resources.
 * Returns true on success, or raises an error. */
static int lua_module_camera_close(lua_State *L)
{
    esp_err_t err = camera_close();

    if (err != ESP_OK) {
        return luaL_error(L, "camera close failed: %s", esp_err_to_name(err));
    }

    lua_pushboolean(L, 1);
    return 1;
}

int luaopen_camera(lua_State *L)
{
    static const luaL_Reg funcs[] = {
        {"open",          lua_module_camera_open},
        {"info",          lua_module_camera_info},
        {"capture",       lua_module_camera_capture},
        {"get_frame",     lua_module_camera_get_frame},
        {"release_frame", lua_module_camera_release_frame},
        {"close",         lua_module_camera_close},
        {NULL, NULL},
    };

    lua_module_camera_create_frame_metatable(L);
    lua_newtable(L);
    luaL_setfuncs(L, funcs, 0);
    return 1;
}

esp_err_t lua_module_camera_register(void)
{
    return cap_lua_register_module(LUA_MODULE_CAMERA_NAME, luaopen_camera);
}
