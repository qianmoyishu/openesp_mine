/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "llm/backends/claw_llm_backend_custom.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    const claw_llm_backend_vtable_t *delegate;
    void *delegate_ctx;
} custom_backend_ctx_t;

typedef struct custom_backend_registration_node {
    claw_llm_custom_backend_registration_t registration;
    struct custom_backend_registration_node *next;
} custom_backend_registration_node_t;

static custom_backend_registration_node_t *s_registrations = NULL;

static const claw_llm_backend_vtable_t *find_custom_backend(const char *id)
{
    custom_backend_registration_node_t *node = s_registrations;

    while (node) {
        if (strcmp(node->registration.id, id) == 0) {
            return node->registration.vtable;
        }
        node = node->next;
    }

    return NULL;
}

esp_err_t claw_llm_register_custom_backend(const claw_llm_custom_backend_registration_t *registration)
{
    custom_backend_registration_node_t *node;

    if (!registration || !registration->id || !registration->vtable) {
        return ESP_ERR_INVALID_ARG;
    }
    if (find_custom_backend(registration->id)) {
        return ESP_ERR_INVALID_STATE;
    }

    node = calloc(1, sizeof(*node));
    if (!node) {
        return ESP_ERR_NO_MEM;
    }

    node->registration = *registration;
    node->next = s_registrations;
    s_registrations = node;
    return ESP_OK;
}

static esp_err_t custom_backend_init(const claw_llm_runtime_config_t *config,
                                     const claw_llm_model_profile_t *profile,
                                     void **out_backend_ctx,
                                     char **out_error_message)
{
    custom_backend_ctx_t *ctx;
    const claw_llm_backend_vtable_t *delegate;
    esp_err_t err;

    if (!config || !config->profile || !out_backend_ctx || !out_error_message) {
        return ESP_ERR_INVALID_ARG;
    }

    delegate = find_custom_backend(config->profile);
    if (!delegate) {
        *out_error_message = strdup("No custom backend is registered for the selected profile");
        return ESP_ERR_NOT_FOUND;
    }

    ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        *out_error_message = strdup("Out of memory allocating custom backend");
        return ESP_ERR_NO_MEM;
    }

    ctx->delegate = delegate;
    err = delegate->init(config, profile, &ctx->delegate_ctx, out_error_message);
    if (err != ESP_OK) {
        free(ctx);
        return err;
    }

    *out_backend_ctx = ctx;
    return ESP_OK;
}

static esp_err_t custom_backend_chat(void *backend_ctx,
                                     const claw_llm_model_profile_t *profile,
                                     const claw_llm_chat_request_t *request,
                                     claw_llm_response_t *out_response,
                                     char **out_error_message)
{
    custom_backend_ctx_t *ctx = (custom_backend_ctx_t *)backend_ctx;

    if (!ctx || !ctx->delegate || !ctx->delegate->chat) {
        return ESP_ERR_INVALID_STATE;
    }

    return ctx->delegate->chat(ctx->delegate_ctx, profile, request, out_response, out_error_message);
}

static esp_err_t custom_backend_infer_media(void *backend_ctx,
                                            const claw_llm_model_profile_t *profile,
                                            const claw_llm_media_request_t *request,
                                            char **out_text,
                                            char **out_error_message)
{
    custom_backend_ctx_t *ctx = (custom_backend_ctx_t *)backend_ctx;

    if (!ctx || !ctx->delegate || !ctx->delegate->infer_media) {
        return ESP_ERR_INVALID_STATE;
    }

    return ctx->delegate->infer_media(ctx->delegate_ctx, profile, request, out_text, out_error_message);
}

static void custom_backend_deinit(void *backend_ctx)
{
    custom_backend_ctx_t *ctx = (custom_backend_ctx_t *)backend_ctx;

    if (!ctx) {
        return;
    }
    if (ctx->delegate && ctx->delegate->deinit) {
        ctx->delegate->deinit(ctx->delegate_ctx);
    }
    free(ctx);
}

const claw_llm_backend_vtable_t *claw_llm_backend_custom_vtable(void)
{
    static const claw_llm_backend_vtable_t vtable = {
        .id = "custom",
        .init = custom_backend_init,
        .chat = custom_backend_chat,
        .infer_media = custom_backend_infer_media,
        .deinit = custom_backend_deinit,
    };

    return &vtable;
}
