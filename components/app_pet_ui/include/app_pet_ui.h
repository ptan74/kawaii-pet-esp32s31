// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_gsp.h"
#include "pet_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque pet screen controller; contracts in ../README.md. */
typedef struct app_pet_ui *app_pet_ui_handle_t;

/** What a held-mic recording is for. */
typedef enum {
    APP_PET_TALK_CHAT = 0,   /**< Normal conversation. */
    APP_PET_TALK_NAME,       /**< First-run: the owner says their name. */
} app_pet_talk_kind_t;

/** Conversation phase reported by the platform. */
typedef enum {
    APP_PET_PHASE_IDLE = 0,
    APP_PET_PHASE_THINKING,  /**< Transcribing / waiting for the model. */
    APP_PET_PHASE_SPEAKING,  /**< Reply audio is playing. */
} app_pet_phase_t;

/** Platform side. Callbacks run on the GSP render task and must not block.
 * Any member may be NULL. */
typedef struct {
    /** Mic button pressed (start=true) or released. */
    void (*talk)(bool start, app_pet_talk_kind_t kind, void *ctx);
    /** Play a synthesized effect. */
    void (*sound)(pet_animal_t animal, pet_sound_t sound, void *ctx);
    /** Speak a line aloud (spontaneous chatter, greetings). Plain text. */
    void (*speak)(const char *text, void *ctx);
    /** Start (station may be "" for default) or stop music. */
    void (*music)(bool on, const char *station, void *ctx);
    /** Persist the owner's name after first-run capture. */
    void (*save_name)(const char *name, void *ctx);
    /** Setup panel opened (true) or closed. */
    void (*setup)(bool open, void *ctx);
    /** Volume buttons on the Setup panel: +1 step (+10) or -1 step (-10), saved by the platform. */
    void (*volume_step)(int delta, void *ctx);
    void *ctx;
} app_pet_ui_sink_t;

/** Locks guarding cross-task inputs. NULL members mean single-threaded use. */
typedef struct {
    void (*lock)(void *ctx);
    void (*unlock)(void *ctx);
    void *ctx;
} app_pet_ui_lock_t;

typedef struct {
    pet_animal_t animal;
    const char *name;          /**< Owner's name; NULL/"" starts first-run capture. */
    bool talk_on_its_own;      /**< Speak spontaneous lines through sink.speak. */
    uint32_t seed;             /**< Varies idle behaviour. */
    app_pet_ui_sink_t sink;    /**< Copied; ctx must outlive the controller. */
    app_pet_ui_lock_t lock;    /**< Copied. */
} app_pet_ui_config_t;

/** Bind the pet scene on ui and start animating. *out_handle NULL on failure. */
esp_gsp_err_t app_pet_ui_create(esp_gsp_handle_t ui, const app_pet_ui_config_t *config,
                                app_pet_ui_handle_t *out_handle);

/* Inputs below are thread-safe, non-blocking and latest-wins; the render
 * task applies them within one frame (~50 ms). Text is copied. */

/** What the owner said (transcript) for a talk of the given kind. */
void app_pet_ui_heard(app_pet_ui_handle_t h, app_pet_talk_kind_t kind, const char *transcript);
/** Raw model reply; tags like [mood:x] [action:y] are applied and stripped. */
void app_pet_ui_reply(app_pet_ui_handle_t h, const char *raw_reply);
void app_pet_ui_set_phase(app_pet_ui_handle_t h, app_pet_phase_t phase);
/** Reply audio loudness for lip-sync, 0..1. */
void app_pet_ui_set_voice_level(app_pet_ui_handle_t h, float level);
/** Problem to tell the owner about (plain text); the pet looks sad. */
void app_pet_ui_error(app_pet_ui_handle_t h, const char *message);
/** Actual music state from the player. */
void app_pet_ui_set_music(app_pet_ui_handle_t h, bool playing);
/** Animal and/or name changed on the phone (name NULL keeps it). */
void app_pet_ui_set_identity(app_pet_ui_handle_t h, pet_animal_t animal, const char *name);
/** Weather line for the clock screen, e.g. "Singapore 31C Partly cloudy". */
void app_pet_ui_set_weather(app_pet_ui_handle_t h, const char *line);
/** Text for the setup panel (pre-wrapped with '\n'). */
void app_pet_ui_set_setup_text(app_pet_ui_handle_t h, const char *text);
/** Market rows ("Name|Value" lines, '\n'-separated) and headline lines for the news screen. */
void app_pet_ui_set_markets(app_pet_ui_handle_t h, const char *rows, const char *news);
void app_pet_ui_set_talk_on_its_own(app_pet_ui_handle_t h, bool enabled);
/** Current speaker volume 0..100, shown on the Setup panel. */
void app_pet_ui_set_volume(app_pet_ui_handle_t h, uint8_t volume);

/** Detach and free. Quiesce GSP dispatch first. */
esp_gsp_err_t app_pet_ui_destroy(app_pet_ui_handle_t *handle);

#ifdef __cplusplus
}
#endif
