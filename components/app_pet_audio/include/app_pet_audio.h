// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "pet_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Microphone capture format delivered to the recording callback. */
#define APP_PET_AUDIO_REC_RATE 16000U
#define APP_PET_AUDIO_REC_MAX_S 15U

typedef enum {
    APP_PET_AUDIO_CLIP_STARTED = 0,
    APP_PET_AUDIO_CLIP_LEVEL,     /**< level = loudness 0..1, ~20 times a second. */
    APP_PET_AUDIO_CLIP_DONE,      /**< Finished, failed or interrupted. */
    APP_PET_AUDIO_MUSIC_ON,
    APP_PET_AUDIO_MUSIC_OFF,      /**< Stopped or failed. */
} app_pet_audio_event_t;

/** Runs on audio tasks; must not block. */
typedef void (*app_pet_audio_event_cb_t)(app_pet_audio_event_t event, float level, void *ctx);

/** Finished recording: mono 16-bit PCM at APP_PET_AUDIO_REC_RATE. The callee
 * owns pcm and frees it with free(). Runs on the audio task; must not block. */
typedef void (*app_pet_audio_rec_cb_t)(int16_t *pcm, size_t samples, void *ctx);

typedef struct {
    uint8_t volume;                    /**< 0..100 */
    app_pet_audio_event_cb_t event_cb;
    app_pet_audio_rec_cb_t rec_cb;
    void *ctx;
} app_pet_audio_config_t;

/** Initialise speaker and microphone and start the audio task (once). */
esp_err_t app_pet_audio_start(const app_pet_audio_config_t *config);

/* Requests below are queued, non-blocking and callable from any task.
 * They run in order; a full queue drops the request (logged). */

/** Synthesize and play a short effect (skipped while music plays). */
void app_pet_audio_effect(pet_animal_t animal, pet_sound_t sound);
/** Play an MP3 or WAV clip (a spoken reply). Takes ownership of data
 * (freed with free()), also on failure. Interrupts music. */
void app_pet_audio_clip(uint8_t *data, size_t len);
/** Stream an internet radio URL, or stop when url is NULL. */
void app_pet_audio_music(const char *url);
void app_pet_audio_record_start(void);
void app_pet_audio_record_stop(void);
void app_pet_audio_set_volume(uint8_t volume);

#ifdef __cplusplus
}
#endif
