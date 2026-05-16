/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "display_hal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "display_arbiter.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_painter.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#if CONFIG_SOC_LCD_RGB_SUPPORTED
#include "esp_lcd_panel_rgb.h"
#endif
#if CONFIG_SOC_MIPI_DSI_SUPPORTED
#include "esp_lcd_mipi_dsi.h"
#endif
#if CONFIG_ESP_ROM_HAS_JPEG_DECODE
#include "rom/tjpgd.h"
#endif

static const char *TAG = "display_hal";

#define DISPLAY_HAL_FRAMEBUFFER_COUNT_MAX 2
#define DISPLAY_HAL_FLUSH_TIMEOUT_MS      2000
#define DISPLAY_HAL_TEXT_CACHE_ALIGN      32
#define DISPLAY_HAL_PI                    3.14159265358979323846f
#define DISPLAY_HAL_JPEG_WORKBUF_SIZE     3100

typedef struct {
    SemaphoreHandle_t lock;
    esp_lcd_panel_handle_t panel;
    esp_lcd_panel_io_handle_t io;
    display_hal_panel_if_t panel_if;
    esp_painter_handle_t painter;
    bool display_callbacks_registered;
    bool clip_enabled;
    int clip_x;
    int clip_y;
    int clip_width;
    int clip_height;
    int width;
    int height;
    size_t framebuffer_bytes;
    uint16_t *framebuffers[DISPLAY_HAL_FRAMEBUFFER_COUNT_MAX];
    uint8_t framebuffer_count;
    uint8_t draw_framebuffer_index;
    uint8_t visible_framebuffer_index;
    int8_t pending_framebuffer_index;
    bool frame_active;
    bool flush_in_flight;
    bool framebuffer_initialized;
    SemaphoreHandle_t display_flush_done;
    uint16_t *submit_swap_buffer;
    size_t submit_swap_buffer_pixels;
} display_hal_state_t;

static display_hal_state_t s_state;

static void display_hal_clear_clip_locked(void);
static esp_err_t display_hal_clear_display_callbacks_locked(void);
static bool display_hal_flush_done_isr(esp_lcd_panel_io_handle_t panel_io,
                                       esp_lcd_panel_io_event_data_t *edata,
                                       void *user_ctx);
#if CONFIG_SOC_LCD_RGB_SUPPORTED
static bool display_hal_flush_done_rgb_isr(esp_lcd_panel_handle_t panel,
                                           const esp_lcd_rgb_panel_event_data_t *edata,
                                           void *user_ctx);
#endif
#if CONFIG_SOC_MIPI_DSI_SUPPORTED
static bool display_hal_flush_done_dpi_isr(esp_lcd_panel_handle_t panel,
                                           esp_lcd_dpi_panel_event_data_t *edata,
                                           void *user_ctx);
#endif
static esp_err_t display_hal_register_display_callbacks_locked(void);
static esp_err_t display_hal_wait_flush_done_locked(TickType_t timeout_ticks);
static bool display_hal_clip_rect_to_screen_locked(int *x, int *y, int *width, int *height);

static bool display_hal_panel_requires_swap(void)
{
    return s_state.panel_if == DISPLAY_HAL_PANEL_IF_IO;
}

static void display_hal_bswap16_into(uint16_t *dst, const uint16_t *src, size_t pixel_count)
{
    for (size_t i = 0; i < pixel_count; ++i) {
        dst[i] = __builtin_bswap16(src[i]);
    }
}

static esp_err_t display_hal_lock(void)
{
    if (!s_state.lock) {
        s_state.lock = xSemaphoreCreateMutex();
    }
    ESP_RETURN_ON_FALSE(s_state.lock != NULL, ESP_ERR_NO_MEM, TAG, "create mutex failed");
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_state.lock, pdMS_TO_TICKS(1000)) == pdTRUE,
                        ESP_ERR_TIMEOUT, TAG, "mutex timeout");
    return ESP_OK;
}

static void display_hal_unlock(void)
{
    if (s_state.lock) {
        xSemaphoreGive(s_state.lock);
    }
}

static esp_err_t display_hal_require_created_locked(void)
{
    bool handles_ready = s_state.panel != NULL &&
                         s_state.width > 0 && s_state.height > 0;

    if (s_state.panel_if == DISPLAY_HAL_PANEL_IF_IO) {
        handles_ready = handles_ready && (s_state.io != NULL);
    }

    ESP_RETURN_ON_FALSE(handles_ready,
                        ESP_ERR_INVALID_STATE, TAG, "display not created");
    return ESP_OK;
}

static esp_err_t display_hal_ensure_display_locked(void)
{
    return display_hal_require_created_locked();
}

esp_err_t display_hal_create(esp_lcd_panel_handle_t panel_handle,
                             esp_lcd_panel_io_handle_t io_handle,
                             display_hal_panel_if_t panel_if,
                             int lcd_width,
                             int lcd_height)
{
    esp_err_t ret = display_hal_lock();

    if (ret != ESP_OK) {
        return ret;
    }

    ESP_GOTO_ON_FALSE(panel_handle != NULL, ESP_ERR_INVALID_ARG, fail, TAG, "panel handle missing");
    ESP_GOTO_ON_FALSE(panel_if >= DISPLAY_HAL_PANEL_IF_IO &&
                      panel_if <= DISPLAY_HAL_PANEL_IF_MIPI_DSI,
                      ESP_ERR_INVALID_ARG, fail, TAG, "invalid panel interface");
    if (panel_if == DISPLAY_HAL_PANEL_IF_IO) {
        ESP_GOTO_ON_FALSE(io_handle != NULL, ESP_ERR_INVALID_ARG, fail, TAG, "io handle missing");
    }
    ESP_GOTO_ON_FALSE(lcd_width > 0 && lcd_height > 0, ESP_ERR_INVALID_ARG,
                      fail, TAG, "invalid lcd size");

    if (s_state.panel == panel_handle &&
            s_state.io == io_handle &&
            s_state.panel_if == panel_if &&
            s_state.width == lcd_width &&
            s_state.height == lcd_height &&
            s_state.display_flush_done != NULL &&
            s_state.display_callbacks_registered &&
            (!display_hal_panel_requires_swap() || s_state.submit_swap_buffer != NULL)) {
        ESP_LOGD(TAG, "display_hal_create: already initialized with matching params, no-op");
        ret = ESP_OK;
        goto fail;
    }

    if (s_state.submit_swap_buffer) {
        ESP_LOGW(TAG, "display_hal_create: freeing leftover swap buffer (%u px)",
                 (unsigned)s_state.submit_swap_buffer_pixels);
        heap_caps_free(s_state.submit_swap_buffer);
        s_state.submit_swap_buffer = NULL;
        s_state.submit_swap_buffer_pixels = 0;
    }
    if (s_state.display_callbacks_registered) {
        ESP_LOGW(TAG, "display_hal_create: clearing leftover display callbacks");
        (void)display_hal_clear_display_callbacks_locked();
    }

    s_state.panel = panel_handle;
    s_state.io = io_handle;
    s_state.panel_if = panel_if;
    s_state.width = lcd_width;
    s_state.height = lcd_height;
    s_state.framebuffer_bytes = (size_t)s_state.width * (size_t)s_state.height * sizeof(uint16_t);
    s_state.framebuffer_count = 0;
    s_state.draw_framebuffer_index = 0;
    s_state.visible_framebuffer_index = 0;
    s_state.pending_framebuffer_index = -1;
    s_state.frame_active = false;
    s_state.flush_in_flight = false;
    s_state.framebuffer_initialized = false;
    if (display_hal_panel_requires_swap()) {
        s_state.submit_swap_buffer = heap_caps_aligned_alloc(16, (size_t)lcd_width * (size_t)lcd_height * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        ESP_GOTO_ON_FALSE(s_state.submit_swap_buffer != NULL, ESP_ERR_NO_MEM, fail, TAG, "alloc submit swap buffer failed");
        s_state.submit_swap_buffer_pixels = (size_t)lcd_width * (size_t)lcd_height;
    }
    display_hal_clear_clip_locked();

    if (!s_state.display_flush_done) {
        s_state.display_flush_done = xSemaphoreCreateBinary();
        ESP_GOTO_ON_FALSE(s_state.display_flush_done != NULL, ESP_ERR_NO_MEM, fail, TAG,
                          "create flush semaphore failed");
    }
    if (!s_state.display_callbacks_registered) {
        ESP_GOTO_ON_ERROR(display_hal_register_display_callbacks_locked(),
                          fail, TAG, "register flush callback failed");
    }

fail:
    if (ret != ESP_OK) {
        heap_caps_free(s_state.submit_swap_buffer);
        s_state.submit_swap_buffer = NULL;
        s_state.submit_swap_buffer_pixels = 0;
    }
    display_hal_unlock();
    return ret;
}

esp_err_t display_hal_destroy(void)
{
    esp_err_t ret = display_hal_lock();
    SemaphoreHandle_t flush_done_to_delete = NULL;
    SemaphoreHandle_t lock_to_delete = NULL;

    if (ret != ESP_OK) {
        return ret;
    }

    if (s_state.flush_in_flight) {
        ret = display_hal_wait_flush_done_locked(pdMS_TO_TICKS(DISPLAY_HAL_FLUSH_TIMEOUT_MS));
        if (ret != ESP_OK) {
            display_hal_unlock();
            return ret;
        }
    }

    if (s_state.display_callbacks_registered) {
        ret = display_hal_clear_display_callbacks_locked();
        if (ret != ESP_OK) {
            display_hal_unlock();
            return ret;
        }
    }

    for (size_t i = 0; i < DISPLAY_HAL_FRAMEBUFFER_COUNT_MAX; ++i) {
        heap_caps_free(s_state.framebuffers[i]);
        s_state.framebuffers[i] = NULL;
    }
    if (s_state.painter) {
        esp_painter_deinit(s_state.painter);
        s_state.painter = NULL;
    }

    flush_done_to_delete = s_state.display_flush_done;
    lock_to_delete = s_state.lock;

    s_state.panel = NULL;
    s_state.io = NULL;
    s_state.panel_if = DISPLAY_HAL_PANEL_IF_IO;
    s_state.display_callbacks_registered = false;
    s_state.width = 0;
    s_state.height = 0;
    s_state.framebuffer_bytes = 0;
    s_state.framebuffer_count = 0;
    s_state.draw_framebuffer_index = 0;
    s_state.visible_framebuffer_index = 0;
    s_state.pending_framebuffer_index = -1;
    s_state.frame_active = false;
    s_state.flush_in_flight = false;
    s_state.framebuffer_initialized = false;
    s_state.clip_enabled = false;
    s_state.clip_x = 0;
    s_state.clip_y = 0;
    s_state.clip_width = 0;
    s_state.clip_height = 0;
    heap_caps_free(s_state.submit_swap_buffer);
    s_state.submit_swap_buffer = NULL;
    s_state.submit_swap_buffer_pixels = 0;
    s_state.display_flush_done = NULL;
    s_state.lock = NULL;

    if (lock_to_delete) {
        xSemaphoreGive(lock_to_delete);
    }
    if (flush_done_to_delete) {
        vSemaphoreDelete(flush_done_to_delete);
    }
    if (lock_to_delete) {
        vSemaphoreDelete(lock_to_delete);
    }
    return ESP_OK;
}

static void display_hal_clear_clip_locked(void)
{
    s_state.clip_enabled = false;
    s_state.clip_x = 0;
    s_state.clip_y = 0;
    s_state.clip_width = s_state.width;
    s_state.clip_height = s_state.height;
}

static esp_err_t display_hal_clear_display_callbacks_locked(void)
{
    esp_err_t ret = ESP_OK;

    switch (s_state.panel_if) {
    case DISPLAY_HAL_PANEL_IF_MIPI_DSI:
#if CONFIG_SOC_MIPI_DSI_SUPPORTED
    {
        const esp_lcd_dpi_panel_event_callbacks_t callbacks = {0};
        ret = esp_lcd_dpi_panel_register_event_callbacks(s_state.panel, &callbacks, NULL);
        break;
    }
#else
        ret = ESP_ERR_NOT_SUPPORTED;
        break;
#endif
    case DISPLAY_HAL_PANEL_IF_RGB:
#if CONFIG_SOC_LCD_RGB_SUPPORTED
    {
        const esp_lcd_rgb_panel_event_callbacks_t callbacks = {0};
        ret = esp_lcd_rgb_panel_register_event_callbacks(s_state.panel, &callbacks, NULL);
        break;
    }
#else
        ret = ESP_ERR_NOT_SUPPORTED;
        break;
#endif
    case DISPLAY_HAL_PANEL_IF_IO:
    default: {
        const esp_lcd_panel_io_callbacks_t callbacks = {0};
        if (!s_state.io) {
            s_state.display_callbacks_registered = false;
            return ESP_OK;
        }
        ret = esp_lcd_panel_io_register_event_callbacks(s_state.io, &callbacks, NULL);
        break;
    }
    }

    ESP_RETURN_ON_ERROR(ret, TAG, "clear flush callback failed");
    s_state.display_callbacks_registered = false;
    return ESP_OK;
}

static esp_err_t display_hal_register_display_callbacks_locked(void)
{
    esp_err_t ret = ESP_OK;

    switch (s_state.panel_if) {
    case DISPLAY_HAL_PANEL_IF_MIPI_DSI:
#if CONFIG_SOC_MIPI_DSI_SUPPORTED
    {
        const esp_lcd_dpi_panel_event_callbacks_t callbacks = {
            .on_color_trans_done = display_hal_flush_done_dpi_isr,
        };
        ret = esp_lcd_dpi_panel_register_event_callbacks(s_state.panel, &callbacks, NULL);
        break;
    }
#else
        ret = ESP_ERR_NOT_SUPPORTED;
        break;
#endif
    case DISPLAY_HAL_PANEL_IF_RGB:
#if CONFIG_SOC_LCD_RGB_SUPPORTED
    {
        const esp_lcd_rgb_panel_event_callbacks_t callbacks = {
            .on_color_trans_done = display_hal_flush_done_rgb_isr,
        };
        ret = esp_lcd_rgb_panel_register_event_callbacks(s_state.panel, &callbacks, NULL);
        break;
    }
#else
        ret = ESP_ERR_NOT_SUPPORTED;
        break;
#endif
    case DISPLAY_HAL_PANEL_IF_IO:
    default: {
        const esp_lcd_panel_io_callbacks_t callbacks = {
            .on_color_trans_done = display_hal_flush_done_isr,
        };
        ESP_RETURN_ON_FALSE(s_state.io != NULL, ESP_ERR_INVALID_STATE, TAG, "io handle missing");
        ret = esp_lcd_panel_io_register_event_callbacks(s_state.io, &callbacks, NULL);
        break;
    }
    }

    ESP_RETURN_ON_ERROR(ret, TAG, "register flush callback failed");
    s_state.display_callbacks_registered = true;
    return ESP_OK;
}

static uint16_t *display_hal_get_draw_framebuffer_locked(void)
{
    if (s_state.framebuffer_count == 0) {
        return NULL;
    }
    return s_state.framebuffers[s_state.draw_framebuffer_index];
}

static uint16_t *display_hal_get_visible_framebuffer_locked(void)
{
    if (s_state.framebuffer_count == 0) {
        return NULL;
    }
    return s_state.framebuffers[s_state.visible_framebuffer_index];
}

static esp_err_t display_hal_alloc_framebuffer_locked(size_t index)
{
    if (index >= DISPLAY_HAL_FRAMEBUFFER_COUNT_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_state.framebuffers[index]) {
        return ESP_OK;
    }

    s_state.framebuffers[index] = heap_caps_aligned_alloc(
        16, s_state.framebuffer_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(s_state.framebuffers[index] != NULL, ESP_ERR_NO_MEM, TAG,
                        "framebuffer alloc failed");
    memset(s_state.framebuffers[index], 0, s_state.framebuffer_bytes);
    return ESP_OK;
}

static esp_err_t display_hal_ensure_framebuffer_locked(void)
{
    if (s_state.framebuffer_count > 0) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(display_hal_alloc_framebuffer_locked(0), TAG, "alloc framebuffer 0 failed");
    s_state.framebuffer_count = 1;
    s_state.draw_framebuffer_index = 0;
    s_state.visible_framebuffer_index = 0;
    s_state.pending_framebuffer_index = -1;

    if (display_hal_alloc_framebuffer_locked(1) == ESP_OK) {
        s_state.framebuffer_count = 2;
    }

    s_state.framebuffer_initialized = true;
    return ESP_OK;
}

static void display_hal_fill_framebuffer_locked(uint16_t *framebuffer, uint16_t panel_color)
{
    if (!framebuffer) {
        return;
    }
    size_t pixels = (size_t)s_state.width * (size_t)s_state.height;
    for (size_t i = 0; i < pixels; ++i) {
        framebuffer[i] = panel_color;
    }
}

static bool display_hal_get_clip_bounds_locked(int *left, int *top, int *right, int *bottom)
{
    int clip_left = 0;
    int clip_top = 0;
    int clip_right = s_state.width;
    int clip_bottom = s_state.height;

    if (s_state.clip_enabled) {
        clip_left = s_state.clip_x;
        clip_top = s_state.clip_y;
        clip_right = s_state.clip_x + s_state.clip_width;
        clip_bottom = s_state.clip_y + s_state.clip_height;

        if (clip_left < 0) {
            clip_left = 0;
        }
        if (clip_top < 0) {
            clip_top = 0;
        }
        if (clip_right > s_state.width) {
            clip_right = s_state.width;
        }
        if (clip_bottom > s_state.height) {
            clip_bottom = s_state.height;
        }
    }

    if (left) {
        *left = clip_left;
    }
    if (top) {
        *top = clip_top;
    }
    if (right) {
        *right = clip_right;
    }
    if (bottom) {
        *bottom = clip_bottom;
    }
    return clip_right > clip_left && clip_bottom > clip_top;
}

static bool display_hal_clip_rect_locked(int *x, int *y, int *width, int *height,
                                         int *src_x, int *src_y)
{
    int clip_left = 0;
    int clip_top = 0;
    int clip_right = 0;
    int clip_bottom = 0;
    int dst_x = 0;
    int dst_y = 0;
    int dst_w = 0;
    int dst_h = 0;
    int dst_right = 0;
    int dst_bottom = 0;

    if (!x || !y || !width || !height || *width <= 0 || *height <= 0) {
        return false;
    }
    if (!display_hal_get_clip_bounds_locked(&clip_left, &clip_top, &clip_right, &clip_bottom)) {
        return false;
    }

    dst_x = *x;
    dst_y = *y;
    dst_w = *width;
    dst_h = *height;
    dst_right = dst_x + dst_w;
    dst_bottom = dst_y + dst_h;

    if (dst_x < clip_left) {
        int delta = clip_left - dst_x;
        if (src_x) {
            *src_x += delta;
        }
        dst_x = clip_left;
    }
    if (dst_y < clip_top) {
        int delta = clip_top - dst_y;
        if (src_y) {
            *src_y += delta;
        }
        dst_y = clip_top;
    }
    if (dst_right > clip_right) {
        dst_right = clip_right;
    }
    if (dst_bottom > clip_bottom) {
        dst_bottom = clip_bottom;
    }

    dst_w = dst_right - dst_x;
    dst_h = dst_bottom - dst_y;
    if (dst_w <= 0 || dst_h <= 0) {
        return false;
    }

    *x = dst_x;
    *y = dst_y;
    *width = dst_w;
    *height = dst_h;
    return true;
}

static bool display_hal_clip_rect_to_screen_locked(int *x, int *y, int *width, int *height)
{
    int dst_x = 0;
    int dst_y = 0;
    int dst_right = 0;
    int dst_bottom = 0;

    if (!x || !y || !width || !height || *width <= 0 || *height <= 0) {
        return false;
    }

    dst_x = *x;
    dst_y = *y;
    dst_right = dst_x + *width;
    dst_bottom = dst_y + *height;

    if (dst_x < 0) {
        dst_x = 0;
    }
    if (dst_y < 0) {
        dst_y = 0;
    }
    if (dst_right > s_state.width) {
        dst_right = s_state.width;
    }
    if (dst_bottom > s_state.height) {
        dst_bottom = s_state.height;
    }

    *x = dst_x;
    *y = dst_y;
    *width = dst_right - dst_x;
    *height = dst_bottom - dst_y;
    return *width > 0 && *height > 0;
}

static float display_hal_normalize_degrees(float degrees)
{
    while (degrees < 0.0f) {
        degrees += 360.0f;
    }
    while (degrees >= 360.0f) {
        degrees -= 360.0f;
    }
    return degrees;
}

static bool display_hal_arc_is_full_sweep(float start_deg, float end_deg)
{
    float sweep = end_deg - start_deg;

    if (fabsf(sweep) >= 359.999f) {
        return true;
    }

    return fabsf(display_hal_normalize_degrees(start_deg) -
                 display_hal_normalize_degrees(end_deg)) < 0.001f;
}

static float display_hal_arc_sweep_degrees(float start_deg, float end_deg)
{
    float start = display_hal_normalize_degrees(start_deg);
    float end = display_hal_normalize_degrees(end_deg);
    float sweep = end - start;

    if (sweep < 0.0f) {
        sweep += 360.0f;
    }
    return sweep;
}

static float display_hal_point_angle_degrees(int dx, int dy)
{
    float degrees = atan2f((float)dy, (float)dx) * (180.0f / DISPLAY_HAL_PI);

    if (degrees < 0.0f) {
        degrees += 360.0f;
    }
    return degrees;
}

static bool display_hal_angle_in_arc(float angle_deg, float start_deg, float end_deg)
{
    float start = display_hal_normalize_degrees(start_deg);
    float end = display_hal_normalize_degrees(end_deg);
    float angle = display_hal_normalize_degrees(angle_deg);

    if (display_hal_arc_is_full_sweep(start_deg, end_deg)) {
        return true;
    }
    if (start <= end) {
        return angle >= start && angle <= end;
    }
    return angle >= start || angle <= end;
}

static IRAM_ATTR bool display_hal_flush_done_isr(esp_lcd_panel_io_handle_t panel_io,
                                                 esp_lcd_panel_io_event_data_t *edata,
                                                 void *user_ctx)
{
    BaseType_t high_task_woken = pdFALSE;

    (void)panel_io;
    (void)edata;
    (void)user_ctx;

    if (s_state.display_flush_done) {
        xSemaphoreGiveFromISR(s_state.display_flush_done, &high_task_woken);
    }

    return high_task_woken == pdTRUE;
}

#if CONFIG_SOC_LCD_RGB_SUPPORTED
static IRAM_ATTR bool display_hal_flush_done_rgb_isr(esp_lcd_panel_handle_t panel,
                                                     const esp_lcd_rgb_panel_event_data_t *edata,
                                                     void *user_ctx)
{
    (void)panel;
    (void)edata;
    (void)user_ctx;
    return display_hal_flush_done_isr(NULL, NULL, NULL);
}
#endif

#if CONFIG_SOC_MIPI_DSI_SUPPORTED
static IRAM_ATTR bool display_hal_flush_done_dpi_isr(esp_lcd_panel_handle_t panel,
                                                     esp_lcd_dpi_panel_event_data_t *edata,
                                                     void *user_ctx)
{
    (void)panel;
    (void)edata;
    (void)user_ctx;
    return display_hal_flush_done_isr(NULL, NULL, NULL);
}
#endif

static esp_err_t display_hal_wait_flush_done_locked(TickType_t timeout_ticks)
{
    if (!s_state.flush_in_flight) {
        return ESP_OK;
    }
    ESP_RETURN_ON_FALSE(s_state.display_flush_done != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "flush semaphore missing");

    if (xSemaphoreTake(s_state.display_flush_done, timeout_ticks) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    s_state.flush_in_flight = false;
    if (s_state.pending_framebuffer_index >= 0) {
        s_state.visible_framebuffer_index = (uint8_t)s_state.pending_framebuffer_index;
        s_state.pending_framebuffer_index = -1;
    }
    return ESP_OK;
}

static esp_err_t display_hal_submit_bitmap_locked(int x_start, int y_start,
                                                  int x_end, int y_end,
                                                  const uint16_t *pixels,
                                                  int pending_framebuffer_index,
                                                  bool wait_for_done)
{
    const uint16_t *submit_pixels = pixels;
    size_t pixel_count = 0;
    uint16_t *swap_buffer = NULL;
    esp_err_t ret = display_hal_wait_flush_done_locked(pdMS_TO_TICKS(DISPLAY_HAL_FLUSH_TIMEOUT_MS));
    if (ret != ESP_OK) {
        return ret;
    }

    if (s_state.display_flush_done) {
        while (xSemaphoreTake(s_state.display_flush_done, 0) == pdTRUE) {
        }
    }

    if (!display_arbiter_is_owner(DISPLAY_ARBITER_OWNER_LUA)) {
        s_state.flush_in_flight = false;
        if (pending_framebuffer_index >= 0) {
            s_state.visible_framebuffer_index = (uint8_t)pending_framebuffer_index;
            s_state.pending_framebuffer_index = -1;
        }
        return ESP_OK;
    }

    if (display_hal_panel_requires_swap()) {
        pixel_count = (size_t)(x_end - x_start) * (size_t)(y_end - y_start);
        ESP_RETURN_ON_FALSE(s_state.submit_swap_buffer != NULL, ESP_ERR_INVALID_STATE, TAG,
                            "submit swap buffer missing");
        swap_buffer = s_state.submit_swap_buffer;
        display_hal_bswap16_into(swap_buffer, pixels, pixel_count);
        submit_pixels = swap_buffer;
    }

    ret = esp_lcd_panel_draw_bitmap(s_state.panel, x_start, y_start, x_end, y_end, submit_pixels);
    if (ret != ESP_OK) {
        return ret;
    }

    s_state.flush_in_flight = true;
    s_state.pending_framebuffer_index = (int8_t)pending_framebuffer_index;

    if (wait_for_done) {
        ret = display_hal_wait_flush_done_locked(pdMS_TO_TICKS(DISPLAY_HAL_FLUSH_TIMEOUT_MS));
    }
    return ret;
}

static const esp_painter_basic_font_t *display_hal_get_font(uint8_t font_size)
{
    switch (font_size) {
#if CONFIG_ESP_PAINTER_BASIC_FONT_12
    case 12:
        return &esp_painter_basic_font_12;
#endif
#if CONFIG_ESP_PAINTER_BASIC_FONT_16
    case 16:
        return &esp_painter_basic_font_16;
#endif
#if CONFIG_ESP_PAINTER_BASIC_FONT_20
    case 20:
        return &esp_painter_basic_font_20;
#endif
#if CONFIG_ESP_PAINTER_BASIC_FONT_24
    case 0:
    case 24:
        return &esp_painter_basic_font_24;
#endif
#if CONFIG_ESP_PAINTER_BASIC_FONT_28
    case 28:
        return &esp_painter_basic_font_28;
#endif
#if CONFIG_ESP_PAINTER_BASIC_FONT_32
    case 32:
        return &esp_painter_basic_font_32;
#endif
#if CONFIG_ESP_PAINTER_BASIC_FONT_36
    case 36:
        return &esp_painter_basic_font_36;
#endif
#if CONFIG_ESP_PAINTER_BASIC_FONT_40
    case 40:
        return &esp_painter_basic_font_40;
#endif
#if CONFIG_ESP_PAINTER_BASIC_FONT_44
    case 44:
        return &esp_painter_basic_font_44;
#endif
#if CONFIG_ESP_PAINTER_BASIC_FONT_48
    case 48:
        return &esp_painter_basic_font_48;
#endif
    default:
        break;
    }

#if CONFIG_ESP_PAINTER_BASIC_FONT_24
    return &esp_painter_basic_font_24;
#elif CONFIG_ESP_PAINTER_BASIC_FONT_20
    return &esp_painter_basic_font_20;
#elif CONFIG_ESP_PAINTER_BASIC_FONT_16
    return &esp_painter_basic_font_16;
#elif CONFIG_ESP_PAINTER_BASIC_FONT_12
    return &esp_painter_basic_font_12;
#else
    return NULL;
#endif
}

static void display_hal_measure_text_raw(const char *text, const esp_painter_basic_font_t *font,
                                         uint16_t *out_width, uint16_t *out_height)
{
    uint16_t max_cols = 0;
    uint16_t cols = 0;
    uint16_t lines = 1;

    if (!text || !font) {
        if (out_width) {
            *out_width = 0;
        }
        if (out_height) {
            *out_height = 0;
        }
        return;
    }

    for (const char *p = text; *p; ++p) {
        if (*p == '\n') {
            if (cols > max_cols) {
                max_cols = cols;
            }
            cols = 0;
            lines++;
        } else if (*p != '\r') {
            cols++;
        }
    }
    if (cols > max_cols) {
        max_cols = cols;
    }

    if (out_width) {
        *out_width = (uint16_t)(max_cols * font->width);
    }
    if (out_height) {
        *out_height = (uint16_t)(lines * font->height);
    }
}

static esp_err_t display_hal_ensure_painter_locked(void)
{
    esp_painter_config_t painter_cfg = {
        .canvas = {
            .width = (uint16_t)s_state.width,
            .height = (uint16_t)s_state.height,
        },
        .color_format = ESP_PAINTER_COLOR_FORMAT_RGB565,
        .default_font = display_hal_get_font(24),
        .swap_rgb565 = false,
    };

    if (s_state.painter) {
        return ESP_OK;
    }
    ESP_RETURN_ON_FALSE(painter_cfg.default_font != NULL, ESP_ERR_NOT_SUPPORTED, TAG,
                        "no esp_painter font enabled");
    ESP_RETURN_ON_ERROR(esp_painter_init(&painter_cfg, &s_state.painter), TAG, "esp_painter_init failed");
    return ESP_OK;
}

static esp_err_t display_hal_fill_rect_locked(int x, int y, int width, int height, uint16_t color565)
{
    uint16_t *framebuffer = display_hal_get_draw_framebuffer_locked();

    if (!display_hal_clip_rect_locked(&x, &y, &width, &height, NULL, NULL)) {
        return ESP_OK;
    }

    if (s_state.frame_active && framebuffer) {
        if (s_state.flush_in_flight &&
            s_state.pending_framebuffer_index == (int8_t)s_state.draw_framebuffer_index) {
            ESP_RETURN_ON_ERROR(
                display_hal_wait_flush_done_locked(pdMS_TO_TICKS(DISPLAY_HAL_FLUSH_TIMEOUT_MS)),
                TAG, "wait flush failed");
        }
        for (int row = 0; row < height; ++row) {
            uint16_t *dst = framebuffer + ((size_t)(y + row) * s_state.width) + x;
            for (int col = 0; col < width; ++col) {
                dst[col] = color565;
            }
        }
        return ESP_OK;
    }

    uint16_t *line = malloc((size_t)width * sizeof(uint16_t));
    ESP_RETURN_ON_FALSE(line != NULL, ESP_ERR_NO_MEM, TAG, "line alloc failed");
    for (int i = 0; i < width; ++i) {
        line[i] = color565;
    }
    for (int row = 0; row < height; ++row) {
        esp_err_t ret = display_hal_submit_bitmap_locked(x, y + row, x + width, y + row + 1, line, -1, true);
        if (ret != ESP_OK) {
            free(line);
            return ret;
        }
    }
    free(line);
    return ESP_OK;
}

static esp_err_t display_hal_draw_pixel_locked(int x, int y, uint16_t color565)
{
    return display_hal_fill_rect_locked(x, y, 1, 1, color565);
}

static esp_err_t display_hal_draw_hline_locked(int x, int y, int width, uint16_t color565)
{
    return display_hal_fill_rect_locked(x, y, width, 1, color565);
}

static esp_err_t display_hal_draw_vline_locked(int x, int y, int height, uint16_t color565)
{
    return display_hal_fill_rect_locked(x, y, 1, height, color565);
}

static esp_err_t display_hal_draw_line_locked(int x0, int y0, int x1, int y1, uint16_t color565)
{
    if (y0 == y1) {
        int x = x0 < x1 ? x0 : x1;
        return display_hal_draw_hline_locked(x, y0, abs(x1 - x0) + 1, color565);
    }
    if (x0 == x1) {
        int y = y0 < y1 ? y0 : y1;
        return display_hal_draw_vline_locked(x0, y, abs(y1 - y0) + 1, color565);
    }

    int dx = abs(x1 - x0);
    int sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0);
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;

    while (true) {
        esp_err_t ret = display_hal_draw_pixel_locked(x0, y0, color565);
        if (ret != ESP_OK) {
            return ret;
        }
        if (x0 == x1 && y0 == y1) {
            break;
        }
        int e2 = err * 2;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
    return ESP_OK;
}

static esp_err_t display_hal_draw_rect_locked(int x, int y, int width, int height, uint16_t color565)
{
    if (width <= 0 || height <= 0) {
        return ESP_OK;
    }
    if (width == 1) {
        return display_hal_draw_vline_locked(x, y, height, color565);
    }
    if (height == 1) {
        return display_hal_draw_hline_locked(x, y, width, color565);
    }

    ESP_RETURN_ON_ERROR(display_hal_draw_hline_locked(x, y, width, color565), TAG, "draw top failed");
    ESP_RETURN_ON_ERROR(display_hal_draw_hline_locked(x, y + height - 1, width, color565), TAG,
                        "draw bottom failed");
    ESP_RETURN_ON_ERROR(display_hal_draw_vline_locked(x, y + 1, height - 2, color565), TAG,
                        "draw left failed");
    return display_hal_draw_vline_locked(x + width - 1, y + 1, height - 2, color565);
}

static esp_err_t display_hal_draw_bitmap_crop_locked(int x, int y,
                                                     int src_x, int src_y,
                                                     int w, int h,
                                                     int src_width, int src_height,
                                                     const uint16_t *pixels)
{
    if (!pixels || src_width <= 0 || src_height <= 0 || w <= 0 || h <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (src_x < 0) {
        x -= src_x;
        w += src_x;
        src_x = 0;
    }
    if (src_y < 0) {
        y -= src_y;
        h += src_y;
        src_y = 0;
    }
    if (src_x + w > src_width) {
        w = src_width - src_x;
    }
    if (src_y + h > src_height) {
        h = src_height - src_y;
    }
    if (w <= 0 || h <= 0) {
        return ESP_OK;
    }

    if (!display_hal_clip_rect_locked(&x, &y, &w, &h, &src_x, &src_y)) {
        return ESP_OK;
    }

    uint16_t *framebuffer = display_hal_get_draw_framebuffer_locked();
    if (s_state.frame_active && framebuffer) {
        if (s_state.flush_in_flight &&
            s_state.pending_framebuffer_index == (int8_t)s_state.draw_framebuffer_index) {
            ESP_RETURN_ON_ERROR(
                display_hal_wait_flush_done_locked(pdMS_TO_TICKS(DISPLAY_HAL_FLUSH_TIMEOUT_MS)),
                TAG, "wait flush failed");
        }
        for (int row = 0; row < h; ++row) {
            const uint16_t *src = pixels + ((size_t)(src_y + row) * src_width) + src_x;
            uint16_t *dst = framebuffer + ((size_t)(y + row) * s_state.width) + x;
            memcpy(dst, src, (size_t)w * sizeof(uint16_t));
        }
        return ESP_OK;
    }

    if (src_x == 0 && w == src_width) {
        const uint16_t *start = pixels + ((size_t)src_y * src_width);
        return display_hal_submit_bitmap_locked(x, y, x + w, y + h, start, -1, true);
    }

    for (int row = 0; row < h; ++row) {
        const uint16_t *row_ptr = pixels + ((size_t)(src_y + row) * src_width) + src_x;
        ESP_RETURN_ON_ERROR(
            display_hal_submit_bitmap_locked(x, y + row, x + w, y + row + 1, row_ptr, -1, true),
            TAG, "submit bitmap row failed");
    }
    return ESP_OK;
}

static esp_err_t display_hal_draw_bitmap_locked(int x, int y, int w, int h, const uint16_t *pixels)
{
    return display_hal_draw_bitmap_crop_locked(x, y, 0, 0, w, h, w, h, pixels);
}

static esp_err_t display_hal_present_locked(void)
{
    uint16_t *framebuffer = display_hal_get_draw_framebuffer_locked();

    if (!framebuffer || !s_state.frame_active) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_RETURN_ON_ERROR(
        display_hal_submit_bitmap_locked(
            0, 0, s_state.width, s_state.height,
            framebuffer, (int)s_state.draw_framebuffer_index, false),
        TAG, "present failed");

    if (s_state.framebuffer_count > 1) {
        uint16_t *prev_draw_fb = framebuffer;
        s_state.draw_framebuffer_index = (uint8_t)((s_state.draw_framebuffer_index + 1) %
                                                   s_state.framebuffer_count);
        uint16_t *new_draw_fb = display_hal_get_draw_framebuffer_locked();
        if (new_draw_fb && new_draw_fb != prev_draw_fb) {
            memcpy(new_draw_fb, prev_draw_fb, s_state.framebuffer_bytes);
        }
    }
    return ESP_OK;
}

static esp_err_t display_hal_present_rect_locked(int x, int y, int width, int height)
{
    uint16_t *draw_fb = display_hal_get_draw_framebuffer_locked();
    uint16_t *visible_fb = display_hal_get_visible_framebuffer_locked();

    if (!draw_fb || !s_state.frame_active) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!display_hal_clip_rect_to_screen_locked(&x, &y, &width, &height)) {
        return ESP_OK;
    }
    if (s_state.flush_in_flight) {
        ESP_RETURN_ON_ERROR(
            display_hal_wait_flush_done_locked(pdMS_TO_TICKS(DISPLAY_HAL_FLUSH_TIMEOUT_MS)),
            TAG, "wait flush failed");
    }

    if (width == s_state.width) {
        const uint16_t *start = draw_fb + ((size_t)y * s_state.width);
        ESP_RETURN_ON_ERROR(
            display_hal_submit_bitmap_locked(x, y, x + width, y + height, start, -1, true),
            TAG, "present rect failed");
    } else {
        for (int row = 0; row < height; ++row) {
            const uint16_t *row_ptr = draw_fb + ((size_t)(y + row) * s_state.width) + x;
            ESP_RETURN_ON_ERROR(
                display_hal_submit_bitmap_locked(
                    x, y + row, x + width, y + row + 1, row_ptr, -1, true),
                TAG, "present rect row failed");
        }
    }

    if (visible_fb && visible_fb != draw_fb) {
        for (int row = 0; row < height; ++row) {
            const uint16_t *src = draw_fb + ((size_t)(y + row) * s_state.width) + x;
            uint16_t *dst = visible_fb + ((size_t)(y + row) * s_state.width) + x;
            memcpy(dst, src, (size_t)width * sizeof(uint16_t));
        }
    }
    return ESP_OK;
}

static void display_hal_sort_vertices_by_y(int *x1, int *y1, int *x2, int *y2, int *x3, int *y3)
{
    if (*y1 > *y2) {
        int tx = *x1;
        int ty = *y1;
        *x1 = *x2;
        *y1 = *y2;
        *x2 = tx;
        *y2 = ty;
    }
    if (*y2 > *y3) {
        int tx = *x2;
        int ty = *y2;
        *x2 = *x3;
        *y2 = *y3;
        *x3 = tx;
        *y3 = ty;
    }
    if (*y1 > *y2) {
        int tx = *x1;
        int ty = *y1;
        *x1 = *x2;
        *y1 = *y2;
        *x2 = tx;
        *y2 = ty;
    }
}

#if CONFIG_ESP_ROM_HAS_JPEG_DECODE
typedef struct {
    const uint8_t *data;
    size_t len;
    size_t offset;
    uint16_t *pixels;
    int width;
    int height;
} display_hal_jpeg_ctx_t;

static esp_err_t display_hal_jpeg_result_to_err(JRESULT result)
{
    switch (result) {
    case JDR_OK:
        return ESP_OK;
    case JDR_PAR:
        return ESP_ERR_INVALID_ARG;
    case JDR_MEM1:
    case JDR_MEM2:
        return ESP_ERR_NO_MEM;
    case JDR_FMT1:
    case JDR_FMT2:
    case JDR_FMT3:
        return ESP_ERR_NOT_SUPPORTED;
    case JDR_INTR:
    case JDR_INP:
    default:
        return ESP_FAIL;
    }
}

static UINT display_hal_jpeg_input_cb(JDEC *decoder, BYTE *buf, UINT len)
{
    display_hal_jpeg_ctx_t *ctx = (display_hal_jpeg_ctx_t *)decoder->device;
    size_t remaining = ctx->len - ctx->offset;
    size_t chunk = len;

    if (chunk > remaining) {
        chunk = remaining;
    }
    if (buf) {
        memcpy(buf, ctx->data + ctx->offset, chunk);
    }
    ctx->offset += chunk;
    return (UINT)chunk;
}

static UINT display_hal_jpeg_output_cb(JDEC *decoder, void *bitmap, JRECT *rect)
{
    display_hal_jpeg_ctx_t *ctx = (display_hal_jpeg_ctx_t *)decoder->device;
    uint8_t *src = (uint8_t *)bitmap;

    for (int y = rect->top; y <= rect->bottom; ++y) {
        uint16_t *dst = ctx->pixels + ((size_t)y * (size_t)ctx->width) + rect->left;
        for (int x = rect->left; x <= rect->right; ++x) {
            uint16_t color =
                (uint16_t)(((src[0] >> 3) << 11) | ((src[1] >> 2) << 5) | (src[2] >> 3));
            *dst++ = color;
            src += 3;
        }
    }
    return 1;
}

static esp_err_t display_hal_decode_jpeg_rgb565(const uint8_t *jpeg_data, size_t jpeg_len,
                                                uint16_t **pixels_out, int *out_w, int *out_h)
{
    display_hal_jpeg_ctx_t ctx = {
        .data = jpeg_data,
        .len = jpeg_len,
    };
    JDEC decoder = {0};
    void *work = NULL;
    esp_err_t ret = ESP_OK;
    JRESULT result;

    if (!jpeg_data || jpeg_len == 0 || !pixels_out || !out_w || !out_h) {
        return ESP_ERR_INVALID_ARG;
    }

    work = calloc(1, DISPLAY_HAL_JPEG_WORKBUF_SIZE);
    ESP_GOTO_ON_FALSE(work != NULL, ESP_ERR_NO_MEM, cleanup, TAG, "jpeg workspace alloc failed");

    result = jd_prepare(&decoder, display_hal_jpeg_input_cb, work, DISPLAY_HAL_JPEG_WORKBUF_SIZE, &ctx);
    ret = display_hal_jpeg_result_to_err(result);
    ESP_GOTO_ON_ERROR(ret, cleanup, TAG, "jpeg header parse failed (%d)", result);

    ctx.width = (int)decoder.width;
    ctx.height = (int)decoder.height;
    ctx.pixels = malloc((size_t)ctx.width * (size_t)ctx.height * sizeof(uint16_t));
    ESP_GOTO_ON_FALSE(ctx.pixels != NULL, ESP_ERR_NO_MEM, cleanup, TAG, "jpeg pixel buffer alloc failed");

    result = jd_decomp(&decoder, display_hal_jpeg_output_cb, 0);
    ret = display_hal_jpeg_result_to_err(result);
    ESP_GOTO_ON_ERROR(ret, cleanup, TAG, "jpeg decode failed (%d)", result);

    *pixels_out = ctx.pixels;
    *out_w = ctx.width;
    *out_h = ctx.height;
    ctx.pixels = NULL;

cleanup:
    free(ctx.pixels);
    free(work);
    return ret;
}

static esp_err_t display_hal_jpeg_get_size_internal(const uint8_t *jpeg_data, size_t jpeg_len,
                                                    int *out_w, int *out_h)
{
    display_hal_jpeg_ctx_t ctx = {
        .data = jpeg_data,
        .len = jpeg_len,
    };
    JDEC decoder = {0};
    void *work = NULL;
    esp_err_t ret = ESP_OK;
    JRESULT result;

    if (!jpeg_data || jpeg_len == 0 || !out_w || !out_h) {
        return ESP_ERR_INVALID_ARG;
    }

    work = calloc(1, DISPLAY_HAL_JPEG_WORKBUF_SIZE);
    ESP_GOTO_ON_FALSE(work != NULL, ESP_ERR_NO_MEM, cleanup, TAG, "jpeg workspace alloc failed");

    result = jd_prepare(&decoder, display_hal_jpeg_input_cb, work, DISPLAY_HAL_JPEG_WORKBUF_SIZE, &ctx);
    ret = display_hal_jpeg_result_to_err(result);
    ESP_GOTO_ON_ERROR(ret, cleanup, TAG, "jpeg header parse failed (%d)", result);

    *out_w = (int)decoder.width;
    *out_h = (int)decoder.height;

cleanup:
    free(work);
    return ret;
}
#endif

static esp_err_t display_hal_scale_rgb565(const uint16_t *src, int src_w, int src_h,
                                          int dst_w, int dst_h, uint16_t **dst_out)
{
    uint16_t *dst = NULL;

    if (!src || src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0 || !dst_out) {
        return ESP_ERR_INVALID_ARG;
    }
    dst = malloc((size_t)dst_w * (size_t)dst_h * sizeof(uint16_t));
    ESP_RETURN_ON_FALSE(dst != NULL, ESP_ERR_NO_MEM, TAG, "scale buffer alloc failed");

    for (int y = 0; y < dst_h; ++y) {
        int src_y = (y * src_h) / dst_h;
        const uint16_t *src_row = src + ((size_t)src_y * src_w);
        uint16_t *dst_row = dst + ((size_t)y * dst_w);
        for (int x = 0; x < dst_w; ++x) {
            int src_x = (x * src_w) / dst_w;
            dst_row[x] = src_row[src_x];
        }
    }

    *dst_out = dst;
    return ESP_OK;
}

int display_hal_width(void)
{
    return s_state.width;
}

int display_hal_height(void)
{
    return s_state.height;
}

esp_err_t display_hal_begin_frame(bool clear, uint16_t color565)
{
    esp_err_t ret = display_hal_lock();
    uint16_t *draw_fb = NULL;
    uint16_t *visible_fb = NULL;

    if (ret != ESP_OK) {
        return ret;
    }

    ret = display_hal_ensure_display_locked();
    if (ret == ESP_OK) {
        ret = display_hal_ensure_framebuffer_locked();
    }
    if (ret == ESP_OK && s_state.flush_in_flight &&
        (!clear || s_state.pending_framebuffer_index == (int8_t)s_state.draw_framebuffer_index)) {
        ret = display_hal_wait_flush_done_locked(pdMS_TO_TICKS(DISPLAY_HAL_FLUSH_TIMEOUT_MS));
    }
    if (ret == ESP_OK) {
        s_state.frame_active = true;
        display_hal_clear_clip_locked();
        draw_fb = display_hal_get_draw_framebuffer_locked();
        visible_fb = display_hal_get_visible_framebuffer_locked();
        if (clear || !s_state.framebuffer_initialized) {
            display_hal_fill_framebuffer_locked(draw_fb, color565);
            s_state.framebuffer_initialized = true;
        } else if (draw_fb && visible_fb && draw_fb != visible_fb) {
            memcpy(draw_fb, visible_fb, s_state.framebuffer_bytes);
        }
    }

    display_hal_unlock();
    return ret;
}

esp_err_t display_hal_present(void)
{
    esp_err_t ret = display_hal_lock();

    if (ret != ESP_OK) {
        return ret;
    }
    if (ret == ESP_OK) {
        ret = display_hal_ensure_display_locked();
    }
    if (ret == ESP_OK) {
        ret = display_hal_present_locked();
    }
    display_hal_unlock();
    return ret;
}

esp_err_t display_hal_present_rect(int x, int y, int width, int height)
{
    esp_err_t ret = display_hal_lock();

    if (ret != ESP_OK) {
        return ret;
    }
    if (ret == ESP_OK) {
        ret = display_hal_ensure_display_locked();
    }
    if (ret == ESP_OK) {
        ret = display_hal_present_rect_locked(x, y, width, height);
    }
    display_hal_unlock();
    return ret;
}

esp_err_t display_hal_end_frame(void)
{
    esp_err_t ret = display_hal_lock();

    if (ret != ESP_OK) {
        return ret;
    }
    if (s_state.flush_in_flight) {
        ret = display_hal_wait_flush_done_locked(pdMS_TO_TICKS(DISPLAY_HAL_FLUSH_TIMEOUT_MS));
    }
    s_state.frame_active = false;
    display_hal_clear_clip_locked();
    display_hal_unlock();
    return ret;
}

bool display_hal_is_frame_active(void)
{
    bool active = false;

    if (display_hal_lock() == ESP_OK) {
        active = s_state.frame_active;
        display_hal_unlock();
    }
    return active;
}

esp_err_t display_hal_get_animation_info(display_hal_animation_info_t *info)
{
    esp_err_t ret = display_hal_lock();

    if (ret != ESP_OK) {
        return ret;
    }
    if (!info) {
        display_hal_unlock();
        return ESP_ERR_INVALID_ARG;
    }
    ret = display_hal_ensure_display_locked();
    if (ret == ESP_OK) {
        ret = display_hal_ensure_framebuffer_locked();
    }
    if (ret == ESP_OK) {
        info->framebuffer_count = s_state.framebuffer_count;
        info->double_buffered = s_state.framebuffer_count > 1;
        info->frame_active = s_state.frame_active;
        info->flush_in_flight = s_state.flush_in_flight;
    }
    display_hal_unlock();
    return ret;
}

esp_err_t display_hal_clear(uint16_t color565)
{
    return display_hal_fill_rect(0, 0, display_hal_width(), display_hal_height(), color565);
}

esp_err_t display_hal_set_clip_rect(int x, int y, int width, int height)
{
    esp_err_t ret = display_hal_lock();

    if (ret != ESP_OK) {
        return ret;
    }
    ret = display_hal_require_created_locked();
    if (ret != ESP_OK) {
        display_hal_unlock();
        return ret;
    }
    if (width <= 0 || height <= 0) {
        display_hal_unlock();
        return ESP_ERR_INVALID_ARG;
    }
    s_state.clip_enabled = true;
    s_state.clip_x = x;
    s_state.clip_y = y;
    s_state.clip_width = width;
    s_state.clip_height = height;
    display_hal_unlock();
    return ESP_OK;
}

esp_err_t display_hal_clear_clip_rect(void)
{
    esp_err_t ret = display_hal_lock();

    if (ret != ESP_OK) {
        return ret;
    }
    display_hal_clear_clip_locked();
    display_hal_unlock();
    return ESP_OK;
}

esp_err_t display_hal_fill_rect(int x, int y, int width, int height, uint16_t color565)
{
    esp_err_t ret = display_hal_lock();

    if (ret != ESP_OK) {
        return ret;
    }
    if (ret == ESP_OK) {
        ret = display_hal_ensure_display_locked();
    }
    if (ret == ESP_OK) {
        ret = display_hal_fill_rect_locked(x, y, width, height, color565);
    }
    display_hal_unlock();
    return ret;
}

esp_err_t display_hal_draw_line(int x0, int y0, int x1, int y1, uint16_t color565)
{
    esp_err_t ret = display_hal_lock();

    if (ret != ESP_OK) {
        return ret;
    }
    if (ret == ESP_OK) {
        ret = display_hal_ensure_display_locked();
    }
    if (ret == ESP_OK) {
        ret = display_hal_draw_line_locked(x0, y0, x1, y1, color565);
    }
    display_hal_unlock();
    return ret;
}

esp_err_t display_hal_draw_rect(int x, int y, int width, int height, uint16_t color565)
{
    esp_err_t ret = display_hal_lock();

    if (ret != ESP_OK) {
        return ret;
    }
    if (ret == ESP_OK) {
        ret = display_hal_ensure_display_locked();
    }
    if (ret == ESP_OK) {
        ret = display_hal_draw_rect_locked(x, y, width, height, color565);
    }
    display_hal_unlock();
    return ret;
}

esp_err_t display_hal_draw_pixel(int x, int y, uint16_t color565)
{
    return display_hal_fill_rect(x, y, 1, 1, color565);
}

esp_err_t display_hal_set_backlight(bool on)
{
    esp_err_t ret = display_hal_lock();

    if (ret != ESP_OK) {
        return ret;
    }
    if (ret == ESP_OK) {
        ret = display_hal_ensure_display_locked();
    }
    if (ret == ESP_OK) {
        ret = esp_lcd_panel_disp_on_off(s_state.panel, on);
    }
    display_hal_unlock();
    return ret;
}

esp_err_t display_hal_fill_circle(int cx, int cy, int r, uint16_t color565)
{
    if (r <= 0) {
        return ESP_OK;
    }

    uint16_t *span = malloc((size_t)(r * 2 + 1) * sizeof(uint16_t));
    ESP_RETURN_ON_FALSE(span != NULL, ESP_ERR_NO_MEM, TAG, "circle span alloc failed");
    for (int i = 0; i < r * 2 + 1; ++i) {
        span[i] = color565;
    }

    esp_err_t ret = display_hal_lock();
    if (ret != ESP_OK) {
        free(span);
        return ret;
    }
    if (ret == ESP_OK) {
        ret = display_hal_ensure_display_locked();
    }
    for (int dy = -r; dy <= r && ret == ESP_OK; ++dy) {
        int dx = (int)sqrtf((float)(r * r - dy * dy));
        ret = display_hal_draw_bitmap_locked(cx - dx, cy + dy, dx * 2 + 1, 1, span);
    }
    display_hal_unlock();
    free(span);
    return ret;
}

esp_err_t display_hal_draw_circle(int cx, int cy, int r, uint16_t color565)
{
    if (r <= 0) {
        return display_hal_draw_pixel(cx, cy, color565);
    }

    esp_err_t ret = display_hal_lock();
    if (ret != ESP_OK) {
        return ret;
    }
    if (ret == ESP_OK) {
        ret = display_hal_ensure_display_locked();
    }
    if (ret != ESP_OK) {
        display_hal_unlock();
        return ret;
    }

    int x = 0;
    int y = r;
    int d = 1 - r;

    while (x <= y) {
        const int pts[8][2] = {
            {cx + x, cy + y}, {cx - x, cy + y},
            {cx + x, cy - y}, {cx - x, cy - y},
            {cx + y, cy + x}, {cx - y, cy + x},
            {cx + y, cy - x}, {cx - y, cy - x},
        };
        for (int i = 0; i < 8 && ret == ESP_OK; ++i) {
            ret = display_hal_draw_pixel_locked(pts[i][0], pts[i][1], color565);
        }
        if (d < 0) {
            d += 2 * x + 3;
        } else {
            d += 2 * (x - y) + 5;
            y--;
        }
        x++;
    }

    display_hal_unlock();
    return ret;
}

esp_err_t display_hal_draw_arc(int cx, int cy, int radius,
                               float start_deg, float end_deg, uint16_t color565)
{
    if (radius <= 0) {
        return display_hal_draw_pixel(cx, cy, color565);
    }
    if (display_hal_arc_is_full_sweep(start_deg, end_deg)) {
        return display_hal_draw_circle(cx, cy, radius, color565);
    }

    esp_err_t ret = display_hal_lock();
    if (ret != ESP_OK) {
        return ret;
    }
    if (ret == ESP_OK) {
        ret = display_hal_ensure_display_locked();
    }
    if (ret != ESP_OK) {
        display_hal_unlock();
        return ret;
    }

    float start = display_hal_normalize_degrees(start_deg);
    float sweep = display_hal_arc_sweep_degrees(start_deg, end_deg);
    int steps = (int)ceilf(((sweep * DISPLAY_HAL_PI) / 180.0f) * (float)radius);
    if (steps < 8) {
        steps = 8;
    }

    int prev_x = cx + (int)lroundf(cosf(start * DISPLAY_HAL_PI / 180.0f) * (float)radius);
    int prev_y = cy + (int)lroundf(sinf(start * DISPLAY_HAL_PI / 180.0f) * (float)radius);

    for (int i = 1; i <= steps && ret == ESP_OK; ++i) {
        float angle = start + (sweep * (float)i / (float)steps);
        float rad = angle * DISPLAY_HAL_PI / 180.0f;
        int x = cx + (int)lroundf(cosf(rad) * (float)radius);
        int y = cy + (int)lroundf(sinf(rad) * (float)radius);
        ret = display_hal_draw_line_locked(prev_x, prev_y, x, y, color565);
        prev_x = x;
        prev_y = y;
    }

    display_hal_unlock();
    return ret;
}

esp_err_t display_hal_fill_arc(int cx, int cy, int inner_radius, int outer_radius,
                               float start_deg, float end_deg, uint16_t color565)
{
    if (outer_radius < 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (outer_radius == 0) {
        return display_hal_draw_pixel(cx, cy, color565);
    }
    if (inner_radius < 0) {
        inner_radius = 0;
    }
    if (inner_radius > outer_radius) {
        int tmp = inner_radius;
        inner_radius = outer_radius;
        outer_radius = tmp;
    }
    if (display_hal_arc_is_full_sweep(start_deg, end_deg) && inner_radius == 0) {
        return display_hal_fill_circle(cx, cy, outer_radius, color565);
    }

    esp_err_t ret = display_hal_lock();
    if (ret != ESP_OK) {
        return ret;
    }
    if (ret == ESP_OK) {
        ret = display_hal_ensure_display_locked();
    }
    if (ret != ESP_OK) {
        display_hal_unlock();
        return ret;
    }

    int clip_left = 0;
    int clip_top = 0;
    int clip_right = s_state.width;
    int clip_bottom = s_state.height;
    if (!display_hal_get_clip_bounds_locked(&clip_left, &clip_top, &clip_right, &clip_bottom)) {
        display_hal_unlock();
        return ESP_OK;
    }

    int outer_sq = outer_radius * outer_radius;
    int inner_sq = inner_radius * inner_radius;
    int y_start = cy - outer_radius;
    int y_end = cy + outer_radius;
    if (y_start < clip_top) {
        y_start = clip_top;
    }
    if (y_end >= clip_bottom) {
        y_end = clip_bottom - 1;
    }

    for (int y = y_start; y <= y_end && ret == ESP_OK; ++y) {
        int span_start = -1;
        int x_start = cx - outer_radius;
        int x_end = cx + outer_radius;
        if (x_start < clip_left) {
            x_start = clip_left;
        }
        if (x_end >= clip_right) {
            x_end = clip_right - 1;
        }

        for (int x = x_start; x <= x_end; ++x) {
            int dx = x - cx;
            int dy = y - cy;
            int dist_sq = dx * dx + dy * dy;
            bool inside = dist_sq <= outer_sq && dist_sq >= inner_sq;

            if (inside && !display_hal_arc_is_full_sweep(start_deg, end_deg)) {
                inside = display_hal_angle_in_arc(display_hal_point_angle_degrees(dx, dy), start_deg, end_deg);
            }

            if (inside) {
                if (span_start < 0) {
                    span_start = x;
                }
            } else if (span_start >= 0) {
                ret = display_hal_draw_hline_locked(span_start, y, x - span_start, color565);
                span_start = -1;
            }
        }

        if (span_start >= 0 && ret == ESP_OK) {
            ret = display_hal_draw_hline_locked(span_start, y, x_end - span_start + 1, color565);
        }
    }

    display_hal_unlock();
    return ret;
}

esp_err_t display_hal_draw_ellipse(int cx, int cy, int radius_x, int radius_y,
                                   uint16_t color565)
{
    if (radius_x < 0 || radius_y < 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (radius_x == 0 && radius_y == 0) {
        return display_hal_draw_pixel(cx, cy, color565);
    }
    if (radius_x == 0) {
        return display_hal_draw_line(cx, cy - radius_y, cx, cy + radius_y, color565);
    }
    if (radius_y == 0) {
        return display_hal_draw_line(cx - radius_x, cy, cx + radius_x, cy, color565);
    }

    esp_err_t ret = display_hal_lock();
    if (ret != ESP_OK) {
        return ret;
    }
    if (ret == ESP_OK) {
        ret = display_hal_ensure_display_locked();
    }
    if (ret != ESP_OK) {
        display_hal_unlock();
        return ret;
    }

    float perimeter = DISPLAY_HAL_PI *
                      (3.0f * (radius_x + radius_y) -
                       sqrtf((float)((3 * radius_x + radius_y) * (radius_x + 3 * radius_y))));
    int steps = (int)ceilf(perimeter);
    if (steps < 16) {
        steps = 16;
    }

    int prev_x = cx + radius_x;
    int prev_y = cy;
    for (int i = 1; i <= steps && ret == ESP_OK; ++i) {
        float angle = ((float)i / (float)steps) * 2.0f * DISPLAY_HAL_PI;
        int x = cx + (int)lroundf(cosf(angle) * (float)radius_x);
        int y = cy + (int)lroundf(sinf(angle) * (float)radius_y);
        ret = display_hal_draw_line_locked(prev_x, prev_y, x, y, color565);
        prev_x = x;
        prev_y = y;
    }

    display_hal_unlock();
    return ret;
}

esp_err_t display_hal_fill_ellipse(int cx, int cy, int radius_x, int radius_y,
                                   uint16_t color565)
{
    if (radius_x < 0 || radius_y < 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (radius_x == 0 && radius_y == 0) {
        return display_hal_draw_pixel(cx, cy, color565);
    }
    if (radius_x == 0) {
        return display_hal_fill_rect(cx, cy - radius_y, 1, radius_y * 2 + 1, color565);
    }
    if (radius_y == 0) {
        return display_hal_fill_rect(cx - radius_x, cy, radius_x * 2 + 1, 1, color565);
    }

    esp_err_t ret = display_hal_lock();
    if (ret != ESP_OK) {
        return ret;
    }
    if (ret == ESP_OK) {
        ret = display_hal_ensure_display_locked();
    }
    if (ret != ESP_OK) {
        display_hal_unlock();
        return ret;
    }

    int clip_left = 0;
    int clip_top = 0;
    int clip_right = s_state.width;
    int clip_bottom = s_state.height;
    if (!display_hal_get_clip_bounds_locked(&clip_left, &clip_top, &clip_right, &clip_bottom)) {
        display_hal_unlock();
        return ESP_OK;
    }

    int y_start = cy - radius_y;
    int y_end = cy + radius_y;
    if (y_start < clip_top) {
        y_start = clip_top;
    }
    if (y_end >= clip_bottom) {
        y_end = clip_bottom - 1;
    }

    for (int y = y_start; y <= y_end && ret == ESP_OK; ++y) {
        float dy = (float)(y - cy) / (float)radius_y;
        float dx = (float)radius_x * sqrtf(fmaxf(0.0f, 1.0f - (dy * dy)));
        int span = (int)lroundf(dx);
        int x0 = cx - span;
        int x1 = cx + span;

        if (x0 < clip_left) {
            x0 = clip_left;
        }
        if (x1 >= clip_right) {
            x1 = clip_right - 1;
        }
        if (x1 >= x0) {
            ret = display_hal_draw_hline_locked(x0, y, x1 - x0 + 1, color565);
        }
    }

    display_hal_unlock();
    return ret;
}

esp_err_t display_hal_draw_round_rect(int x, int y, int width, int height,
                                      int radius, uint16_t color565)
{
    if (width <= 0 || height <= 0) {
        return ESP_OK;
    }

    int max_radius = (width < height ? width : height) / 2;
    if (radius <= 0 || max_radius <= 0) {
        return display_hal_draw_rect(x, y, width, height, color565);
    }
    if (radius > max_radius) {
        radius = max_radius;
    }

    esp_err_t ret = display_hal_lock();
    if (ret != ESP_OK) {
        return ret;
    }
    if (ret == ESP_OK) {
        ret = display_hal_ensure_display_locked();
    }
    if (ret != ESP_OK) {
        display_hal_unlock();
        return ret;
    }

    ret = display_hal_draw_hline_locked(x + radius, y, width - (radius * 2), color565);
    if (ret == ESP_OK) {
        ret = display_hal_draw_hline_locked(x + radius, y + height - 1, width - (radius * 2), color565);
    }
    if (ret == ESP_OK) {
        ret = display_hal_draw_vline_locked(x, y + radius, height - (radius * 2), color565);
    }
    if (ret == ESP_OK) {
        ret = display_hal_draw_vline_locked(x + width - 1, y + radius, height - (radius * 2), color565);
    }

    for (int row = 0; row < radius && ret == ESP_OK; ++row) {
        int off = radius - row - 1;
        int dx = (int)sqrtf((float)(radius * radius - off * off));
        int inset = radius - dx;
        ret = display_hal_draw_pixel_locked(x + inset, y + row, color565);
        if (ret == ESP_OK) {
            ret = display_hal_draw_pixel_locked(x + width - 1 - inset, y + row, color565);
        }
        if (ret == ESP_OK) {
            ret = display_hal_draw_pixel_locked(x + inset, y + height - 1 - row, color565);
        }
        if (ret == ESP_OK) {
            ret = display_hal_draw_pixel_locked(x + width - 1 - inset, y + height - 1 - row, color565);
        }
    }

    display_hal_unlock();
    return ret;
}

esp_err_t display_hal_fill_round_rect(int x, int y, int width, int height,
                                      int radius, uint16_t color565)
{
    if (width <= 0 || height <= 0) {
        return ESP_OK;
    }

    int max_radius = (width < height ? width : height) / 2;
    if (radius <= 0 || max_radius <= 0) {
        return display_hal_fill_rect(x, y, width, height, color565);
    }
    if (radius > max_radius) {
        radius = max_radius;
    }

    esp_err_t ret = display_hal_lock();
    if (ret != ESP_OK) {
        return ret;
    }
    if (ret == ESP_OK) {
        ret = display_hal_ensure_display_locked();
    }
    if (ret != ESP_OK) {
        display_hal_unlock();
        return ret;
    }

    ret = display_hal_fill_rect_locked(x + radius, y, width - (radius * 2), height, color565);
    for (int row = 0; row < radius && ret == ESP_OK; ++row) {
        int off = radius - row - 1;
        int dx = (int)sqrtf((float)(radius * radius - off * off));
        int inset = radius - dx;
        int span_w = width - (inset * 2);
        ret = display_hal_draw_hline_locked(x + inset, y + row, span_w, color565);
        if (ret == ESP_OK) {
            ret = display_hal_draw_hline_locked(x + inset, y + height - 1 - row, span_w, color565);
        }
    }

    display_hal_unlock();
    return ret;
}

esp_err_t display_hal_draw_triangle(int x1, int y1, int x2, int y2,
                                    int x3, int y3, uint16_t color565)
{
    esp_err_t ret = display_hal_lock();
    if (ret != ESP_OK) {
        return ret;
    }
    if (ret == ESP_OK) {
        ret = display_hal_ensure_display_locked();
    }
    if (ret == ESP_OK) {
        ret = display_hal_draw_line_locked(x1, y1, x2, y2, color565);
    }
    if (ret == ESP_OK) {
        ret = display_hal_draw_line_locked(x2, y2, x3, y3, color565);
    }
    if (ret == ESP_OK) {
        ret = display_hal_draw_line_locked(x3, y3, x1, y1, color565);
    }
    display_hal_unlock();
    return ret;
}

esp_err_t display_hal_fill_triangle(int x1, int y1, int x2, int y2,
                                    int x3, int y3, uint16_t color565)
{
    display_hal_sort_vertices_by_y(&x1, &y1, &x2, &y2, &x3, &y3);

    esp_err_t ret = display_hal_lock();
    if (ret != ESP_OK) {
        return ret;
    }
    if (ret == ESP_OK) {
        ret = display_hal_ensure_display_locked();
    }
    if (ret != ESP_OK) {
        display_hal_unlock();
        return ret;
    }

    if (y1 == y3) {
        int min_x = x1;
        int max_x = x1;
        if (x2 < min_x) {
            min_x = x2;
        }
        if (x3 < min_x) {
            min_x = x3;
        }
        if (x2 > max_x) {
            max_x = x2;
        }
        if (x3 > max_x) {
            max_x = x3;
        }
        ret = display_hal_draw_hline_locked(min_x, y1, max_x - min_x + 1, color565);
        display_hal_unlock();
        return ret;
    }

    for (int y = y1; y <= y3 && ret == ESP_OK; ++y) {
        float alpha = (y3 == y1) ? 0.0f : (float)(y - y1) / (float)(y3 - y1);
        float beta = 0.0f;
        int ax = x1 + (int)lroundf((x3 - x1) * alpha);
        int bx = 0;

        if (y < y2) {
            beta = (y2 == y1) ? 0.0f : (float)(y - y1) / (float)(y2 - y1);
            bx = x1 + (int)lroundf((x2 - x1) * beta);
        } else {
            beta = (y3 == y2) ? 0.0f : (float)(y - y2) / (float)(y3 - y2);
            bx = x2 + (int)lroundf((x3 - x2) * beta);
        }

        if (ax > bx) {
            int tmp = ax;
            ax = bx;
            bx = tmp;
        }
        ret = display_hal_draw_hline_locked(ax, y, bx - ax + 1, color565);
    }

    display_hal_unlock();
    return ret;
}

esp_err_t display_hal_measure_text(const char *text, uint8_t font_size,
                                   uint16_t *out_width, uint16_t *out_height)
{
    const esp_painter_basic_font_t *font = display_hal_get_font(font_size);

    ESP_RETURN_ON_FALSE(font != NULL, ESP_ERR_NOT_SUPPORTED, TAG, "font size %u unavailable", font_size);
    display_hal_measure_text_raw(text, font, out_width, out_height);
    return ESP_OK;
}

esp_err_t display_hal_draw_text(int x, int y, const char *text, uint8_t font_size,
                                uint16_t text_color565, bool has_bg, uint16_t bg_color565)
{
    const esp_painter_basic_font_t *font = NULL;
    uint16_t text_w = 0;
    uint16_t text_h = 0;
    uint16_t fill_color = 0;
    uint16_t *buffer = NULL;
    size_t buffer_bytes = 0;
    size_t aligned_buffer_bytes = 0;
    esp_err_t ret = display_hal_lock();

    if (ret != ESP_OK) {
        return ret;
    }
    ESP_GOTO_ON_FALSE(text != NULL, ESP_ERR_INVALID_ARG, fail, TAG, "text is NULL");
    if (text[0] == '\0') {
        ret = ESP_OK;
        goto fail;
    }

    ret = display_hal_ensure_display_locked();
    if (ret == ESP_OK) {
        ret = display_hal_ensure_painter_locked();
    }
    if (ret != ESP_OK) {
        goto fail;
    }

    font = display_hal_get_font(font_size);
    ESP_GOTO_ON_FALSE(font != NULL, ESP_ERR_NOT_SUPPORTED, fail, TAG, "font size %u unavailable", font_size);
    display_hal_measure_text_raw(text, font, &text_w, &text_h);
    if (text_w == 0 || text_h == 0) {
        ret = ESP_OK;
        goto fail;
    }

    buffer_bytes = (size_t)text_w * (size_t)text_h * sizeof(uint16_t);
    aligned_buffer_bytes = (buffer_bytes + (DISPLAY_HAL_TEXT_CACHE_ALIGN - 1)) &
                           ~(size_t)(DISPLAY_HAL_TEXT_CACHE_ALIGN - 1);
    buffer = heap_caps_aligned_alloc(DISPLAY_HAL_TEXT_CACHE_ALIGN, aligned_buffer_bytes,
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buffer) {
        buffer = heap_caps_aligned_alloc(DISPLAY_HAL_TEXT_CACHE_ALIGN, aligned_buffer_bytes, MALLOC_CAP_DEFAULT);
    }
    ESP_GOTO_ON_FALSE(buffer != NULL, ESP_ERR_NO_MEM, fail, TAG, "text buffer alloc failed");

    fill_color = has_bg ? bg_color565 : 0x0000;
    for (size_t i = 0; i < aligned_buffer_bytes / sizeof(uint16_t); ++i) {
        buffer[i] = fill_color;
    }

    ret = esp_painter_draw_string_rgb565(s_state.painter, (uint8_t *)buffer, (uint32_t)aligned_buffer_bytes,
                                         0, 0, font, text_color565, text);
    if (ret == ESP_OK) {
        ret = display_hal_draw_bitmap_locked(x, y, (int)text_w, (int)text_h, buffer);
    }

fail:
    heap_caps_free(buffer);
    display_hal_unlock();
    return ret;
}

esp_err_t display_hal_draw_text_aligned(int x, int y, int width, int height,
                                        const char *text, uint8_t font_size,
                                        uint16_t text_color565, bool has_bg, uint16_t bg_color565,
                                        display_hal_text_align_t align,
                                        display_hal_text_valign_t valign)
{
    uint16_t text_w = 0;
    uint16_t text_h = 0;
    ESP_RETURN_ON_ERROR(display_hal_measure_text(text, font_size, &text_w, &text_h), TAG,
                        "measure text failed");

    int draw_x = x;
    int draw_y = y;

    if (width > 0) {
        if (align == DISPLAY_HAL_TEXT_ALIGN_CENTER) {
            draw_x = x + (width - (int)text_w) / 2;
        } else if (align == DISPLAY_HAL_TEXT_ALIGN_RIGHT) {
            draw_x = x + width - (int)text_w;
        }
    }
    if (height > 0) {
        if (valign == DISPLAY_HAL_TEXT_VALIGN_MIDDLE) {
            draw_y = y + (height - (int)text_h) / 2;
        } else if (valign == DISPLAY_HAL_TEXT_VALIGN_BOTTOM) {
            draw_y = y + height - (int)text_h;
        }
    }

    return display_hal_draw_text(draw_x, draw_y, text, font_size, text_color565, has_bg, bg_color565);
}

esp_err_t display_hal_draw_bitmap(int x, int y, int w, int h, const uint16_t *pixels)
{
    esp_err_t ret = display_hal_lock();

    if (ret != ESP_OK) {
        return ret;
    }
    if (ret == ESP_OK) {
        ret = display_hal_ensure_display_locked();
    }
    if (ret == ESP_OK) {
        ret = display_hal_draw_bitmap_locked(x, y, w, h, pixels);
    }
    display_hal_unlock();
    return ret;
}

esp_err_t display_hal_draw_bitmap_crop(int x, int y,
                                       int src_x, int src_y,
                                       int w, int h,
                                       int src_width, int src_height,
                                       const uint16_t *pixels)
{
    esp_err_t ret = display_hal_lock();

    if (ret != ESP_OK) {
        return ret;
    }
    if (ret == ESP_OK) {
        ret = display_hal_ensure_display_locked();
    }
    if (ret == ESP_OK) {
        ret = display_hal_draw_bitmap_crop_locked(x, y, src_x, src_y, w, h, src_width, src_height, pixels);
    }
    display_hal_unlock();
    return ret;
}

esp_err_t display_hal_draw_bitmap_scaled(int x, int y,
                                         const uint16_t *pixels,
                                         int src_width, int src_height,
                                         int scale_w, int scale_h,
                                         int *out_w, int *out_h)
{
    uint16_t *scaled = NULL;
    esp_err_t ret = ESP_OK;

    if (!pixels || src_width <= 0 || src_height <= 0 || scale_w <= 0 || scale_h <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    ret = display_hal_scale_rgb565(pixels, src_width, src_height, scale_w, scale_h, &scaled);
    if (ret == ESP_OK) {
        ret = display_hal_draw_bitmap(x, y, scale_w, scale_h, scaled);
    }
    if (out_w) {
        *out_w = scale_w;
    }
    if (out_h) {
        *out_h = scale_h;
    }
    free(scaled);
    return ret;
}

esp_err_t display_hal_draw_jpeg(int x, int y,
                                const uint8_t *jpeg_data, size_t jpeg_len,
                                int *out_w, int *out_h)
{
#if !CONFIG_ESP_ROM_HAS_JPEG_DECODE
    (void)x;
    (void)y;
    (void)jpeg_data;
    (void)jpeg_len;
    (void)out_w;
    (void)out_h;
    return ESP_ERR_NOT_SUPPORTED;
#else
    uint16_t *pixels = NULL;
    int width = 0;
    int height = 0;
    esp_err_t ret = display_hal_decode_jpeg_rgb565(jpeg_data, jpeg_len, &pixels, &width, &height);

    if (ret != ESP_OK) {
        return ret;
    }
    if (out_w) {
        *out_w = width;
    }
    if (out_h) {
        *out_h = height;
    }
    ret = display_hal_draw_bitmap(x, y, width, height, pixels);
    free(pixels);
    return ret;
#endif
}

esp_err_t display_hal_draw_jpeg_crop(int x, int y,
                                     int src_x, int src_y,
                                     int w, int h,
                                     const uint8_t *jpeg_data, size_t jpeg_len,
                                     int *out_w, int *out_h)
{
#if !CONFIG_ESP_ROM_HAS_JPEG_DECODE
    (void)x;
    (void)y;
    (void)src_x;
    (void)src_y;
    (void)w;
    (void)h;
    (void)jpeg_data;
    (void)jpeg_len;
    (void)out_w;
    (void)out_h;
    return ESP_ERR_NOT_SUPPORTED;
#else
    uint16_t *pixels = NULL;
    int width = 0;
    int height = 0;
    esp_err_t ret = display_hal_decode_jpeg_rgb565(jpeg_data, jpeg_len, &pixels, &width, &height);

    if (ret != ESP_OK) {
        return ret;
    }
    if (out_w) {
        *out_w = width;
    }
    if (out_h) {
        *out_h = height;
    }
    ret = display_hal_draw_bitmap_crop(x, y, src_x, src_y, w, h, width, height, pixels);
    free(pixels);
    return ret;
#endif
}

esp_err_t display_hal_jpeg_get_size(const uint8_t *jpeg_data, size_t jpeg_len,
                                    int *out_w, int *out_h)
{
#if !CONFIG_ESP_ROM_HAS_JPEG_DECODE
    (void)jpeg_data;
    (void)jpeg_len;
    (void)out_w;
    (void)out_h;
    return ESP_ERR_NOT_SUPPORTED;
#else
    return display_hal_jpeg_get_size_internal(jpeg_data, jpeg_len, out_w, out_h);
#endif
}

esp_err_t display_hal_draw_jpeg_scaled(int x, int y,
                                       const uint8_t *jpeg_data, size_t jpeg_len,
                                       int scale_w, int scale_h,
                                       int *out_w, int *out_h)
{
#if !CONFIG_ESP_ROM_HAS_JPEG_DECODE
    (void)x;
    (void)y;
    (void)jpeg_data;
    (void)jpeg_len;
    (void)scale_w;
    (void)scale_h;
    (void)out_w;
    (void)out_h;
    return ESP_ERR_NOT_SUPPORTED;
#else
    uint16_t *pixels = NULL;
    int width = 0;
    int height = 0;
    esp_err_t ret = ESP_OK;

    if (!jpeg_data || jpeg_len == 0 || scale_w <= 0 || scale_h <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    ret = display_hal_decode_jpeg_rgb565(jpeg_data, jpeg_len, &pixels, &width, &height);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = display_hal_draw_bitmap_scaled(x, y, pixels, width, height, scale_w, scale_h, out_w, out_h);
    free(pixels);
    return ret;
#endif
}
