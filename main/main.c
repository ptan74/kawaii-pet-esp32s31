// SPDX-License-Identifier: Apache-2.0
#include <ctype.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "app_display.h"
#include "app_pet_audio.h"
#include "app_pet_brain.h"
#include "app_pet_net.h"
#include "app_pet_settings.h"
#include "app_pet_ui.h"
#include "app_pet_info.h"
#include "app_pet_market.h"
#include "app_stations.h"
#include "esp_iris.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "iris_ota_support.h"
#include "iris_screen_mirror.h"
#include "nvs_flash.h"
#include "ui_bundle.h"

static const char *TAG = "kawaii_pet";

#define DEFAULT_STATION "Class 95"
#define SETUP_TEXT_MAX 640

/* Composition root: owned handles and the little routing state that crosses
 * components. `ui` is published once before any producer starts; `talk_kind`
 * crosses from the render task to the audio task; `net` is guarded by
 * `net_lock`; `setup_text` by `setup_lock`. Everything lives until reboot. */
static struct {
    esp_gsp_handle_t gsp;
    _Atomic(app_pet_ui_handle_t) ui;
    SemaphoreHandle_t ui_lock;
    SemaphoreHandle_t setup_lock;
    _Atomic int talk_kind;
    portMUX_TYPE net_lock;
    app_pet_net_status_t net;
    char setup_text[SETUP_TEXT_MAX];
    app_pet_settings_t settings;     /* boot-time snapshot (app_main only) */
    app_pet_settings_t setup_settings; /* guarded by setup_lock */
} app = {.net_lock = portMUX_INITIALIZER_UNLOCKED};

static app_pet_ui_handle_t ui(void)
{
    return atomic_load(&app.ui);
}

/* ---- setup panel text ------------------------------------------------------- */

/* Called from the Wi-Fi event, web server and render tasks. */
static void refresh_setup_text(void)
{
    app_pet_net_status_t n;
    portENTER_CRITICAL(&app.net_lock);
    n = app.net;
    portEXIT_CRITICAL(&app.net_lock);
    if (!app.setup_lock || xSemaphoreTake(app.setup_lock, pdMS_TO_TICKS(200)) != pdTRUE) {
        return;
    }
    app_pet_settings_t *const sp = &app.setup_settings;
    app_pet_settings_get(sp);
    const char *hotspot = n.sta_ssid[0] ? n.sta_ssid : "not set yet";
    const char *state = n.online ? "connected" : n.sta_ssid[0] ? "connecting..." : "";
    snprintf(app.setup_text, sizeof(app.setup_text),
             "1. Turn on your phone's hotspot.\n"
             "2. On your phone, join Wi-Fi \"%s\"\n"
             "   password: %s\n"
             "3. Open http://%s in your browser.\n"
             "4. Enter your hotspot, Caplora key and pet\n"
             "   choices, then tap Save.\n\n"
             "Hotspot: %s %s\n"
             "Caplora key: %s   Backup AI: %s\n"
             "Voice key: %s\n"
             "Chat model: %s%s%s",
             n.setup_ssid, n.setup_password, n.setup_ip, hotspot, state,
             sp->api_key[0] ? "saved" : "not set", app_pet_settings_has_backup(sp) ? "saved" : "none",
             sp->voice_api_key[0] ? "saved" : "not set (no talking yet)", sp->chat_model,
             n.online ? "\n\nOn the hotspot, open http://" : "", n.online ? n.sta_ip : "");
    app_pet_settings_wipe(sp);
    app_pet_ui_set_setup_text(ui(), app.setup_text);
    xSemaphoreGive(app.setup_lock);
}

static void on_net(const app_pet_net_status_t *status, void *ctx)
{
    (void)ctx;
    portENTER_CRITICAL(&app.net_lock);
    app.net = *status;
    portEXIT_CRITICAL(&app.net_lock);
    refresh_setup_text();
}

/* ---- settings ------------------------------------------------------------------- */

/* Device volume buttons: save the new level; on_settings applies it to audio and the label. */
static void ui_volume_step(int delta, void *ctx)
{
    (void)ctx;
    app_pet_settings_t cur;
    app_pet_settings_get(&cur);
    int v = (int)cur.volume + delta;
    cur.volume = (uint8_t)(v < 0 ? 0 : v > 100 ? 100 : v);
    (void)app_pet_settings_set(&cur);
}

static void on_settings(const app_pet_settings_t *s, void *ctx)
{
    (void)ctx;
    app_pet_ui_set_volume(ui(), s->volume);
    app_pet_ui_set_identity(ui(), (pet_animal_t)s->animal, s->name);
    app_pet_ui_set_talk_on_its_own(ui(), s->talk_on_its_own);
    app_pet_audio_set_volume(s->volume);
    app_pet_brain_forget();
    refresh_setup_text();
}

/* ---- music ----------------------------------------------------------------------- */

static bool contains_nocase(const char *hay, const char *needle)
{
    const size_t n = strlen(needle);
    for (; *hay; ++hay) {
        if (strncasecmp(hay, needle, n) == 0) {
            return true;
        }
    }
    return false;
}

static const app_station_t *find_station(const char *wanted)
{
    const app_station_t *fallback = NULL;
    for (size_t i = 0; i < app_stations_count(); ++i) {
        const app_station_t *st = app_stations_get(i);
        if (contains_nocase(st->name, DEFAULT_STATION)) {
            fallback = st;
        }
        if (wanted && wanted[0] && (contains_nocase(st->name, wanted) || contains_nocase(wanted, st->name))) {
            return st;
        }
    }
    return fallback ? fallback : app_stations_get(0);
}

/* ---- UI sink (render task; everything here only queues work) ------------------ */

static void ui_talk(bool start, app_pet_talk_kind_t kind, void *ctx)
{
    (void)ctx;
    if (start) {
        atomic_store(&app.talk_kind, (int)kind);
        app_pet_audio_record_start();
    } else {
        app_pet_audio_record_stop();
    }
}

static void ui_sound(pet_animal_t animal, pet_sound_t sound, void *ctx)
{
    (void)ctx;
    app_pet_audio_effect(animal, sound);
}

static void ui_speak(const char *text, void *ctx)
{
    (void)ctx;
    app_pet_brain_say(text);
}

static void ui_music(bool on, const char *station, void *ctx)
{
    (void)ctx;
    if (!on) {
        app_pet_audio_music(NULL);
        return;
    }
    const app_station_t *st = find_station(station);
    ESP_LOGI(TAG, "music: %s", st->name);
    app_pet_audio_music(st->stream_url);
}

static void ui_save_name(const char *name, void *ctx)
{
    (void)ctx;
    if (app_pet_settings_set_name(name) != ESP_OK) {
        ESP_LOGW(TAG, "could not save name");
    }
    app_pet_brain_forget();
}

static void ui_setup(bool open, void *ctx)
{
    (void)ctx;
    if (open) {
        refresh_setup_text();
    }
}

static void ui_lock_take(void *ctx)
{
    xSemaphoreTake((SemaphoreHandle_t)ctx, portMAX_DELAY);
}

static void ui_lock_give(void *ctx)
{
    xSemaphoreGive((SemaphoreHandle_t)ctx);
}

/* ---- audio and brain results ------------------------------------------------------ */

static void on_audio(app_pet_audio_event_t ev, float level, void *ctx)
{
    (void)ctx;
    switch (ev) {
    case APP_PET_AUDIO_CLIP_STARTED:
        app_pet_ui_set_phase(ui(), APP_PET_PHASE_SPEAKING);
        break;
    case APP_PET_AUDIO_CLIP_LEVEL:
        app_pet_ui_set_voice_level(ui(), level);
        break;
    case APP_PET_AUDIO_CLIP_DONE:
        app_pet_ui_set_phase(ui(), APP_PET_PHASE_IDLE);
        break;
    case APP_PET_AUDIO_MUSIC_ON:
        app_pet_ui_set_music(ui(), true);
        break;
    case APP_PET_AUDIO_MUSIC_OFF:
        app_pet_ui_set_music(ui(), false);
        break;
    }
}

static void on_recording(int16_t *pcm, size_t samples, void *ctx)
{
    (void)ctx;
    const app_pet_brain_kind_t kind = atomic_load(&app.talk_kind) == APP_PET_TALK_NAME
                                      ? APP_PET_BRAIN_NAME : APP_PET_BRAIN_CHAT;
    app_pet_brain_listen(kind, pcm, samples);
}

static void brain_heard(app_pet_brain_kind_t kind, const char *text, void *ctx)
{
    (void)ctx;
    app_pet_ui_heard(ui(), kind == APP_PET_BRAIN_NAME ? APP_PET_TALK_NAME : APP_PET_TALK_CHAT, text);
}

static void brain_reply(const char *raw, void *ctx)
{
    (void)ctx;
    app_pet_ui_reply(ui(), raw);
}

static void brain_voice(uint8_t *audio, size_t len, void *ctx)
{
    (void)ctx;
    app_pet_audio_clip(audio, len);
}

static void brain_done(void *ctx)
{
    (void)ctx;
    app_pet_ui_set_phase(ui(), APP_PET_PHASE_IDLE);
}

static void on_weather(const char *line, void *ctx)
{
    (void)ctx;
    app_pet_ui_handle_t h = atomic_load(&app.ui);
    if (h) {
        app_pet_ui_set_weather(h, line);
    }
}

static void on_markets(const char *rows, const char *headlines, void *ctx)
{
    (void)ctx;
    app_pet_ui_handle_t h = atomic_load(&app.ui);
    if (h) {
        app_pet_ui_set_markets(h, rows, headlines);
    }
}

static bool brain_online(void *ctx)
{
    (void)ctx;
    return app_pet_net_online();
}

static void brain_error(const char *msg, void *ctx)
{
    (void)ctx;
    app_pet_ui_error(ui(), msg);
}

/* ---- boot ----------------------------------------------------------------------------- */

void app_main(void)
{
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_iris_boot_probe());
    ESP_ERROR_CHECK(nvs_flash_init());
    /* Recovery must remain reachable even when the UI bundle cannot be loaded. */
    iris_ota_support_start();

    esp_gsp_config_t config;
    const esp_err_t err = ui_bundle_open(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GSP UI unavailable (0x%x); Recovery RPC remains active", err);
        return;
    }
    esp_gsp_esp_lcd_config_t display;
    const esp_err_t display_err = app_display_init(&display);
    if (display_err != ESP_OK) {
        ESP_LOGE(TAG, "Display unavailable (0x%x); Recovery RPC remains active", display_err);
        return;
    }
    ESP_ERROR_CHECK(iris_screen_mirror_init());
    ESP_ERROR_CHECK(esp_gsp_esp_lcd_start(&config, &display, &app.gsp));
    ESP_ERROR_CHECK(iris_screen_mirror_attach(app.gsp));

    app.setup_lock = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(app.setup_lock ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(app_pet_settings_init(on_settings, NULL));
    app_pet_settings_get(&app.settings);

    /* Audio and brain first, so the pet's first greeting can be heard. Either
     * failing leaves a silent but still animated pet. */
    const app_pet_audio_config_t audio_cfg = {
        .volume = app.settings.volume, .event_cb = on_audio, .rec_cb = on_recording,
    };
    ESP_ERROR_CHECK_WITHOUT_ABORT(app_pet_audio_start(&audio_cfg));
    const app_pet_brain_cb_t brain_cb = {
        .heard = brain_heard, .reply = brain_reply, .voice = brain_voice, .done = brain_done, .error = brain_error,
        .online = brain_online,
    };
    ESP_ERROR_CHECK_WITHOUT_ABORT(app_pet_brain_start(&brain_cb));

    app.ui_lock = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(app.ui_lock ? ESP_OK : ESP_ERR_NO_MEM);
    const app_pet_ui_config_t ui_cfg = {
        .animal = (pet_animal_t)app.settings.animal,
        .name = app.settings.name,
        .talk_on_its_own = app.settings.talk_on_its_own,
        .seed = esp_random(),
        .sink = {.talk = ui_talk, .sound = ui_sound, .speak = ui_speak, .music = ui_music,
                 .save_name = ui_save_name, .setup = ui_setup,
                 .volume_step = ui_volume_step},
        .lock = {.lock = ui_lock_take, .unlock = ui_lock_give, .ctx = app.ui_lock},
    };
    app_pet_settings_wipe(&app.settings);
    app_pet_ui_handle_t handle;
    ESP_ERROR_CHECK(app_pet_ui_create(app.gsp, &ui_cfg, &handle));
    atomic_store(&app.ui, handle);

    /* Network last: its status callbacks need the UI. */
    ESP_ERROR_CHECK_WITHOUT_ABORT(app_pet_net_start(on_net, NULL));
    ESP_ERROR_CHECK_WITHOUT_ABORT(app_pet_info_start(on_weather, NULL));
    ESP_ERROR_CHECK_WITHOUT_ABORT(app_pet_market_start(on_markets, NULL));
    ESP_LOGI(TAG, "kawaii pet ready");
}
