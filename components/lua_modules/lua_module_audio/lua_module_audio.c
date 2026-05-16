/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lua_module_audio.h"

#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_dsp.h"
#include "esp_codec_dev.h"
#include "esp_err.h"
#include "esp_log.h"
#include "lauxlib.h"
#include "cap_lua.h"

static const char *TAG = "lua_audio";

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* --------------------------------------------------------------------------
 * Audio constants
 * -------------------------------------------------------------------------- */
#define AUDIO_CHUNK_BYTES      512
#define AUDIO_DEFAULT_VOL      80
#define AUDIO_DEFAULT_GAIN_DB  10.0f
#define AUDIO_HANDLE_METATABLE "lua_audio_handle"
#define AUDIO_SPECTRUM_MIN_FFT  64
#define AUDIO_SPECTRUM_MAX_FFT  4096
#define AUDIO_SPECTRUM_DEF_FFT  512
#define AUDIO_SPECTRUM_DEF_BANDS 16
#define AUDIO_SPECTRUM_DB_MIN   (-90.0f)
#define AUDIO_SPECTRUM_DB_MAX   (-20.0f)

typedef enum {
    AUDIO_HANDLE_INPUT = 0,
    AUDIO_HANDLE_OUTPUT,
} audio_handle_kind_t;

typedef struct audio_lua_handle_t {
    audio_handle_kind_t    kind;
    esp_codec_dev_handle_t codec_dev;
    uint32_t               sample_rate;
    uint8_t                channels;
    uint8_t                bits_per_sample;
    uint8_t                bytes_per_sample;
    int                    out_vol;
    float                  in_gain_db;
} audio_lua_handle_t;

/* --------------------------------------------------------------------------
 * WAV helpers
 * -------------------------------------------------------------------------- */
typedef struct {
    uint16_t audio_format;
    uint16_t num_channels;
    uint32_t sample_rate;
    uint16_t bits_per_sample;
    uint32_t data_offset;
    uint32_t data_size;
} audio_wav_info_t;

static bool s_fft_ready = false;
static int s_fft_init_size = 0;

static void wav_write_u16(uint8_t *dst, uint16_t v)
{
    dst[0] = (uint8_t)(v & 0xFF);
    dst[1] = (uint8_t)((v >> 8) & 0xFF);
}

static void wav_write_u32(uint8_t *dst, uint32_t v)
{
    dst[0] = (uint8_t)(v & 0xFF);
    dst[1] = (uint8_t)((v >> 8) & 0xFF);
    dst[2] = (uint8_t)((v >> 16) & 0xFF);
    dst[3] = (uint8_t)((v >> 24) & 0xFF);
}

static uint16_t wav_read_u16(const uint8_t *src)
{
    return (uint16_t)src[0] | ((uint16_t)src[1] << 8);
}

static uint32_t wav_read_u32(const uint8_t *src)
{
    return (uint32_t)src[0] | ((uint32_t)src[1] << 8) |
           ((uint32_t)src[2] << 16) | ((uint32_t)src[3] << 24);
}

static void wav_build_header(uint8_t *hdr, uint32_t data_size,
                             uint32_t sample_rate, uint16_t channels,
                             uint16_t bytes_per_sample, uint16_t bits_per_sample)
{
    uint32_t byte_rate = sample_rate * channels * bytes_per_sample;
    uint16_t block_align = channels * bytes_per_sample;

    memcpy(hdr + 0,  "RIFF", 4);
    wav_write_u32(hdr + 4,  data_size + 36);
    memcpy(hdr + 8,  "WAVE", 4);
    memcpy(hdr + 12, "fmt ", 4);
    wav_write_u32(hdr + 16, 16);
    wav_write_u16(hdr + 20, 1);
    wav_write_u16(hdr + 22, channels);
    wav_write_u32(hdr + 24, sample_rate);
    wav_write_u32(hdr + 28, byte_rate);
    wav_write_u16(hdr + 32, block_align);
    wav_write_u16(hdr + 34, bits_per_sample);
    memcpy(hdr + 36, "data", 4);
    wav_write_u32(hdr + 40, data_size);
}

static esp_err_t wav_parse(FILE *f, audio_wav_info_t *info)
{
    uint8_t hdr[12];

    memset(info, 0, sizeof(*info));
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) {
        return ESP_FAIL;
    }
    if (memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    while (1) {
        uint8_t chunk_hdr[8];
        uint32_t chunk_size;

        if (fread(chunk_hdr, 1, sizeof(chunk_hdr), f) != sizeof(chunk_hdr)) {
            break;
        }
        chunk_size = wav_read_u32(chunk_hdr + 4);

        if (memcmp(chunk_hdr, "fmt ", 4) == 0) {
            uint8_t fmt[16];
            if (chunk_size < sizeof(fmt) || fread(fmt, 1, sizeof(fmt), f) != sizeof(fmt)) {
                return ESP_FAIL;
            }
            info->audio_format = wav_read_u16(fmt + 0);
            info->num_channels = wav_read_u16(fmt + 2);
            info->sample_rate = wav_read_u32(fmt + 4);
            info->bits_per_sample = wav_read_u16(fmt + 14);
            if (chunk_size > sizeof(fmt)) {
                fseek(f, (long)(chunk_size - sizeof(fmt)), SEEK_CUR);
            }
        } else if (memcmp(chunk_hdr, "data", 4) == 0) {
            info->data_offset = (uint32_t)ftell(f);
            info->data_size = chunk_size;
            fseek(f, (long)chunk_size, SEEK_CUR);
        } else {
            fseek(f, (long)chunk_size, SEEK_CUR);
        }

        if (chunk_size & 1U) {
            fseek(f, 1, SEEK_CUR);
        }
    }

    if (info->data_offset == 0 || info->data_size == 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

/* --------------------------------------------------------------------------
 * Helpers
 * -------------------------------------------------------------------------- */
static bool audio_path_valid(const char *path, const char *ext)
{
    size_t ext_len = strlen(ext);
    size_t len;

    if (!path || !path[0] || strstr(path, "..")) {
        return false;
    }
    len = strlen(path);
    return len > ext_len && strcmp(path + len - ext_len, ext) == 0;
}

static void *lua_audio_check_lightuserdata_arg(lua_State *L, int index, const char *name)
{
    void *ptr = lua_touserdata(L, index);

    luaL_argcheck(L, ptr != NULL, index, name);
    return ptr;
}

static uint32_t lua_audio_check_u32_arg(lua_State *L, int index, const char *name)
{
    lua_Integer value = luaL_checkinteger(L, index);

    if (value <= 0 || value > UINT32_MAX) {
        luaL_error(L, "audio %s must be a positive integer", name);
        return 0;
    }
    return (uint32_t)value;
}

static uint8_t lua_audio_check_u8_arg(lua_State *L, int index, const char *name)
{
    lua_Integer value = luaL_checkinteger(L, index);

    if (value <= 0 || value > UINT8_MAX) {
        luaL_error(L, "audio %s must be a positive integer", name);
        return 0;
    }
    return (uint8_t)value;
}

static esp_err_t audio_validate_sample_info(audio_lua_handle_t *handle)
{
    if (handle->sample_rate == 0 || handle->channels == 0 || handle->bits_per_sample == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if ((handle->bits_per_sample % 8) != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    handle->bytes_per_sample = (uint8_t)(handle->bits_per_sample / 8);
    if (handle->bytes_per_sample == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static esp_err_t audio_validate_handle_formats(const audio_lua_handle_t *input,
                                               const audio_lua_handle_t *output)
{
    if (input->sample_rate != output->sample_rate ||
        input->channels != output->channels ||
        input->bits_per_sample != output->bits_per_sample) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static esp_err_t audio_handle_activate(audio_lua_handle_t *handle)
{
    esp_codec_dev_sample_info_t fs = {
        .sample_rate = handle->sample_rate,
        .channel = handle->channels,
        .bits_per_sample = handle->bits_per_sample,
    };
    int ret;

    if (audio_validate_sample_info(handle) != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }

    ret = esp_codec_dev_open(handle->codec_dev, &fs);
    if (ret != ESP_CODEC_DEV_OK) {
        return ESP_FAIL;
    }
    if (handle->kind == AUDIO_HANDLE_OUTPUT) {
        ret = esp_codec_dev_set_out_vol(handle->codec_dev, handle->out_vol);
    } else {
        ret = esp_codec_dev_set_in_gain(handle->codec_dev, handle->in_gain_db);
    }
    if (ret != ESP_CODEC_DEV_OK && ret != ESP_CODEC_DEV_NOT_SUPPORT) {
        esp_codec_dev_close(handle->codec_dev);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static bool audio_is_power_of_two(uint32_t value)
{
    return value != 0 && (value & (value - 1U)) == 0;
}

static esp_err_t audio_fft_ensure_ready(int fft_size)
{
    if (s_fft_ready && s_fft_init_size >= fft_size) {
        return ESP_OK;
    }

    if (s_fft_ready) {
        dsps_fft2r_deinit_fc32();
        s_fft_ready = false;
        s_fft_init_size = 0;
    }

    esp_err_t err = dsps_fft2r_init_fc32(NULL, fft_size);
    if (err == ESP_OK) {
        s_fft_ready = true;
        s_fft_init_size = fft_size;
    }
    return err;
}

static uint8_t audio_spectrum_db_to_level(float db)
{
    if (db <= AUDIO_SPECTRUM_DB_MIN) {
        return 0;
    }
    if (db >= AUDIO_SPECTRUM_DB_MAX) {
        return 255;
    }

    float scaled = (db - AUDIO_SPECTRUM_DB_MIN) * 255.0f /
                   (AUDIO_SPECTRUM_DB_MAX - AUDIO_SPECTRUM_DB_MIN);
    if (scaled < 0.0f) {
        scaled = 0.0f;
    } else if (scaled > 255.0f) {
        scaled = 255.0f;
    }
    return (uint8_t)(scaled + 0.5f);
}

static uint32_t audio_spectrum_log_bin_start(uint32_t band, uint32_t band_count, uint32_t half_bins)
{
    double min_bin = 1.0;
    double max_bin = (double)(half_bins - 1);
    double ratio;
    double pos;

    if (half_bins <= 1 || band_count == 0) {
        return 1;
    }
    if (band == 0) {
        return 1;
    }

    ratio = pow(max_bin / min_bin, 1.0 / (double)band_count);
    pos = min_bin * pow(ratio, (double)band);
    if (pos < 1.0) {
        pos = 1.0;
    }
    if (pos > max_bin) {
        pos = max_bin;
    }
    return (uint32_t)pos;
}

static audio_lua_handle_t *lua_audio_check_any_handle(lua_State *L, int idx, const char *what)
{
    audio_lua_handle_t *handle =
        (audio_lua_handle_t *)luaL_checkudata(L, idx, AUDIO_HANDLE_METATABLE);

    if (handle->codec_dev == NULL) {
        luaL_error(L, "audio %s: invalid or closed handle", what);
        return NULL;
    }
    return handle;
}

static audio_lua_handle_t *lua_audio_check_handle(lua_State *L, int idx, audio_handle_kind_t kind, const char *what)
{
    audio_lua_handle_t *handle = lua_audio_check_any_handle(L, idx, what);

    if (handle->kind != kind) {
        luaL_error(L, "audio %s: wrong handle type", what);
        return NULL;
    }
    return handle;
}

static void lua_audio_push_handle(lua_State *L, const audio_lua_handle_t *handle)
{
    audio_lua_handle_t *ud =
        (audio_lua_handle_t *)lua_newuserdata(L, sizeof(*ud));

    *ud = *handle;
    luaL_getmetatable(L, AUDIO_HANDLE_METATABLE);
    lua_setmetatable(L, -2);
}

static int lua_audio_handle_gc(lua_State *L)
{
    audio_lua_handle_t *handle =
        (audio_lua_handle_t *)luaL_testudata(L, 1, AUDIO_HANDLE_METATABLE);

    if (handle && handle->codec_dev != NULL) {
        esp_codec_dev_close(handle->codec_dev);
        handle->codec_dev = NULL;
    }
    return 0;
}

/* --------------------------------------------------------------------------
 * audio.new_input(codec_dev_handle, sample_rate, channels, bits_per_sample [, gain_db])
 *   -> handle | nil, errmsg
 * -------------------------------------------------------------------------- */
static int lua_audio_new_input(lua_State *L)
{
    audio_lua_handle_t handle = {0};

    handle.kind = AUDIO_HANDLE_INPUT;
    handle.codec_dev = (esp_codec_dev_handle_t)lua_audio_check_lightuserdata_arg(
        L, 1, "audio codec_dev_handle lightuserdata expected");
    handle.sample_rate = lua_audio_check_u32_arg(L, 2, "sample_rate");
    handle.channels = lua_audio_check_u8_arg(L, 3, "channels");
    handle.bits_per_sample = lua_audio_check_u8_arg(L, 4, "bits_per_sample");
    handle.in_gain_db = (float)luaL_optnumber(L, 5, AUDIO_DEFAULT_GAIN_DB);

    if (audio_handle_activate(&handle) != ESP_OK) {
        lua_pushnil(L);
        lua_pushstring(L, "audio new_input: failed to activate input device");
        return 2;
    }
    lua_audio_push_handle(L, &handle);
    return 1;
}

/* --------------------------------------------------------------------------
 * audio.new_output(codec_dev_handle, sample_rate, channels, bits_per_sample [, volume])
 *   -> handle | nil, errmsg
 * -------------------------------------------------------------------------- */
static int lua_audio_new_output(lua_State *L)
{
    audio_lua_handle_t handle = {0};

    handle.kind = AUDIO_HANDLE_OUTPUT;
    handle.codec_dev = (esp_codec_dev_handle_t)lua_audio_check_lightuserdata_arg(
        L, 1, "audio codec_dev_handle lightuserdata expected");
    handle.sample_rate = lua_audio_check_u32_arg(L, 2, "sample_rate");
    handle.channels = lua_audio_check_u8_arg(L, 3, "channels");
    handle.bits_per_sample = lua_audio_check_u8_arg(L, 4, "bits_per_sample");
    handle.out_vol = (int)luaL_optinteger(L, 5, AUDIO_DEFAULT_VOL);

    if (handle.out_vol < 0 || handle.out_vol > 100) {
        lua_pushnil(L);
        lua_pushstring(L, "audio new_output: volume must be 0..100");
        return 2;
    }
    if (audio_handle_activate(&handle) != ESP_OK) {
        lua_pushnil(L);
        lua_pushstring(L, "audio new_output: failed to activate output device");
        return 2;
    }
    lua_audio_push_handle(L, &handle);
    return 1;
}

/* --------------------------------------------------------------------------
 * audio.close(handle) -> true | nil, errmsg
 * -------------------------------------------------------------------------- */
static int lua_audio_close(lua_State *L)
{
    audio_lua_handle_t *handle = lua_audio_check_any_handle(L, 1, "close");

    esp_codec_dev_close(handle->codec_dev);
    handle->codec_dev = NULL;

    lua_pushboolean(L, 1);
    return 1;
}

/* --------------------------------------------------------------------------
 * audio.play_wav(output_handle, path) -> nil
 * path must be a .wav file and must not contain "..".
 * -------------------------------------------------------------------------- */
static int lua_audio_play_wav(lua_State *L)
{
    audio_lua_handle_t *dac = lua_audio_check_handle(L, 1, AUDIO_HANDLE_OUTPUT, "play_wav");
    const char *path = luaL_checkstring(L, 2);
    FILE *f = NULL;
    uint8_t *buf = NULL;
    audio_wav_info_t info = {0};

    if (!audio_path_valid(path, ".wav")) {
        return luaL_error(L, "audio play_wav: path must be a .wav file and must not contain '..'");
    }

    f = fopen(path, "rb");
    if (!f) {
        luaL_error(L, "audio play_wav: cannot open %s", path);
        goto cleanup;
    }

    if (wav_parse(f, &info) != ESP_OK) {
        luaL_error(L, "audio play_wav: invalid WAV file");
        goto cleanup;
    }
    if (info.audio_format != 1 ||
        info.num_channels != dac->channels ||
        info.sample_rate != dac->sample_rate ||
        info.bits_per_sample != dac->bits_per_sample) {
        luaL_error(L, "audio play_wav: WAV format does not match output handle configuration");
        goto cleanup;
    }

    buf = malloc(AUDIO_CHUNK_BYTES);
    if (!buf) {
        luaL_error(L, "audio play_wav: out of memory");
        goto cleanup;
    }

    fseek(f, (long)info.data_offset, SEEK_SET);
    uint32_t remaining = info.data_size;
    while (remaining > 0) {
        size_t chunk = remaining < AUDIO_CHUNK_BYTES ? remaining : AUDIO_CHUNK_BYTES;
        size_t read = fread(buf, 1, chunk, f);
        if (read == 0) {
            luaL_error(L, "audio play_wav: truncated data");
            goto cleanup;
        }
        if (esp_codec_dev_write(dac->codec_dev, buf, (int)read) != ESP_CODEC_DEV_OK) {
            luaL_error(L, "audio play_wav: write failed");
            goto cleanup;
        }
        remaining -= (uint32_t)read;
    }

cleanup:
    free(buf);
    if (f) {
        fclose(f);
    }
    return 0;
}

/* --------------------------------------------------------------------------
 * audio.play_tone(output_handle, freq_hz, duration_ms [, volume_pct [, wait_done]]) -> nil
 * -------------------------------------------------------------------------- */
static int lua_audio_play_tone(lua_State *L)
{
    audio_lua_handle_t *dac = lua_audio_check_handle(L, 1, AUDIO_HANDLE_OUTPUT, "play_tone");
    uint32_t freq_hz = lua_audio_check_u32_arg(L, 2, "freq_hz");
    uint32_t duration_ms = lua_audio_check_u32_arg(L, 3, "duration_ms");
    int volume_pct = (int)luaL_optinteger(L, 4, 90);
    bool wait_done = false;
    void *buf = NULL;
    uint32_t total_frames;
    uint32_t frames_written = 0;
    uint32_t chunk_frames;
    float amplitude;
    float phase = 0.0f;
    float phase_step;
    float gain_scale;
    TickType_t start_tick;
    TickType_t target_ticks;

    if (volume_pct < 0 || volume_pct > 100) {
        return luaL_error(L, "audio play_tone: volume_pct must be 0..100");
    }
    if (!lua_isnoneornil(L, 5)) {
        wait_done = lua_toboolean(L, 5);
    }
    /* 32-bit outputs are produced by left-shifting the 16-bit sine into the upper half (MSB-justified). */
    if (dac->bits_per_sample != 16 && dac->bits_per_sample != 32) {
        return luaL_error(L, "audio play_tone: only 16-bit or 32-bit PCM output is supported");
    }
    if (freq_hz >= dac->sample_rate / 2) {
        return luaL_error(L, "audio play_tone: freq_hz must be less than half of sample_rate");
    }

    chunk_frames = AUDIO_CHUNK_BYTES / (dac->channels * dac->bytes_per_sample);
    if (chunk_frames == 0) {
        return luaL_error(L, "audio play_tone: invalid output frame size");
    }

    total_frames = (uint32_t)(((uint64_t)dac->sample_rate * duration_ms) / 1000);
    start_tick = xTaskGetTickCount();
    target_ticks = pdMS_TO_TICKS(duration_ms);
    if (target_ticks == 0 && duration_ms > 0) {
        target_ticks = 1;
    }
    if (volume_pct == 0) {
        gain_scale = 0.0f;
    } else {
        /* Match esp_codec_dev default volume semantics: [1, 100] -> [-49.5 dB, 0 dB]. */
        float gain_db = ((float)volume_pct - 100.0f) * 0.5f;
        gain_scale = powf(10.0f, gain_db / 20.0f);
    }
    amplitude = 32767.0f * gain_scale;
    phase_step = 2.0f * (float)M_PI * (float)freq_hz / (float)dac->sample_rate;

    buf = malloc(chunk_frames * dac->channels * dac->bytes_per_sample);
    if (!buf) {
        return luaL_error(L, "audio play_tone: out of memory");
    }

    /* Generate one tone chunk at a time to avoid a large temporary buffer. */
    while (frames_written < total_frames) {
        uint32_t frames_this_chunk = total_frames - frames_written;
        if (frames_this_chunk > chunk_frames) {
            frames_this_chunk = chunk_frames;
        }

        for (uint32_t i = 0; i < frames_this_chunk; i++) {
            int16_t sample16 = (int16_t)(sinf(phase) * amplitude);
            if (dac->bits_per_sample == 16) {
                int16_t *p = (int16_t *)buf + (size_t)i * dac->channels;
                for (uint8_t ch = 0; ch < dac->channels; ch++) {
                    p[ch] = sample16;
                }
            } else {
                int32_t sample32 = (int32_t)sample16 << 16;
                int32_t *p = (int32_t *)buf + (size_t)i * dac->channels;
                for (uint8_t ch = 0; ch < dac->channels; ch++) {
                    p[ch] = sample32;
                }
            }
            phase += phase_step;
            if (phase >= 2.0f * (float)M_PI) {
                phase -= 2.0f * (float)M_PI;
            }
        }

        int bytes_to_write = (int)(frames_this_chunk * dac->channels * dac->bytes_per_sample);
        if (esp_codec_dev_write(dac->codec_dev, buf, bytes_to_write) != ESP_CODEC_DEV_OK) {
            free(buf);
            return luaL_error(L, "audio play_tone: write failed");
        }
        frames_written += frames_this_chunk;
    }

    if (wait_done) {
        TickType_t elapsed_ticks = xTaskGetTickCount() - start_tick;
        if (elapsed_ticks < target_ticks) {
            vTaskDelay(target_ticks - elapsed_ticks);
        }
    }

    free(buf);
    return 0;
}

/* --------------------------------------------------------------------------
 * audio.record_wav(input_handle, path, duration_ms) -> { path, duration_ms, bytes }
 * path must be a .wav file and must not contain "..".
 * -------------------------------------------------------------------------- */
static int lua_audio_record_wav(lua_State *L)
{
    audio_lua_handle_t *adc = lua_audio_check_handle(L, 1, AUDIO_HANDLE_INPUT, "record_wav");
    const char *path = luaL_checkstring(L, 2);
    uint32_t duration_ms = (uint32_t)luaL_checkinteger(L, 3);
    FILE *f = NULL;
    uint8_t *buf = NULL;
    uint32_t total_bytes = 0;
    uint8_t wav_hdr[44];

    if (!audio_path_valid(path, ".wav")) {
        return luaL_error(L, "audio record_wav: path must be a .wav file and must not contain '..'");
    }
    if (duration_ms == 0) {
        return luaL_error(L, "audio record_wav: duration_ms must be positive");
    }

    buf = malloc(AUDIO_CHUNK_BYTES);
    if (!buf) {
        luaL_error(L, "audio record_wav: out of memory");
        goto cleanup;
    }

    remove(path);
    f = fopen(path, "wb");
    if (!f) {
        luaL_error(L, "audio record_wav: cannot open %s for writing", path);
        goto cleanup;
    }
    memset(wav_hdr, 0, sizeof(wav_hdr));
    if (fwrite(wav_hdr, 1, sizeof(wav_hdr), f) != sizeof(wav_hdr)) {
        luaL_error(L, "audio record_wav: cannot write WAV header placeholder");
        goto cleanup;
    }

    uint32_t total_target_bytes =
        (uint32_t)((uint64_t)adc->sample_rate * duration_ms / 1000) *
        adc->channels * adc->bytes_per_sample;

    while (total_bytes < total_target_bytes) {
        uint32_t chunk = total_target_bytes - total_bytes;
        if (chunk > AUDIO_CHUNK_BYTES) {
            chunk = AUDIO_CHUNK_BYTES;
        }
        if (esp_codec_dev_read(adc->codec_dev, buf, (int)chunk) != ESP_CODEC_DEV_OK) {
            luaL_error(L, "audio record_wav: read failed");
            goto cleanup;
        }
        if (fwrite(buf, 1, chunk, f) != chunk) {
            luaL_error(L, "audio record_wav: write to file failed");
            goto cleanup;
        }
        total_bytes += chunk;
    }

    wav_build_header(wav_hdr, total_bytes, adc->sample_rate, adc->channels,
                     adc->bytes_per_sample, adc->bits_per_sample);
    fseek(f, 0, SEEK_SET);
    if (fwrite(wav_hdr, 1, sizeof(wav_hdr), f) != sizeof(wav_hdr)) {
        luaL_error(L, "audio record_wav: cannot finalise WAV header");
        goto cleanup;
    }
    fclose(f);
    f = NULL;

    free(buf);
    buf = NULL;

    lua_newtable(L);
    lua_pushstring(L, path);
    lua_setfield(L, -2, "path");
    lua_pushinteger(L, duration_ms);
    lua_setfield(L, -2, "duration_ms");
    lua_pushinteger(L, total_bytes);
    lua_setfield(L, -2, "bytes");
    return 1;

cleanup:
    free(buf);
    if (f) {
        fclose(f);
        remove(path);
    }
    return 0;
}

/* --------------------------------------------------------------------------
 * audio.loopback(input_handle, output_handle [, duration_ms]) -> nil
 * -------------------------------------------------------------------------- */
static int lua_audio_loopback(lua_State *L)
{
    audio_lua_handle_t *adc = lua_audio_check_handle(L, 1, AUDIO_HANDLE_INPUT, "loopback");
    audio_lua_handle_t *dac = lua_audio_check_handle(L, 2, AUDIO_HANDLE_OUTPUT, "loopback");
    uint32_t duration_ms = (uint32_t)luaL_optinteger(L, 3, 1000);
    uint8_t *buf = NULL;

    if (audio_validate_handle_formats(adc, dac) != ESP_OK) {
        return luaL_error(L, "audio loopback: input/output handle formats must match");
    }

    buf = malloc(AUDIO_CHUNK_BYTES);
    if (!buf) {
        luaL_error(L, "audio loopback: out of memory");
        goto cleanup;
    }

    uint32_t total_bytes =
        (uint32_t)((uint64_t)adc->sample_rate * duration_ms / 1000) *
        adc->channels * adc->bytes_per_sample;
    uint32_t transferred = 0;

    ESP_LOGI(TAG, "loopback start: %" PRIu32 " ms (%" PRIu32 " bytes)", duration_ms, total_bytes);
    while (transferred < total_bytes) {
        uint32_t chunk = total_bytes - transferred;
        if (chunk > AUDIO_CHUNK_BYTES) {
            chunk = AUDIO_CHUNK_BYTES;
        }

        if (esp_codec_dev_read(adc->codec_dev, buf, (int)chunk) != ESP_CODEC_DEV_OK) {
            luaL_error(L, "audio loopback: input read failed");
            goto cleanup;
        }
        if (esp_codec_dev_write(dac->codec_dev, buf, (int)chunk) != ESP_CODEC_DEV_OK) {
            luaL_error(L, "audio loopback: output write failed");
            goto cleanup;
        }
        transferred += chunk;
    }
    ESP_LOGI(TAG, "loopback done");

cleanup:
    free(buf);
    return 0;
}

/* --------------------------------------------------------------------------
 * audio.set_volume(output_handle, pct) -> nil
 * -------------------------------------------------------------------------- */
static int lua_audio_set_volume(lua_State *L)
{
    audio_lua_handle_t *dac = lua_audio_check_handle(L, 1, AUDIO_HANDLE_OUTPUT, "set_volume");
    int vol = (int)luaL_checkinteger(L, 2);

    if (vol < 0 || vol > 100) {
        return luaL_error(L, "audio set_volume: volume must be 0..100");
    }
    if (esp_codec_dev_set_out_vol(dac->codec_dev, vol) != ESP_CODEC_DEV_OK) {
        return luaL_error(L, "audio set_volume: set failed");
    }
    dac->out_vol = vol;
    return 0;
}

/* --------------------------------------------------------------------------
 * audio.get_volume(output_handle) -> pct
 * -------------------------------------------------------------------------- */
static int lua_audio_get_volume(lua_State *L)
{
    audio_lua_handle_t *dac = lua_audio_check_handle(L, 1, AUDIO_HANDLE_OUTPUT, "get_volume");
    int vol = 0;

    if (esp_codec_dev_get_out_vol(dac->codec_dev, &vol) != ESP_CODEC_DEV_OK) {
        return luaL_error(L, "audio get_volume: get failed");
    }
    lua_pushinteger(L, vol);
    return 1;
}

/* --------------------------------------------------------------------------
 * audio.set_mute(output_handle, bool) -> nil
 * -------------------------------------------------------------------------- */
static int lua_audio_set_mute(lua_State *L)
{
    audio_lua_handle_t *dac = lua_audio_check_handle(L, 1, AUDIO_HANDLE_OUTPUT, "set_mute");
    bool mute = lua_toboolean(L, 2);

    if (esp_codec_dev_set_out_mute(dac->codec_dev, mute) != ESP_CODEC_DEV_OK) {
        return luaL_error(L, "audio set_mute: set failed");
    }
    return 0;
}

/* --------------------------------------------------------------------------
 * audio.set_gain(input_handle, db) -> nil
 * -------------------------------------------------------------------------- */
static int lua_audio_set_gain(lua_State *L)
{
    audio_lua_handle_t *adc = lua_audio_check_handle(L, 1, AUDIO_HANDLE_INPUT, "set_gain");
    float db = (float)luaL_checknumber(L, 2);

    if (esp_codec_dev_set_in_gain(adc->codec_dev, db) != ESP_CODEC_DEV_OK) {
        return luaL_error(L, "audio set_gain: set failed");
    }
    adc->in_gain_db = db;
    return 0;
}

/* --------------------------------------------------------------------------
 * audio.mic_read_level(input_handle [, duration_ms]) -> { rms, peak, duration_ms }
 * -------------------------------------------------------------------------- */
static int lua_audio_mic_read_level(lua_State *L)
{
    audio_lua_handle_t *adc = lua_audio_check_handle(L, 1, AUDIO_HANDLE_INPUT, "mic_read_level");
    uint32_t duration_ms = (uint32_t)luaL_optinteger(L, 2, 100);
    uint8_t *buf = NULL;
    uint32_t total_bytes;
    int64_t sum_sq = 0;
    int32_t peak = 0;
    uint32_t n_samples = 0;

    if (duration_ms == 0) {
        return luaL_error(L, "audio mic_read_level: duration_ms must be positive");
    }
    /* 32-bit inputs are treated as MSB-justified: fold back to int16 range before stats. */
    if (adc->bits_per_sample != 16 && adc->bits_per_sample != 32) {
        return luaL_error(L, "audio mic_read_level: only 16-bit or 32-bit PCM input is supported");
    }

    total_bytes =
        (uint32_t)((uint64_t)adc->sample_rate * duration_ms / 1000) *
        adc->channels * adc->bytes_per_sample;

    buf = malloc(AUDIO_CHUNK_BYTES);
    if (!buf) {
        luaL_error(L, "audio mic_read_level: out of memory");
        goto cleanup;
    }

    uint32_t captured = 0;
    while (captured < total_bytes) {
        uint32_t chunk = total_bytes - captured;
        if (chunk > AUDIO_CHUNK_BYTES) {
            chunk = AUDIO_CHUNK_BYTES;
        }

        if (esp_codec_dev_read(adc->codec_dev, buf, (int)chunk) != ESP_CODEC_DEV_OK) {
            luaL_error(L, "audio mic_read_level: read failed");
            goto cleanup;
        }

        if (adc->bits_per_sample == 16) {
            const int16_t *samples = (const int16_t *)buf;
            uint32_t n = chunk / sizeof(int16_t);
            for (uint32_t i = 0; i < n; i++) {
                int32_t s = samples[i];
                sum_sq += (int64_t)s * s;
                if (s < 0) {
                    s = -s;
                }
                if (s > peak) {
                    peak = s;
                }
            }
            n_samples += n;
        } else {
            const int32_t *samples = (const int32_t *)buf;
            uint32_t n = chunk / sizeof(int32_t);
            for (uint32_t i = 0; i < n; i++) {
                int32_t s = samples[i] >> 16;
                sum_sq += (int64_t)s * s;
                if (s < 0) {
                    s = -s;
                }
                if (s > peak) {
                    peak = s;
                }
            }
            n_samples += n;
        }
        captured += chunk;
    }

cleanup:
    free(buf);
    int32_t rms = (n_samples > 0) ? (int32_t)sqrt((double)sum_sq / n_samples) : 0;

    lua_newtable(L);
    lua_pushinteger(L, rms);
    lua_setfield(L, -2, "rms");
    lua_pushinteger(L, peak);
    lua_setfield(L, -2, "peak");
    lua_pushinteger(L, duration_ms);
    lua_setfield(L, -2, "duration_ms");
    return 1;
}

/* --------------------------------------------------------------------------
 * audio.read_spectrum(input_handle [, fft_size] [, band_count])
 *   -> { bands = {...}, peak_freq_hz, peak_db, rms, fft_size, band_count, sample_rate }
 * -------------------------------------------------------------------------- */
static int lua_audio_read_spectrum(lua_State *L)
{
    audio_lua_handle_t *adc = lua_audio_check_handle(L, 1, AUDIO_HANDLE_INPUT, "read_spectrum");
    uint32_t fft_size = (uint32_t)luaL_optinteger(L, 2, AUDIO_SPECTRUM_DEF_FFT);
    uint32_t band_count = (uint32_t)luaL_optinteger(L, 3, AUDIO_SPECTRUM_DEF_BANDS);
    uint8_t *pcm_buf = NULL;
    float *window = NULL;
    float *fft_buf = NULL;
    esp_err_t err;
    uint32_t total_bytes;
    uint32_t captured = 0;
    int64_t sum_sq = 0;
    uint32_t mono_samples = 0;
    uint32_t bytes_per_frame;
    uint32_t half_bins;
    uint32_t peak_bin = 0;
    float peak_mag = 0.0f;

    /* 32-bit inputs are treated as MSB-justified: fold back to int16 range before FFT. */
    if (adc->bits_per_sample != 16 && adc->bits_per_sample != 32) {
        return luaL_error(L, "audio read_spectrum: only 16-bit or 32-bit PCM input is supported");
    }
    if (fft_size < AUDIO_SPECTRUM_MIN_FFT || fft_size > AUDIO_SPECTRUM_MAX_FFT || !audio_is_power_of_two(fft_size)) {
        return luaL_error(L, "audio read_spectrum: fft_size must be a power of two in range 64..4096");
    }
    half_bins = fft_size / 2;
    if (band_count == 0 || band_count > half_bins) {
        return luaL_error(L, "audio read_spectrum: band_count must be in range 1..fft_size/2");
    }

    err = audio_fft_ensure_ready((int)fft_size);
    if (err != ESP_OK) {
        return luaL_error(L, "audio read_spectrum: FFT init failed: %s", esp_err_to_name(err));
    }

    window = (float *)malloc(sizeof(float) * fft_size);
    fft_buf = (float *)calloc((size_t)fft_size * 2U, sizeof(float));
    bytes_per_frame = (uint32_t)adc->channels * adc->bytes_per_sample;
    total_bytes = fft_size * bytes_per_frame;
    pcm_buf = (uint8_t *)malloc(total_bytes);
    if (!window || !fft_buf || !pcm_buf) {
        free(window);
        free(fft_buf);
        free(pcm_buf);
        return luaL_error(L, "audio read_spectrum: out of memory");
    }

    dsps_wind_hann_f32(window, (int)fft_size);

    while (captured < total_bytes) {
        uint32_t chunk = total_bytes - captured;
        if (chunk > AUDIO_CHUNK_BYTES) {
            chunk = AUDIO_CHUNK_BYTES;
            chunk -= (chunk % bytes_per_frame);
            if (chunk == 0) {
                chunk = bytes_per_frame;
            }
        }
        if (esp_codec_dev_read(adc->codec_dev, pcm_buf + captured, (int)chunk) != ESP_CODEC_DEV_OK) {
            free(window);
            free(fft_buf);
            free(pcm_buf);
            return luaL_error(L, "audio read_spectrum: read failed");
        }
        captured += chunk;
    }

    for (uint32_t i = 0; i < fft_size; i++) {
        int32_t mixed = 0;

        if (adc->bits_per_sample == 16) {
            const int16_t *frame = (const int16_t *)(pcm_buf + i * bytes_per_frame);
            for (uint32_t ch = 0; ch < adc->channels; ch++) {
                mixed += frame[ch];
            }
        } else {
            const int32_t *frame = (const int32_t *)(pcm_buf + i * bytes_per_frame);
            for (uint32_t ch = 0; ch < adc->channels; ch++) {
                mixed += frame[ch] >> 16;
            }
        }
        mixed /= adc->channels;

        sum_sq += (int64_t)mixed * mixed;
        mono_samples++;

        fft_buf[2 * i] = ((float)mixed / 32768.0f) * window[i];
        fft_buf[2 * i + 1] = 0.0f;
    }

    dsps_fft2r_fc32(fft_buf, (int)fft_size);
    dsps_bit_rev_fc32(fft_buf, (int)fft_size);

    lua_newtable(L);
    lua_newtable(L);
    for (uint32_t band = 0; band < band_count; band++) {
        uint32_t start_bin = audio_spectrum_log_bin_start(band, band_count, half_bins);
        uint32_t end_bin = audio_spectrum_log_bin_start(band + 1, band_count, half_bins);
        if (end_bin <= start_bin) {
            end_bin = start_bin + 1;
        }
        if (end_bin > half_bins) {
            end_bin = half_bins;
        }

        float band_peak = 0.0f;
        for (uint32_t bin = start_bin; bin < end_bin; bin++) {
            float real = fft_buf[2 * bin];
            float imag = fft_buf[2 * bin + 1];
            float mag = sqrtf(real * real + imag * imag);
            if (mag > band_peak) {
                band_peak = mag;
            }
            if (mag > peak_mag) {
                peak_mag = mag;
                peak_bin = bin;
            }
        }

        float band_db = 20.0f * log10f((band_peak + 1e-9f) / (float)fft_size);
        lua_pushinteger(L, (lua_Integer)audio_spectrum_db_to_level(band_db));
        lua_rawseti(L, -2, (lua_Integer)band + 1);
    }
    lua_setfield(L, -2, "bands");

    float peak_db = 20.0f * log10f((peak_mag + 1e-9f) / (float)fft_size);
    int32_t rms = (mono_samples > 0) ? (int32_t)sqrt((double)sum_sq / mono_samples) : 0;

    lua_pushnumber(L, ((lua_Number)peak_bin * (lua_Number)adc->sample_rate) / (lua_Number)fft_size);
    lua_setfield(L, -2, "peak_freq_hz");
    lua_pushnumber(L, peak_db);
    lua_setfield(L, -2, "peak_db");
    lua_pushinteger(L, rms);
    lua_setfield(L, -2, "rms");
    lua_pushinteger(L, (lua_Integer)fft_size);
    lua_setfield(L, -2, "fft_size");
    lua_pushinteger(L, (lua_Integer)band_count);
    lua_setfield(L, -2, "band_count");
    lua_pushinteger(L, (lua_Integer)adc->sample_rate);
    lua_setfield(L, -2, "sample_rate");

    free(window);
    free(fft_buf);
    free(pcm_buf);
    return 1;
}

/* --------------------------------------------------------------------------
 * Module registration
 * -------------------------------------------------------------------------- */
int luaopen_audio(lua_State *L)
{
    static const luaL_Reg funcs[] = {
        {"new_input",      lua_audio_new_input},
        {"new_output",     lua_audio_new_output},
        {"close",          lua_audio_close},
        {"play_tone",      lua_audio_play_tone},
        {"play_wav",       lua_audio_play_wav},
        {"record_wav",     lua_audio_record_wav},
        {"loopback",       lua_audio_loopback},
        {"set_volume",     lua_audio_set_volume},
        {"get_volume",     lua_audio_get_volume},
        {"set_mute",       lua_audio_set_mute},
        {"set_gain",       lua_audio_set_gain},
        {"mic_read_level", lua_audio_mic_read_level},
        {"read_spectrum",  lua_audio_read_spectrum},
        {NULL, NULL},
    };
    if (luaL_newmetatable(L, AUDIO_HANDLE_METATABLE)) {
        lua_pushcfunction(L, lua_audio_handle_gc);
        lua_setfield(L, -2, "__gc");
    }
    lua_pop(L, 1);
    lua_newtable(L);
    luaL_setfuncs(L, funcs, 0);
    return 1;
}

esp_err_t lua_module_audio_register(void)
{
    return cap_lua_register_module("audio", luaopen_audio);
}
