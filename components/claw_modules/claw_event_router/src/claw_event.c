/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "claw_event.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

static const char *TAG = "claw_event";

static const char *claw_event_event_key(const claw_event_t *event)
{
    if (!event) {
        return "";
    }
    return event->message_id[0] ? event->message_id : event->event_id;
}

esp_err_t claw_event_clone(const claw_event_t *src, claw_event_t *dst)
{
    if (!src || !dst) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(dst, 0, sizeof(*dst));
    memcpy(dst, src, sizeof(*dst));
    dst->text = NULL;
    dst->payload_json = NULL;

    if (src->text) {
        dst->text = strdup(src->text);
        if (!dst->text) {
            ESP_LOGE(TAG, "Failed to clone event text");
            return ESP_ERR_NO_MEM;
        }
    }
    if (src->payload_json) {
        dst->payload_json = strdup(src->payload_json);
        if (!dst->payload_json) {
            free(dst->text);
            dst->text = NULL;
            ESP_LOGE(TAG, "Failed to clone event payload");
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

void claw_event_free(claw_event_t *event)
{
    if (!event) {
        return;
    }
    free(event->text);
    free(event->payload_json);
    memset(event, 0, sizeof(*event));
}

size_t claw_event_build_session_id(const claw_event_t *event, char *buf, size_t buf_size)
{
    if (!buf || buf_size == 0 || !event) {
        return 0;
    }

    switch (event->session_policy) {
    case CLAW_EVENT_SESSION_POLICY_CHAT:
        snprintf(buf, buf_size, "%s:%s", event->source_channel, event->chat_id);
        break;
    case CLAW_EVENT_SESSION_POLICY_TRIGGER:
        snprintf(buf,
                 buf_size,
                 "trigger:%s:%s",
                 event->source_cap[0] ? event->source_cap : "system",
                 claw_event_event_key(event));
        break;
    case CLAW_EVENT_SESSION_POLICY_GLOBAL:
        snprintf(buf,
                 buf_size,
                 "global:%s",
                 event->source_cap[0] ? event->source_cap : "router");
        break;
    case CLAW_EVENT_SESSION_POLICY_EPHEMERAL:
        snprintf(buf, buf_size, "ephemeral:%s", event->event_id);
        break;
    case CLAW_EVENT_SESSION_POLICY_NOSAVE:
        buf[0] = '\0';
        return 0;
    default:
        snprintf(buf, buf_size, "%s:%s", event->source_channel, event->chat_id);
        break;
    }

    return strlen(buf);
}

const char *claw_event_session_policy_to_string(claw_event_session_policy_t policy)
{
    switch (policy) {
    case CLAW_EVENT_SESSION_POLICY_CHAT:
        return "chat";
    case CLAW_EVENT_SESSION_POLICY_TRIGGER:
        return "trigger";
    case CLAW_EVENT_SESSION_POLICY_GLOBAL:
        return "global";
    case CLAW_EVENT_SESSION_POLICY_EPHEMERAL:
        return "ephemeral";
    case CLAW_EVENT_SESSION_POLICY_NOSAVE:
        return "nosave";
    default:
        return "chat";
    }
}
