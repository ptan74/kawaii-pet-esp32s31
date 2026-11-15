// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "pet_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Optional second chat provider, used when Caplora is not set or fails. */
typedef enum {
    APP_PET_PROVIDER_NONE = 0,
    APP_PET_PROVIDER_OPENAI,    /**< ChatGPT models, api.openai.com */
    APP_PET_PROVIDER_DEEPSEEK,  /**< api.deepseek.com */
    APP_PET_PROVIDER_GEMINI,    /**< Google Gemini, OpenAI-compatible endpoint */
    APP_PET_PROVIDER_CLAUDE,    /**< Anthropic Messages API */
    APP_PET_PROVIDER_COUNT,
} app_pet_provider_t;

/** Everything the phone setup page can change. Strings are NUL-terminated. */
typedef struct {
    char hotspot_ssid[33];
    char hotspot_password[65];
    char api_key[192];        /**< Caplora cap_... key. */
    char base_url[96];        /**< e.g. https://caploralab.online/api/v1 */
    char chat_model[64];
    char stt_url[160];        /**< Full speech-to-text URL (OpenAI-compatible). */
    char stt_model[64];
    char tts_url[160];        /**< Full text-to-speech URL (OpenAI-compatible). */
    char tts_model[64];
    char tts_voice[48];
    char name[32];            /**< Owner's name. */
    char setup_password[16];  /**< Setup Wi-Fi password, generated once. */
    uint8_t animal;           /**< pet_animal_t */
    uint8_t volume;           /**< 0..100 */
    bool talk_on_its_own;
    /* Appended in blob v2; keep new fields at the end so v1 blobs load as a prefix. */
    char voice_api_key[192];  /**< Key for the STT/TTS service; "" = no voice. */
    /* Appended in blob v3. */
    uint8_t backup_provider;  /**< app_pet_provider_t */
    char backup_key[192];     /**< That provider's own API key. */
    char backup_model[64];    /**< "" uses the provider's default model. */
} app_pet_settings_t;

/** Called after a successful save, on the saving task; must not block. */
typedef void (*app_pet_settings_cb_t)(const app_pet_settings_t *now, void *ctx);

/** Load from NVS (namespace "kawaii_pet"), filling defaults; once per boot. */
esp_err_t app_pet_settings_init(app_pet_settings_cb_t on_change, void *ctx);

/** Copy the current settings. Thread-safe. */
void app_pet_settings_get(app_pet_settings_t *out);

/** Replace all settings, persist, then notify. Thread-safe. */
esp_err_t app_pet_settings_set(const app_pet_settings_t *in);

/** Persist only the owner's name. Thread-safe. */
esp_err_t app_pet_settings_set_name(const char *name);

/** Zero every secret in a copy once it is no longer needed. */
void app_pet_settings_wipe(app_pet_settings_t *copy);

/** True when a Caplora key is configured. */
bool app_pet_settings_has_key(const app_pet_settings_t *s);

/** True when a backup provider and its key are configured. */
bool app_pet_settings_has_backup(const app_pet_settings_t *s);

/** True when either chat route is usable. */
bool app_pet_settings_can_chat(const app_pet_settings_t *s);


#ifdef __cplusplus
}
#endif
