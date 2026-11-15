// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** What a recording is for (mirrors app_pet_talk_kind_t without the UI dependency). */
typedef enum {
    APP_PET_BRAIN_CHAT = 0,
    APP_PET_BRAIN_NAME,
} app_pet_brain_kind_t;

/** Results. Callbacks run on the brain task; they may block briefly. */
typedef struct {
    void (*heard)(app_pet_brain_kind_t kind, const char *transcript, void *ctx);
    /** Raw model reply, including [mood:x]/[action:y] tags. */
    void (*reply)(const char *raw, void *ctx);
    /** Speech audio (MP3 or WAV); callee takes ownership (free()). */
    void (*voice)(uint8_t *audio, size_t len, void *ctx);
    /** Request finished without voice (TTS off or failed). */
    void (*done)(void *ctx);
    /** Plain sentence for the owner explaining a failure. */
    void (*error)(const char *message, void *ctx);
    /** Required: true once the network can reach the internet. Requests made
     * before that are refused (the TCP/IP stack may not even be running). */
    bool (*online)(void *ctx);
    void *ctx;
} app_pet_brain_cb_t;

/** Start the Caplora client task (once). Reads app_pet_settings per request. */
esp_err_t app_pet_brain_start(const app_pet_brain_cb_t *cb);

/** Transcribe pcm (16 kHz mono, owned; freed by the brain) and, for CHAT,
 * ask the model and speak the answer. Non-blocking. */
void app_pet_brain_listen(app_pet_brain_kind_t kind, int16_t *pcm, size_t samples);

/** Speak a plain line aloud (no chat). Non-blocking; dropped when busy. */
void app_pet_brain_say(const char *text);

/** Forget the conversation (e.g. after the animal or name changes). */
void app_pet_brain_forget(void);

#ifdef __cplusplus
}
#endif
