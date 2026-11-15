// SPDX-License-Identifier: Apache-2.0
/* Native simulator backend: runs the shared pet controller with a scripted
 * stand-in for the microphone, Caplora and the speaker, so moods, actions,
 * onboarding and the bubble can be exercised without hardware or a key.
 * Each mic hold replays the next scripted exchange. */
#include <stdio.h>
#include <string.h>

#include "app_pet_ui.h"
#include "gsp_sim_bridge.h"

/* Simulator: no network time offset is needed; host time is used. */
int app_pet_info_utc_offset(void)
{
    return 0;
}

#define THINK_MS 900
#define SPEAK_MS 2600
#define START_ANIMAL PET_ANIMAL_DOG

typedef struct {
    const char *heard;
    const char *reply;
} exchange_t;

static const exchange_t s_script[] = {
    {"Tell me a joke", "[mood:laugh] Why did the dog sit in the shade? Because he didn't want to be a **hot dog**! Haha!"},
    {"You're so cute", "[mood:shy] Aww, stop it... you're making me *blush*, Patrick."},
    {"Play some music", "[mood:dance][action:music:Class 95] ## Sure!\nPlaying **Class 95** for you, Patrick. Let's dance!"},
    {"Stop the music", "[mood:smile][action:stop] Okay, music off. That was fun!"},
    {"I'm going to eat your bone", "[mood:angry] Hey! That's MY bone. Grrr!"},
    {"Boo!", "[mood:surprised] Whoa! You scared me, Patrick!"},
    {"I had a bad day", "[mood:sad] Oh no, I'm sorry. Come here, I'll cuddle you until you feel better."},
    {"I love you", "[mood:love] I love you too, Patrick! You're the best human in the whole world."},
    {"Tell me about Singapore in detail",
     "[mood:happy] Singapore is a sunny island city with:\n- **Gardens by the Bay**\n- Hawker centres with chicken rice\n"
     "- Lots of parks for walks!\n\nIt's my favourite place because you live here."},
};

/* The bridge ABI has no caller context; keep the backend's state here. */
static struct {
    esp_gsp_handle_t ui;
    app_pet_ui_handle_t pet;
    void *timer;
    int step;            /* 0 idle, 1 heard pending, 2 reply pending, 3 speaking */
    app_pet_talk_kind_t kind;
    size_t next;
    bool music;
} b;

static void stop_timer(void)
{
    if (b.timer) {
        (void)esp_gsp_timer_delete(b.ui, b.timer);
        b.timer = NULL;
    }
}

static void advance(esp_gsp_handle_t ui, void *ctx);

static void schedule(uint32_t ms)
{
    stop_timer();
    b.timer = esp_gsp_timer_create(b.ui, ms, advance, NULL);
}

static void advance(esp_gsp_handle_t ui, void *ctx)
{
    (void)ui;
    (void)ctx;
    stop_timer();
    const exchange_t *x = &s_script[b.next % (sizeof(s_script) / sizeof(s_script[0]))];
    switch (b.step) {
    case 1:
        if (b.kind == APP_PET_TALK_NAME) {
            app_pet_ui_heard(b.pet, APP_PET_TALK_NAME, "Um, hi! My name is patrick.");
            b.step = 0;
            return;
        }
        fprintf(stderr, "pet_backend: heard \"%s\"\n", x->heard);
        app_pet_ui_heard(b.pet, APP_PET_TALK_CHAT, x->heard);
        b.step = 2;
        schedule(THINK_MS);
        break;
    case 2:
        app_pet_ui_reply(b.pet, x->reply);
        app_pet_ui_set_phase(b.pet, APP_PET_PHASE_SPEAKING);
        b.step = 3;
        schedule(SPEAK_MS);
        break;
    case 3:
        app_pet_ui_set_phase(b.pet, APP_PET_PHASE_IDLE);
        b.step = 0;
        ++b.next;
        break;
    default:
        break;
    }
}

static void sim_talk(bool start, app_pet_talk_kind_t kind, void *ctx)
{
    (void)ctx;
    fprintf(stderr, "pet_backend: mic %s (%s)\n", start ? "on" : "off", kind == APP_PET_TALK_NAME ? "name" : "chat");
    if (!start) {
        b.kind = kind;
        b.step = 1;
        schedule(THINK_MS);
    }
}

static void sim_sound(pet_animal_t animal, pet_sound_t sound, void *ctx)
{
    (void)ctx;
    fprintf(stderr, "pet_backend: sound %s #%d\n", pet_animal_id(animal), (int)sound);
}

static void sim_speak(const char *text, void *ctx)
{
    (void)ctx;
    fprintf(stderr, "pet_backend: speak \"%s\"\n", text);
}

static void sim_music(bool on, const char *station, void *ctx)
{
    (void)ctx;
    fprintf(stderr, "pet_backend: music %s %s\n", on ? "on" : "off", station);
    b.music = on;
    app_pet_ui_set_music(b.pet, on);
}

static void sim_save_name(const char *name, void *ctx)
{
    (void)ctx;
    fprintf(stderr, "pet_backend: saved name \"%s\"\n", name);
}

static void sim_setup(bool open, void *ctx)
{
    (void)ctx;
    fprintf(stderr, "pet_backend: setup %s\n", open ? "open" : "closed");
}

esp_gsp_err_t gsp_bridge_app_init(esp_gsp_handle_t ui)
{
    memset(&b, 0, sizeof(b));
    b.ui = ui;
    const app_pet_ui_config_t cfg = {
        .animal = START_ANIMAL,
        .name = "",
        .talk_on_its_own = true,
        .seed = 7,
        .sink = {.talk = sim_talk, .sound = sim_sound, .speak = sim_speak, .music = sim_music,
                 .save_name = sim_save_name, .setup = sim_setup},
    };
    const esp_gsp_err_t err = app_pet_ui_create(ui, &cfg, &b.pet);
    if (err == ESP_GSP_OK) {
        app_pet_ui_set_setup_text(b.pet,
            "1. Turn on your phone's hotspot.\n"
            "2. On your phone, join Wi-Fi \"KawaiiPet-1C76\",\n"
            "   password 1c76-pet-setup.\n"
            "3. Open http://192.168.4.1 in your browser.\n"
            "4. Enter your hotspot, Caplora key, model,\n"
            "   voice and animal, then tap Save.\n\n"
            "Wi-Fi: not connected\nCaplora key: not set");
    }
    return err;
}

void gsp_bridge_app_deinit(esp_gsp_handle_t ui)
{
    (void)ui;
    stop_timer();
    /* The bridge calls deinit after poll exits; no callbacks can use the state. */
    const esp_gsp_err_t err = app_pet_ui_destroy(&b.pet);
    if (err != ESP_GSP_OK) {
        fprintf(stderr, "pet_backend: cleanup failed (%d)\n", err);
    }
}
