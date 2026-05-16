/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CLAW_EVENT_SESSION_POLICY_CHAT = 0,
    CLAW_EVENT_SESSION_POLICY_TRIGGER = 1,
    CLAW_EVENT_SESSION_POLICY_GLOBAL = 2,
    CLAW_EVENT_SESSION_POLICY_EPHEMERAL = 3,
    CLAW_EVENT_SESSION_POLICY_NOSAVE = 4,
} claw_event_session_policy_t;

typedef struct {
    char event_id[48];
    char source_cap[32];
    char event_type[32];
    char source_channel[16];
    char target_channel[16];
    char source_endpoint[64];
    char target_endpoint[96];
    char chat_id[96];
    char sender_id[96];
    char message_id[96];
    char correlation_id[96];
    char content_type[24];
    int64_t timestamp_ms;
    claw_event_session_policy_t session_policy;
    char *text;
    char *payload_json;
} claw_event_t;

esp_err_t claw_event_clone(const claw_event_t *src, claw_event_t *dst);
void claw_event_free(claw_event_t *event);
size_t claw_event_build_session_id(const claw_event_t *event, char *buf, size_t buf_size);
const char *claw_event_session_policy_to_string(claw_event_session_policy_t policy);

/* Compatibility aliases for the former event helper names. */
static inline void claw_event_router_free_event(claw_event_t *event)
{
    claw_event_free(event);
}

static inline size_t claw_event_router_build_session_id(const claw_event_t *event,
                                                        char *buf,
                                                        size_t buf_size)
{
    return claw_event_build_session_id(event, buf, buf_size);
}

static inline const char *claw_event_router_session_policy_to_string(claw_event_session_policy_t policy)
{
    return claw_event_session_policy_to_string(policy);
}

#ifdef __cplusplus
}
#endif
