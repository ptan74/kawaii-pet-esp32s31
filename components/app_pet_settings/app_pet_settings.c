// SPDX-License-Identifier: Apache-2.0
#include "app_pet_settings.h"

#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

static const char *TAG = "pet_settings";

#define NAMESPACE "kawaii_pet"
#define BLOB_KEY "settings"
#define BLOB_VERSION 3u

/* Chat goes to Caplora's documented OpenAI-compatible API. Caplora keys carry
 * no speech models, so voice defaults to the Caplora Companion app key (same
 * speech and transcription API as Caplora's own companion app). OpenAI-compatible
 * speech services can be entered on the setup page instead. */
#define DEFAULT_BASE_URL "https://caploralab.online/api/v1"
#define DEFAULT_CHAT_MODEL "deepseek-v4.1-flash"
#define DEFAULT_STT_URL "https://caploralab.online/api/platform/apps/companion-chat/transcribe"
#define DEFAULT_STT_MODEL ""
#define DEFAULT_TTS_URL "https://caploralab.online/api/platform/apps/companion-chat/speech"
#define DEFAULT_TTS_MODEL ""
#define DEFAULT_TTS_VOICE "female"

typedef struct {
    uint32_t version;
    app_pet_settings_t s;
} blob_t;

/* Module singleton: one settings store per boot. `s_cur` is guarded by
 * `s_mutex`; the callback is fixed at init. */
static SemaphoreHandle_t s_mutex;
static app_pet_settings_t s_cur;
static app_pet_settings_cb_t s_cb;
static void *s_cb_ctx;

static void voice_defaults(app_pet_settings_t *s)
{
    snprintf(s->stt_url, sizeof(s->stt_url), "%s", DEFAULT_STT_URL);
    snprintf(s->stt_model, sizeof(s->stt_model), "%s", DEFAULT_STT_MODEL);
    snprintf(s->tts_url, sizeof(s->tts_url), "%s", DEFAULT_TTS_URL);
    snprintf(s->tts_model, sizeof(s->tts_model), "%s", DEFAULT_TTS_MODEL);
    snprintf(s->tts_voice, sizeof(s->tts_voice), "%s", DEFAULT_TTS_VOICE);
}

static void defaults(app_pet_settings_t *s)
{
    memset(s, 0, sizeof(*s));
    snprintf(s->base_url, sizeof(s->base_url), "%s", DEFAULT_BASE_URL);
    snprintf(s->chat_model, sizeof(s->chat_model), "%s", DEFAULT_CHAT_MODEL);
    voice_defaults(s);
    s->animal = PET_ANIMAL_DOG;
    s->volume = 70;
    s->talk_on_its_own = true;
}

static void terminate_strings(app_pet_settings_t *s)
{
#define TERM(f) s->f[sizeof(s->f) - 1] = '\0'
    TERM(hotspot_ssid); TERM(hotspot_password); TERM(api_key); TERM(base_url); TERM(chat_model);
    TERM(stt_url); TERM(stt_model); TERM(tts_url); TERM(tts_model); TERM(tts_voice); TERM(name);
    TERM(setup_password); TERM(voice_api_key); TERM(backup_key); TERM(backup_model);
#undef TERM
    if (s->backup_provider >= APP_PET_PROVIDER_COUNT) {
        s->backup_provider = APP_PET_PROVIDER_NONE;
    }
    if (s->animal >= PET_ANIMAL_COUNT) {
        s->animal = PET_ANIMAL_DOG;
    }
    if (s->volume > 100) {
        s->volume = 100;
    }
}

static esp_err_t save(const app_pet_settings_t *s)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NAMESPACE, NVS_READWRITE, &h), TAG, "open");
    static blob_t blob; /* 1.2 KiB: keep off the caller's stack; guarded by s_mutex */
    blob.version = BLOB_VERSION;
    blob.s = *s;
    esp_err_t err = nvs_set_blob(h, BLOB_KEY, &blob, sizeof(blob));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t app_pet_settings_init(app_pet_settings_cb_t on_change, void *ctx)
{
    ESP_RETURN_ON_FALSE(!s_mutex, ESP_ERR_INVALID_STATE, TAG, "already initialized");
    s_mutex = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_mutex, ESP_ERR_NO_MEM, TAG, "mutex");
    s_cb = on_change;
    s_cb_ctx = ctx;

    defaults(&s_cur);
    nvs_handle_t h;
    if (nvs_open(NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        static blob_t blob;
        size_t len = sizeof(blob);
        memset(&blob, 0, sizeof(blob));
        const esp_err_t err = nvs_get_blob(h, BLOB_KEY, &blob, &len);
        if (err == ESP_OK && len == sizeof(blob) && blob.version == BLOB_VERSION) {
            s_cur = blob.s;
            terminate_strings(&s_cur);
        } else if (err == ESP_OK && blob.version >= 1u && blob.version < BLOB_VERSION && len < sizeof(blob)) {
            /* Older blobs are strict prefixes; appended fields stay zero. If no
             * voice key was ever saved, voice never worked (v1 pointed at
             * Caplora paths that 404, v2 at OpenAI): move to the free defaults. */
            s_cur = blob.s;
            terminate_strings(&s_cur);
            if (s_cur.voice_api_key[0] == '\0') {
                voice_defaults(&s_cur);
            }
            ESP_LOGI(TAG, "migrated settings v%u -> v%u", (unsigned)blob.version, (unsigned)BLOB_VERSION);
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            if (save(&s_cur) != ESP_OK) {
                ESP_LOGW(TAG, "could not persist migrated settings");
            }
            xSemaphoreGive(s_mutex);
        }
        nvs_close(h);
    }
    if (s_cur.voice_api_key[0] == '\0') {
        voice_defaults(&s_cur); /* no voice key: use the Companion defaults */
    }
    if (s_cur.setup_password[0] == '\0') {
        /* Random WPA2 password for the setup network, shown on screen. */
        static const char alphabet[] = "abcdefghjkmnpqrstuvwxyz23456789";
        for (int i = 0; i < 10; ++i) {
            s_cur.setup_password[i] = alphabet[esp_random() % (sizeof(alphabet) - 1)];
        }
        s_cur.setup_password[10] = '\0';
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        const esp_err_t err = save(&s_cur);
        xSemaphoreGive(s_mutex);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "could not persist setup password: %s", esp_err_to_name(err));
        }
    }
    ESP_LOGI(TAG, "loaded: animal=%s name=\"%s\" key=%s voice_key=%s voice=\"%s\" backup=%u/%s model=%s",
             pet_animal_id(s_cur.animal), s_cur.name, s_cur.api_key[0] ? "set" : "missing",
             s_cur.voice_api_key[0] ? "set" : "missing", s_cur.tts_voice, (unsigned)s_cur.backup_provider,
             s_cur.backup_key[0] ? "set" : "missing", s_cur.chat_model);
    return ESP_OK;
}

void app_pet_settings_get(app_pet_settings_t *out)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out = s_cur;
    xSemaphoreGive(s_mutex);
}

esp_err_t app_pet_settings_set(const app_pet_settings_t *in)
{
    ESP_RETURN_ON_FALSE(in, ESP_ERR_INVALID_ARG, TAG, "null");
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_cur = *in;
    terminate_strings(&s_cur);
    const esp_err_t err = save(&s_cur);
    /* Callers of set() are serialized (only the setup web server saves), so
     * one static snapshot is enough to hand the callback a stable copy. */
    static app_pet_settings_t snapshot;
    snapshot = s_cur;
    xSemaphoreGive(s_mutex);
    if (err == ESP_OK && s_cb) {
        s_cb(&snapshot, s_cb_ctx);
    }
    return err;
}

esp_err_t app_pet_settings_set_name(const char *name)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    snprintf(s_cur.name, sizeof(s_cur.name), "%s", name ? name : "");
    const esp_err_t err = save(&s_cur);
    xSemaphoreGive(s_mutex);
    return err;
}

void app_pet_settings_wipe(app_pet_settings_t *copy)
{
    memset(copy->api_key, 0, sizeof(copy->api_key));
    memset(copy->voice_api_key, 0, sizeof(copy->voice_api_key));
    memset(copy->backup_key, 0, sizeof(copy->backup_key));
    memset(copy->hotspot_password, 0, sizeof(copy->hotspot_password));
}

bool app_pet_settings_has_key(const app_pet_settings_t *s)
{
    return s && s->api_key[0] != '\0';
}

bool app_pet_settings_has_backup(const app_pet_settings_t *s)
{
    return s && s->backup_provider != APP_PET_PROVIDER_NONE && s->backup_key[0] != '\0';
}

bool app_pet_settings_can_chat(const app_pet_settings_t *s)
{
    return app_pet_settings_has_key(s) || app_pet_settings_has_backup(s);
}

