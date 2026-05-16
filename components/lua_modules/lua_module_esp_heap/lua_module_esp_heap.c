/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lua_module_esp_heap.h"

#include <stdlib.h>

#include "cap_lua.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lauxlib.h"

#if (configUSE_TRACE_FACILITY == 1)
static const char *lua_module_esp_heap_task_state_name(eTaskState state)
{
    switch (state) {
    case eRunning:
        return "running";
    case eReady:
        return "ready";
    case eBlocked:
        return "blocked";
    case eSuspended:
        return "suspended";
    case eDeleted:
        return "deleted";
    case eInvalid:
    default:
        return "invalid";
    }
}
#endif

static void lua_module_esp_heap_push_caps_constants(lua_State *L)
{
    lua_pushinteger(L, MALLOC_CAP_DEFAULT);
    lua_setfield(L, -2, "DEFAULT");
    lua_pushinteger(L, MALLOC_CAP_INTERNAL);
    lua_setfield(L, -2, "INTERNAL");
    lua_pushinteger(L, MALLOC_CAP_SPIRAM);
    lua_setfield(L, -2, "SPIRAM");
    lua_pushinteger(L, MALLOC_CAP_DMA);
    lua_setfield(L, -2, "DMA");
    lua_pushinteger(L, MALLOC_CAP_8BIT);
    lua_setfield(L, -2, "BIT8");
    lua_pushinteger(L, MALLOC_CAP_32BIT);
    lua_setfield(L, -2, "BIT32");
    lua_pushinteger(L, MALLOC_CAP_EXEC);
    lua_setfield(L, -2, "EXEC");
    lua_pushinteger(L, MALLOC_CAP_IRAM_8BIT);
    lua_setfield(L, -2, "IRAM_8BIT");
    lua_pushinteger(L, MALLOC_CAP_RTCRAM);
    lua_setfield(L, -2, "RTCRAM");
    lua_pushinteger(L, MALLOC_CAP_RETENTION);
    lua_setfield(L, -2, "RETENTION");
}

static int lua_module_esp_heap_get_info(lua_State *L)
{
    lua_Integer caps_value = luaL_optinteger(L, 1, MALLOC_CAP_DEFAULT);
    multi_heap_info_t info = {0};
    uint32_t caps = (uint32_t)caps_value;

    heap_caps_get_info(&info, caps);

    lua_newtable(L);
    lua_pushinteger(L, (lua_Integer)caps);
    lua_setfield(L, -2, "caps");
    lua_pushinteger(L, (lua_Integer)heap_caps_get_total_size(caps));
    lua_setfield(L, -2, "total_size");
    lua_pushinteger(L, (lua_Integer)info.total_free_bytes);
    lua_setfield(L, -2, "free_size");
    lua_pushinteger(L, (lua_Integer)info.total_allocated_bytes);
    lua_setfield(L, -2, "allocated_size");
    lua_pushinteger(L, (lua_Integer)info.minimum_free_bytes);
    lua_setfield(L, -2, "minimum_free_size");
    lua_pushinteger(L, (lua_Integer)info.largest_free_block);
    lua_setfield(L, -2, "largest_free_block");
    lua_pushinteger(L, (lua_Integer)info.allocated_blocks);
    lua_setfield(L, -2, "allocated_blocks");
    lua_pushinteger(L, (lua_Integer)info.free_blocks);
    lua_setfield(L, -2, "free_blocks");
    lua_pushinteger(L, (lua_Integer)info.total_blocks);
    lua_setfield(L, -2, "total_blocks");
    return 1;
}

static int lua_module_esp_heap_get_task_watermarks(lua_State *L)
{
#if (configUSE_TRACE_FACILITY == 1)
    UBaseType_t task_count = uxTaskGetNumberOfTasks();
    TaskStatus_t *tasks = NULL;
    UBaseType_t count = 0;

    tasks = calloc((size_t)task_count + 4, sizeof(*tasks));
    if (!tasks) {
        return luaL_error(L, "esp_heap get_task_watermarks: out of memory");
    }

    count = uxTaskGetSystemState(tasks, task_count + 4, NULL);
    lua_newtable(L);
    for (UBaseType_t i = 0; i < count; i++) {
        lua_newtable(L);
        lua_pushstring(L, tasks[i].pcTaskName ? tasks[i].pcTaskName : "(unnamed)");
        lua_setfield(L, -2, "name");
        lua_pushinteger(L, (lua_Integer)tasks[i].xTaskNumber);
        lua_setfield(L, -2, "task_number");
        lua_pushstring(L, lua_module_esp_heap_task_state_name(tasks[i].eCurrentState));
        lua_setfield(L, -2, "state");
        lua_pushinteger(L, (lua_Integer)tasks[i].uxCurrentPriority);
        lua_setfield(L, -2, "current_priority");
        lua_pushinteger(L, (lua_Integer)tasks[i].uxBasePriority);
        lua_setfield(L, -2, "base_priority");
        lua_pushinteger(L, (lua_Integer)tasks[i].usStackHighWaterMark);
        lua_setfield(L, -2, "stack_high_water_mark_words");
        lua_pushinteger(L, (lua_Integer)tasks[i].usStackHighWaterMark * (lua_Integer)sizeof(StackType_t));
        lua_setfield(L, -2, "stack_high_water_mark_bytes");
        lua_rawseti(L, -2, (lua_Integer)i + 1);
    }

    free(tasks);
    return 1;
#else
    TaskHandle_t task = xTaskGetCurrentTaskHandle();
    UBaseType_t words = uxTaskGetStackHighWaterMark(task);

    lua_newtable(L);
    lua_newtable(L);
    lua_pushstring(L, pcTaskGetName(task));
    lua_setfield(L, -2, "name");
    lua_pushinteger(L, (lua_Integer)uxTaskPriorityGet(task));
    lua_setfield(L, -2, "current_priority");
    lua_pushinteger(L, (lua_Integer)uxTaskPriorityGet(task));
    lua_setfield(L, -2, "base_priority");
    lua_pushstring(L, "running");
    lua_setfield(L, -2, "state");
    lua_pushinteger(L, (lua_Integer)words);
    lua_setfield(L, -2, "stack_high_water_mark_words");
    lua_pushinteger(L, (lua_Integer)words * (lua_Integer)sizeof(StackType_t));
    lua_setfield(L, -2, "stack_high_water_mark_bytes");
    lua_rawseti(L, -2, 1);
    lua_pushstring(L, "configUSE_TRACE_FACILITY=0, only current task watermark is available");
    lua_setfield(L, -2, "_warning");
    return 1;
#endif
}

static int lua_module_esp_heap_get_current_task(lua_State *L)
{
    TaskHandle_t task = xTaskGetCurrentTaskHandle();
    UBaseType_t words = uxTaskGetStackHighWaterMark(task);

    lua_newtable(L);
    lua_pushstring(L, pcTaskGetName(task));
    lua_setfield(L, -2, "name");
    lua_pushinteger(L, (lua_Integer)words);
    lua_setfield(L, -2, "stack_high_water_mark_words");
    lua_pushinteger(L, (lua_Integer)words * (lua_Integer)sizeof(StackType_t));
    lua_setfield(L, -2, "stack_high_water_mark_bytes");
    return 1;
}

int luaopen_esp_heap(lua_State *L)
{
    static const luaL_Reg funcs[] = {
        {"get_info", lua_module_esp_heap_get_info},
        {"get_task_watermarks", lua_module_esp_heap_get_task_watermarks},
        {"get_current_task", lua_module_esp_heap_get_current_task},
        {NULL, NULL},
    };

    lua_newtable(L);
    luaL_setfuncs(L, funcs, 0);
    lua_newtable(L);
    lua_module_esp_heap_push_caps_constants(L);
    lua_setfield(L, -2, "caps");
    return 1;
}

esp_err_t lua_module_esp_heap_register(void)
{
    return cap_lua_register_module("esp_heap", luaopen_esp_heap);
}
