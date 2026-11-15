// SPDX-License-Identifier: Apache-2.0
#include "app_pet_ui.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The canvas API addresses its target by raw bind ID, an opt-in. */
#define GSP_BUNDLE_ENABLE_RAW_IDS 1
#include "bundle_gsp.h"
#include "pet_life.h"
#include "pet_render.h"
#include "pet_text.h"
#include "radio_screen.h"
#include "esp_log.h"

static const char *TAG_UI = "pet_ui";
#include <time.h>

#define FRAME_MS 50
#define FRAME_S (FRAME_MS / 1000.0f)
#define BACKGROUND 0x1B1A2Eu

#define BUBBLE_LINE_CHARS 42
#define BUBBLE_LINES 3
#define BUBBLE_MIN_PAGE_S 3.0f
#define BUBBLE_S_PER_CHAR 0.065f
#define TALK_MAX_S 15.0f

#define REPLY_MAX 1024
#define HEARD_MAX 256
#define LINE_MAX 256
#define SETUP_MAX 640

/* Inbox: written by any task under the platform lock, drained once per frame
 * on the render task into `work`. Flags mark which fields are new. */
enum {
    IN_HEARD = 1u << 0,
    IN_REPLY = 1u << 1,
    IN_PHASE = 1u << 2,
    IN_LEVEL = 1u << 3,
    IN_ERROR = 1u << 4,
    IN_MUSIC = 1u << 5,
    IN_IDENTITY = 1u << 6,
    IN_SETUP = 1u << 7,
    IN_TALK_OWN = 1u << 8,
    IN_WEATHER = 1u << 9,
    IN_MARKET = 1u << 10,
    IN_VOLUME = 1u << 11,
};

/* Market text from app_pet_market: rows "Name|Value" and headline lines. */
#define MARKET_ROWS_MAX 512
#define MARKET_NEWS_MAX 2048
#define NEWS_ITEMS_MAX 12   /* headlines shown in the scrolling list */
#define NEWS_ITEM_MAX 200   /* wrapped headline text, with '\n' line breaks */
#define NEWS_WRAP_COLS 40   /* characters per headline line, at a space */
#define MARKET_COL_MAX 256

typedef struct {
    uint32_t flags;
    app_pet_talk_kind_t heard_kind;
    char heard[HEARD_MAX];
    char reply[REPLY_MAX];
    app_pet_phase_t phase;
    float level;
    char error[LINE_MAX];
    bool music;
    pet_animal_t animal;
    bool has_name;
    char name[PET_NAME_MAX];
    char setup[SETUP_MAX];
    bool talk_own;
    char weather[96];
    char market_rows[MARKET_ROWS_MAX];
    char market_news[MARKET_NEWS_MAX];
    uint8_t volume;
} inbox_t;

/* Single owner: everything except `in` belongs to the GSP render task (event,
 * timer and canvas callbacks). `in` is guarded by config.lock. */
struct app_pet_ui {
    esp_gsp_handle_t ui;
    app_pet_ui_sink_t sink;
    app_pet_ui_lock_t lock;
    inbox_t in;
    inbox_t work;

    pet_life_t life;
    pet_scene_t scene;
    void *frame_timer;
    float time_s;

    bool onboarding;
    bool talking;
    float talk_held_s;
    app_pet_talk_kind_t talk_kind;
    app_pet_phase_t phase;
    float voice_level;
    bool music_on;
    bool resume_music;
    bool talk_own;
    bool setup_open;
    bool radio_open;
    radio_screen_t radio;
    bool clock_open;
    pet_scene_t clock_scene;
    int clock_shown_min;
    int clock_shown_sec;
    char weather[96];
    bool weather_dirty;
    bool news_open;
    char market_names[MARKET_COL_MAX];
    char market_values[MARKET_COL_MAX];
    /* Headlines for the scrolling news list: wrapped text, one per row. */
    char news_items[NEWS_ITEMS_MAX][NEWS_ITEM_MAX];
    uint32_t news_count;
    esp_gsp_list_t news_list;
    char last_heard[HEARD_MAX];

    char bubble[REPLY_MAX];
    size_t page_next;
    float page_left_s;
    char page[BUBBLE_LINES * (BUBBLE_LINE_CHARS + 1) + 1];

    pet_mood_t shown_mood;
    int shown_tummy;
    int shown_sleep;
    int shown_music;
    char line[LINE_MAX];   /* scratch for formatted lines */
    esp_gsp_err_t last_error;
};

static void remember(struct app_pet_ui *s, esp_gsp_err_t err)
{
    if (err != ESP_GSP_OK) {
        s->last_error = err;
    }
}

static void lock(struct app_pet_ui *s)
{
    if (s->lock.lock) {
        s->lock.lock(s->lock.ctx);
    }
}

static void unlock(struct app_pet_ui *s)
{
    if (s->lock.unlock) {
        s->lock.unlock(s->lock.ctx);
    }
}

/* ---- speech bubble ------------------------------------------------------- */

static void bubble_page(struct app_pet_ui *s)
{
    const size_t start = s->page_next;
    s->page_next = pet_text_page(s->bubble, start, BUBBLE_LINE_CHARS, BUBBLE_LINES, s->page, sizeof(s->page));
    const float t = (float)(s->page_next - start) * BUBBLE_S_PER_CHAR;
    s->page_left_s = t > BUBBLE_MIN_PAGE_S ? t : BUBBLE_MIN_PAGE_S;
    remember(s, gsp_pet_bubble_set_text(s->ui, s->page));
}

/** Show plain text (already clean) from its first page. */
static void bubble_show(struct app_pet_ui *s, const char *text)
{
    /* memmove: callers may pass text that already lives in s->bubble. */
    size_t n = text ? strlen(text) : 0;
    n = n < sizeof(s->bubble) - 1 ? n : sizeof(s->bubble) - 1;
    memmove(s->bubble, text ? text : "", n);
    s->bubble[n] = '\0';
    s->page_next = 0;
    bubble_page(s);
}

static void bubble_tick(struct app_pet_ui *s)
{
    if (s->page_left_s <= 0) {
        return;
    }
    s->page_left_s -= FRAME_S;
    if (s->page_left_s <= 0 && s->page_next < strlen(s->bubble)) {
        bubble_page(s);
    }
}

/** Show and, when enabled, speak a spontaneous line. */
static void say_line(struct app_pet_ui *s, const char *text, bool speak)
{
    bubble_show(s, text);
    if (speak && s->talk_own && s->sink.speak && s->phase == APP_PET_PHASE_IDLE && !s->talking) {
        s->sink.speak(text, s->sink.ctx);
    }
}

static void play(struct app_pet_ui *s, pet_sound_t sound)
{
    if (sound != PET_SOUND_NONE && s->sink.sound) {
        s->sink.sound(s->life.animal, sound, s->sink.ctx);
    }
}

static void apply_event(struct app_pet_ui *s, const pet_life_event_t *e, bool speak)
{
    play(s, e->sound);
    if (e->say[0]) {
        say_line(s, e->say, speak);
    }
}

/* ---- labels ---------------------------------------------------------------- */

static void render_greeting(struct app_pet_ui *s)
{
    if (s->life.name[0]) {
        snprintf(s->line, sizeof(s->line), "Hi, %s", s->life.name);
    } else {
        snprintf(s->line, sizeof(s->line), "Hi there");
    }
    remember(s, gsp_pet_greeting_set_text(s->ui, s->line));
}

static void render_status(struct app_pet_ui *s, pet_mood_t mood)
{
    if (mood != s->shown_mood) {
        s->shown_mood = mood;
        remember(s, gsp_pet_mood_set_text(s->ui, pet_mood_label(mood)));
    }
    const int tummy = (int)(s->life.hunger * 100.0f + 0.5f);
    if (tummy != s->shown_tummy) {
        s->shown_tummy = tummy;
        snprintf(s->line, sizeof(s->line), "Tummy %d%%", tummy);
        remember(s, gsp_pet_needs_set_text(s->ui, s->line));
    }
    const int asleep = s->life.sleeping ? 1 : 0;
    if (asleep != s->shown_sleep) {
        s->shown_sleep = asleep;
        remember(s, gsp_pet_sleep_label_set_text(s->ui, asleep ? "Wake" : "Sleep"));
    }
    const int music = s->music_on ? 1 : 0;
    if (music != s->shown_music) {
        s->shown_music = music;
    }
}

static void set_talk_visual(struct app_pet_ui *s, bool live)
{
    remember(s, gsp_pet_talk_live_set_visible(s->ui, live));
    remember(s, gsp_pet_talk_label_set_text(s->ui, live ? "Talk" : "Hold"));
}

/* ---- conversation ---------------------------------------------------------- */

static void start_onboarding(struct app_pet_ui *s)
{
    s->onboarding = true;
    snprintf(s->line, sizeof(s->line),
             "Hi! I'm your new %s. What's your name? Hold the mic button and tell me.",
             pet_animal_name(s->life.animal));
    pet_life_react(&s->life, PET_MOOD_HAPPY, 6);
    play(s, PET_SOUND_GREET);
    say_line(s, s->line, true);
}

static void talk_begin(struct app_pet_ui *s)
{
    if (s->talking || s->setup_open) {
        return;
    }
    s->talking = true;
    s->talk_held_s = 0;
    s->talk_kind = s->onboarding ? APP_PET_TALK_NAME : APP_PET_TALK_CHAT;
    pet_life_set_busy(&s->life, true);
    set_talk_visual(s, true);
    if (s->music_on) {
        s->resume_music = true;
        if (s->sink.music) {
            s->sink.music(false, "", s->sink.ctx);
        }
    }
    bubble_show(s, s->onboarding ? "Tell me your name..." : "I'm listening...");
    if (s->sink.talk) {
        s->sink.talk(true, s->talk_kind, s->sink.ctx);
    }
}

static void talk_end(struct app_pet_ui *s)
{
    if (!s->talking) {
        return;
    }
    s->talking = false;
    set_talk_visual(s, false);
    s->phase = APP_PET_PHASE_THINKING;
    bubble_show(s, "Hmm, let me think...");
    if (s->sink.talk) {
        s->sink.talk(false, s->talk_kind, s->sink.ctx);
    }
}

static void finish_conversation(struct app_pet_ui *s)
{
    s->phase = APP_PET_PHASE_IDLE;
    s->voice_level = 0;
    pet_life_set_busy(&s->life, false);
    if (s->resume_music) {
        s->resume_music = false;
        if (s->sink.music) {
            s->sink.music(true, "", s->sink.ctx);
        }
    }
}

static void handle_name(struct app_pet_ui *s, const char *transcript)
{
    char name[PET_NAME_MAX];
    if (pet_name_extract(transcript, name, sizeof(name)) == 0) {
        pet_life_react(&s->life, PET_MOOD_SAD, 4);
        play(s, PET_SOUND_SAD);
        say_line(s, "Sorry, I didn't catch that. Hold the mic button and tell me your name again.", true);
        finish_conversation(s);
        return;
    }
    s->onboarding = false;
    pet_life_set_name(&s->life, name);
    render_greeting(s);
    if (s->sink.save_name) {
        s->sink.save_name(name, s->sink.ctx);
    }
    finish_conversation(s);
    pet_life_react(&s->life, PET_MOOD_LOVE, 5);
    play(s, PET_SOUND_GREET);
    snprintf(s->line, sizeof(s->line),
             "Nice to meet you, %s! Hold the mic button any time you want to talk to me.", name);
    say_line(s, s->line, true);
}

static void run_action(struct app_pet_ui *s, pet_action_t action, const char *arg)
{
    pet_life_event_t e;
    switch (action) {
    case PET_ACTION_MUSIC:
        s->resume_music = false;
        if (s->sink.music) {
            s->sink.music(true, arg ? arg : "", s->sink.ctx);
        }
        break;
    case PET_ACTION_STOP:
        s->resume_music = false;
        if (s->sink.music) {
            s->sink.music(false, "", s->sink.ctx);
        }
        break;
    case PET_ACTION_FEED:
        pet_life_feed(&s->life, &e);
        play(s, e.sound);
        break;
    case PET_ACTION_SLEEP:
        pet_life_sleep(&s->life);
        break;
    case PET_ACTION_WAKE:
        pet_life_wake(&s->life);
        break;
    default:
        break;
    }
}

static void handle_reply(struct app_pet_ui *s, const char *raw)
{
    pet_reply_meta_t meta;
    pet_reply_parse(raw, &meta, s->bubble, sizeof(s->bubble));
    const pet_mood_t mood = meta.mood < PET_MOOD_COUNT ? meta.mood : PET_MOOD_HAPPY;
    const float hold = 4.0f + (float)strlen(s->bubble) * BUBBLE_S_PER_CHAR;
    pet_life_react(&s->life, mood, hold);
    play(s, pet_mood_sound(mood) != PET_SOUND_NONE ? pet_mood_sound(mood) : PET_SOUND_GREET);
    s->page_next = 0;
    bubble_page(s);
    pet_action_t action = meta.action;
    char arg[PET_ACTION_ARG_MAX];
    snprintf(arg, sizeof(arg), "%s", meta.action_arg);
    if (action == PET_ACTION_NONE) {
        action = pet_intent_detect(s->last_heard, arg, sizeof(arg));
    }
    run_action(s, action, arg);
}

/* Headline rows: each headline is broken onto lines of about NEWS_WRAP_COLS characters
 * at a space, so the row template's label can show it without clipping. */
static void wrap_headline(const char *src, char *dst, size_t cap)
{
    size_t o = 0, line_start = 0, space = SIZE_MAX;
    for (const char *p = src; *p && o + 1 < cap; ++p) {
        if (*p == ' ') {
            space = o;
        }
        dst[o++] = *p;
        if (o - line_start > NEWS_WRAP_COLS && space != SIZE_MAX && space > line_start) {
            dst[space] = '\n';
            line_start = space + 1;
            space = SIZE_MAX;
        }
    }
    dst[o] = '\0';
}

static const uint32_t HEADLINE_COLORS[] = {0xFAC775, 0x5DCAA5, 0xAFA9EC, 0xF09595, 0x85B7EB, 0xC0DD97};

/* Render task: fill one recycled headline row with its text and colour. */
static esp_gsp_err_t bind_headline(esp_gsp_handle_t gsp, esp_gsp_row_t row, uint32_t index, void *ctx)
{
    struct app_pet_ui *s = ctx;
    if (index >= s->news_count) {
        return ESP_GSP_ERR_INVALID_ARG;
    }
    const uint32_t color = HEADLINE_COLORS[index % (sizeof(HEADLINE_COLORS) / sizeof(HEADLINE_COLORS[0]))];
    esp_gsp_err_t err = esp_gsp_row_set_text(gsp, row, GSP_PET_TEMPLATE_HEADLINE_ROW_HEADLINE_TEXT_TEXT_SLOT,
                                             s->news_items[index]);
    if (err == ESP_GSP_OK) {
        err = esp_gsp_row_set_color(gsp, row, GSP_PET_TEMPLATE_HEADLINE_ROW_HEADLINE_TEXT_FG_COLOR_SLOT,
                                    pet_rgb565(color));
    }
    return err;
}

/* Split the market news (one headline per line) into wrapped rows and refresh the list. */
static void apply_news(struct app_pet_ui *s, const char *news)
{
    s->news_count = 0;
    const char *p = news;
    while (p && *p && s->news_count < NEWS_ITEMS_MAX) {
        const char *eol = strchr(p, '\n');
        const size_t len = eol ? (size_t)(eol - p) : strlen(p);
        if (len > 0) {
            char line[NEWS_ITEM_MAX];
            const size_t n = len < sizeof(line) ? len : sizeof(line) - 1;
            memcpy(line, p, n);
            line[n] = '\0';
            wrap_headline(line, s->news_items[s->news_count], NEWS_ITEM_MAX);
            s->news_count++;
        }
        p = eol ? eol + 1 : NULL;
    }
    if (s->news_count == 0) {
        snprintf(s->news_items[0], NEWS_ITEM_MAX, "No headlines yet.");
        s->news_count = 1;
    }
    remember(s, esp_gsp_list_set_total(s->ui, s->news_list, s->news_count));
    remember(s, esp_gsp_list_refresh(s->ui, s->news_list));
}

/* Split "Name|Value" rows into the two label columns; rows keep their line order. */
static void apply_markets(struct app_pet_ui *s, const inbox_t *w)
{
    size_t n = 0, v = 0;
    s->market_names[0] = s->market_values[0] = '\0';
    for (const char *p = w->market_rows; *p;) {
        const char *eol = strchr(p, '\n');
        const size_t len = eol ? (size_t)(eol - p) : strlen(p);
        const char *bar = memchr(p, '|', len);
        const size_t name_len = bar ? (size_t)(bar - p) : len;
        const char *value = bar ? bar + 1 : "";
        const size_t value_len = bar ? len - name_len - 1 : 0;
        n += (size_t)snprintf(s->market_names + n, MARKET_COL_MAX - n, "%s%.*s", n ? "\n" : "", (int)name_len, p);
        v += (size_t)snprintf(s->market_values + v, MARKET_COL_MAX - v, "%s%.*s", v ? "\n" : "", (int)value_len, value);
        if (!eol) {
            break;
        }
        p = eol + 1;
    }
    remember(s, gsp_pet_market_names_set_text(s->ui, s->market_names));
    remember(s, gsp_pet_market_values_set_text(s->ui, s->market_values));
    apply_news(s, w->market_news);
}

static void drain_inbox(struct app_pet_ui *s)
{
    lock(s);
    const uint32_t flags = s->in.flags;
    if (flags) {
        memcpy(&s->work, &s->in, sizeof(s->work));
        s->in.flags = 0;
    }
    unlock(s);
    if (!flags) {
        return;
    }
    inbox_t *w = &s->work;
    if (flags & IN_IDENTITY) {
        pet_life_set_animal(&s->life, w->animal);
        if (w->has_name) {
            pet_life_set_name(&s->life, w->name);
            s->onboarding = w->name[0] == '\0';
        }
        render_greeting(s);
    }
    if (flags & IN_WEATHER) {
        snprintf(s->weather, sizeof(s->weather), "%s", w->weather);
        s->weather_dirty = true;
    }
    if (flags & IN_MARKET) {
        apply_markets(s, w);
    }
    if (flags & IN_VOLUME) {
        snprintf(s->line, sizeof(s->line), "Volume %u%%", (unsigned)w->volume);
        remember(s, gsp_pet_volume_text_set_text(s->ui, s->line));
    }
    if (flags & IN_TALK_OWN) {
        s->talk_own = w->talk_own;
    }
    if (flags & IN_SETUP) {
        remember(s, gsp_pet_setup_text_set_text(s->ui, w->setup));
    }
    if (flags & IN_MUSIC) {
        s->music_on = w->music;
        radio_screen_set_music(&s->radio, w->music);
        pet_life_set_music(&s->life, w->music);
    }
    if (flags & IN_HEARD) {
        pet_text_clean(w->heard, s->last_heard, sizeof(s->last_heard));
        if (w->heard_kind == APP_PET_TALK_NAME) {
            handle_name(s, w->heard);
        } else if (s->last_heard[0]) {
            snprintf(s->bubble, sizeof(s->bubble), "You said: %s", s->last_heard);
            s->page_next = 0;
            bubble_page(s);
        }
    }
    if (flags & IN_REPLY) {
        handle_reply(s, w->reply);
    }
    if (flags & IN_ERROR) {
        pet_life_react(&s->life, PET_MOOD_SAD, 5);
        play(s, PET_SOUND_SAD);
        bubble_show(s, w->error);
        finish_conversation(s);
    }
    if (flags & IN_PHASE) {
        if (w->phase == APP_PET_PHASE_IDLE) {
            finish_conversation(s);
        } else {
            s->phase = w->phase;
            pet_life_set_busy(&s->life, true);
        }
    }
    if (flags & IN_LEVEL) {
        s->voice_level = w->level;
    }
}

/* ---- frame loop -------------------------------------------------------------- */

static pet_mood_t current_mood(const struct app_pet_ui *s)
{
    if (s->talking) {
        return PET_MOOD_LISTEN;
    }
    if (s->phase == APP_PET_PHASE_THINKING) {
        return PET_MOOD_THINK;
    }
    return pet_life_mood(&s->life);
}

/* From app_pet_info (network time zone offset); declared here so the UI stays free of IDF-only headers. */
int app_pet_info_utc_offset(void);

static void update_clock(struct app_pet_ui *s, esp_gsp_handle_t ui);

static void frame_tick(esp_gsp_handle_t ui, void *ctx)
{
    struct app_pet_ui *s = ctx;
    s->time_s += FRAME_S;
    drain_inbox(s);

    if (s->talking) {
        s->talk_held_s += FRAME_S;
        if (s->talk_held_s > TALK_MAX_S) {
            talk_end(s); /* finger slid off, or a very long speech */
        }
    }
    pet_life_event_t e;
    if (pet_life_tick(&s->life, FRAME_S, &e)) {
        /* Spontaneous behaviour never interrupts a conversation. */
        if (s->phase == APP_PET_PHASE_IDLE && !s->talking) {
            apply_event(s, &e, true);
        } else if (e.sound == PET_SOUND_EAT) {
            play(s, e.sound);
        }
    }
    bubble_tick(s);

    const pet_mood_t mood = current_mood(s);
    float mouth = 0;
    if (s->phase == APP_PET_PHASE_SPEAKING) {
        mouth = s->voice_level > 0.02f ? s->voice_level
                : 0.35f + 0.35f * sinf(s->time_s * 17.0f) * sinf(s->time_s * 5.3f);
        mouth = mouth < 0 ? 0 : mouth > 1 ? 1 : mouth;
    }
    const pet_pose_t pose = {
        .animal = s->life.animal,
        .mood = mood,
        .time_s = s->time_s,
        .mood_time_s = pet_life_mood_age(&s->life),
        .blink = mood == PET_MOOD_SLEEP ? 0 : s->life.blink,
        .mouth_open = mouth,
        .food_left = pet_life_food_left(&s->life),
        .petting = s->life.petting,
        .roam_x = pet_life_roam_x(&s->life),
        .toy = pet_life_toy(&s->life),
        .toy_x = pet_life_toy_x(&s->life),
        .toy_held = pet_life_toy_held(&s->life),
        .running = pet_life_running(&s->life),
    };
    pet_pose_t frame = pose;
    frame.poop_count = pet_life_poop_count(&s->life);
    {
        static int last_poop = -1;
        if (frame.poop_count != last_poop) {
            ESP_LOGI(TAG_UI, "messes on floor: %d (was %d)", frame.poop_count, last_poop);
            last_poop = frame.poop_count;
        }
    }
    for (uint8_t i = 0; i < frame.poop_count && i < PET_SCENE_POOP_MAX; ++i) {
        frame.poop_x[i] = pet_life_poop_x(&s->life, i);
    }
    pet_scene_build(&s->scene, &frame, pet_rgb565(BACKGROUND));
    /* Full repaint: partial canvas invalidation leaves stale columns in the
     * pinned simulator, and a 240x240 raster is cheap at 20 fps. */
    if (s->clock_open) {
        update_clock(s, ui);
    } else if (!s->radio_open) {
        remember(s, esp_gsp_canvas_invalidate(ui, GSP_PET_BIND_PET_CANVAS));
    }
    if (s->weather_dirty) {
        s->weather_dirty = false;
        remember(s, gsp_pet_clock_weather_set_text(ui, s->weather));
    }
    render_status(s, mood);
}

static void update_clock(struct app_pet_ui *s, esp_gsp_handle_t ui)
{
    const time_t now = time(NULL);
    if (now < 1700000000) { /* network time not set yet */
        remember(s, gsp_pet_clock_time_set_text(ui, "--:--"));
        return;
    }
    const time_t local = now + app_pet_info_utc_offset();
    struct tm tm;
    gmtime_r(&local, &tm);
    if (tm.tm_min != s->clock_shown_min) {
        s->clock_shown_min = tm.tm_min;
        char hm[8], date[24];
        snprintf(hm, sizeof(hm), "%02d:%02d", tm.tm_hour, tm.tm_min);
        strftime(date, sizeof(date), "%a %d %b %Y", &tm);
        remember(s, gsp_pet_clock_time_set_text(ui, hm));
        remember(s, gsp_pet_clock_date_set_text(ui, date));
    }
    if (tm.tm_sec != s->clock_shown_sec) {
        s->clock_shown_sec = tm.tm_sec;
    }
}

static void draw_canvas(const esp_gsp_canvas_surface_t *surface, void *ctx)
{
    const struct app_pet_ui *s = ctx;
    if (surface->pixel_format != ESP_GSP_CANVAS_PIXEL_RGB565) {
        for (uint16_t row = 0; row < surface->height; ++row) {
            memset((uint8_t *)surface->pixels + row * surface->stride_bytes, 0, (size_t)surface->width * 3);
        }
        return;
    }
    pet_scene_raster(&s->scene, surface->pixels, surface->stride_bytes,
                     surface->x, surface->y, surface->width, surface->height);
}

/* ---- touch events ---------------------------------------------------------- */

/* Radio tuning asks the pet's audio through the normal music sink. */
static void radio_tune(const char *station, bool on, void *ctx)
{
    struct app_pet_ui *s = ctx;
    if (s->sink.music) {
        s->sink.music(on, station ? station : "", s->sink.ctx);
    }
}

static void on_event(esp_gsp_handle_t ui, const esp_gsp_event_t *event, void *ctx)
{
    (void)ui;
    struct app_pet_ui *s = ctx;
    pet_life_event_t e;
    if (gsp_pet_event_is_radio_open(event)) {
        s->radio_open = true;
        radio_screen_show(&s->radio, true);
        return;
    }
    if (gsp_pet_event_is_clock_open(event)) {
        s->clock_open = true;
        s->clock_shown_min = -1;
        s->clock_shown_sec = -1;
        remember(s, gsp_pet_clock_screen_set_visible(s->ui, true));
        return;
    }
    if (s->news_open) {
        if (gsp_pet_event_is_news_back(event)) {
            s->news_open = false;
            remember(s, gsp_pet_news_screen_set_visible(s->ui, false));
            remember(s, gsp_pet_clock_screen_set_visible(s->ui, true));
        }
        return;
    }
    if (s->clock_open) {
        if (gsp_pet_event_is_clock_back(event)) {
            s->clock_open = false;
            remember(s, gsp_pet_clock_screen_set_visible(s->ui, false));
        } else if (gsp_pet_event_is_news_open(event)) {
            s->news_open = true;
            remember(s, gsp_pet_clock_screen_set_visible(s->ui, false));
            remember(s, gsp_pet_news_screen_set_visible(s->ui, true));
        }
        return;
    }
    if (s->radio_open) {
        if (gsp_pet_event_is_radio_back(event)) {
            s->radio_open = false;
            radio_screen_show(&s->radio, false);
        } else {
            (void)radio_screen_event(&s->radio, event);
        }
        return;
    }
    if (gsp_pet_event_is_talk_down(event)) {
        talk_begin(s);
    } else if (gsp_pet_event_is_talk_up(event)) {
        talk_end(s);
    } else if (s->setup_open && gsp_pet_event_is_vol_up(event)) {
        if (s->sink.volume_step) {
            s->sink.volume_step(10, s->sink.ctx);
        }
    } else if (s->setup_open && gsp_pet_event_is_vol_down(event)) {
        if (s->sink.volume_step) {
            s->sink.volume_step(-10, s->sink.ctx);
        }
    } else if (s->setup_open && gsp_pet_event_is_setup_close(event)) {
        s->setup_open = false;
        remember(s, gsp_pet_setup_panel_set_visible(s->ui, false));
        if (s->sink.setup) {
            s->sink.setup(false, s->sink.ctx);
        }
    } else if (s->setup_open || s->talking) {
        return;
    } else if (gsp_pet_event_is_pet(event)) {
        pet_life_pet(&s->life, &e);
        apply_event(s, &e, false);
    } else if (gsp_pet_event_is_feed(event)) {
        pet_life_feed(&s->life, &e);
        apply_event(s, &e, true);
    } else if (gsp_pet_event_is_clean_up(event)) {
        pet_life_interact(&s->life);
        const bool cleaned = pet_life_clean(&s->life, &e);
        ESP_LOGI(TAG_UI, "clean up pressed: cleaned=%d", (int)cleaned);
        apply_event(s, &e, true);
    } else if (gsp_pet_event_is_sleep(event)) {
        if (s->life.sleeping) {
            pet_life_wake(&s->life);
            pet_life_interact(&s->life);
            play(s, PET_SOUND_YAWN);
        } else {
            pet_life_sleep(&s->life);
            snprintf(s->line, sizeof(s->line), "Good night, %s.", s->life.name[0] ? s->life.name : "friend");
            say_line(s, s->line, false);
        }
    } else if (gsp_pet_event_is_setup_open(event)) {
        s->setup_open = true;
        remember(s, gsp_pet_setup_panel_set_visible(s->ui, true));
        if (s->sink.setup) {
            s->sink.setup(true, s->sink.ctx);
        }
    }
}

/* ---- inputs from other tasks ------------------------------------------------------ */

#define POST(h, flag, body) do { if (!(h)) return; lock(h); body; (h)->in.flags |= (flag); unlock(h); } while (0)

void app_pet_ui_heard(app_pet_ui_handle_t h, app_pet_talk_kind_t kind, const char *transcript)
{
    POST(h, IN_HEARD, {
        h->in.heard_kind = kind;
        snprintf(h->in.heard, sizeof(h->in.heard), "%s", transcript ? transcript : "");
    });
}

void app_pet_ui_reply(app_pet_ui_handle_t h, const char *raw_reply)
{
    POST(h, IN_REPLY, snprintf(h->in.reply, sizeof(h->in.reply), "%s", raw_reply ? raw_reply : ""));
}

void app_pet_ui_set_phase(app_pet_ui_handle_t h, app_pet_phase_t phase)
{
    POST(h, IN_PHASE, h->in.phase = phase);
}

void app_pet_ui_set_voice_level(app_pet_ui_handle_t h, float level)
{
    POST(h, IN_LEVEL, h->in.level = level < 0 ? 0 : level > 1 ? 1 : level);
}

void app_pet_ui_error(app_pet_ui_handle_t h, const char *message)
{
    POST(h, IN_ERROR, snprintf(h->in.error, sizeof(h->in.error), "%s", message ? message : ""));
}

void app_pet_ui_set_music(app_pet_ui_handle_t h, bool playing)
{
    POST(h, IN_MUSIC, h->in.music = playing);
}

void app_pet_ui_set_identity(app_pet_ui_handle_t h, pet_animal_t animal, const char *name)
{
    POST(h, IN_IDENTITY, {
        h->in.animal = (unsigned)animal < PET_ANIMAL_COUNT ? animal : PET_ANIMAL_DOG;
        h->in.has_name = name != NULL;
        snprintf(h->in.name, sizeof(h->in.name), "%s", name ? name : "");
    });
}

void app_pet_ui_set_setup_text(app_pet_ui_handle_t h, const char *text)
{
    POST(h, IN_SETUP, snprintf(h->in.setup, sizeof(h->in.setup), "%s", text ? text : ""));
}

void app_pet_ui_set_weather(app_pet_ui_handle_t h, const char *line)
{
    POST(h, IN_WEATHER, snprintf(h->in.weather, sizeof(h->in.weather), "%s", line ? line : ""));
}

void app_pet_ui_set_markets(app_pet_ui_handle_t h, const char *rows, const char *news)
{
    POST(h, IN_MARKET, {
        snprintf(h->in.market_rows, sizeof(h->in.market_rows), "%s", rows ? rows : "");
        snprintf(h->in.market_news, sizeof(h->in.market_news), "%s", news ? news : "");
    });
}

void app_pet_ui_set_volume(app_pet_ui_handle_t h, uint8_t volume)
{
    POST(h, IN_VOLUME, h->in.volume = volume > 100 ? 100 : volume);
}

void app_pet_ui_set_talk_on_its_own(app_pet_ui_handle_t h, bool enabled)
{
    POST(h, IN_TALK_OWN, h->in.talk_own = enabled);
}

/* ---- lifecycle --------------------------------------------------------------------- */

esp_gsp_err_t app_pet_ui_create(esp_gsp_handle_t ui, const app_pet_ui_config_t *config,
                                app_pet_ui_handle_t *out_handle)
{
    if (!out_handle) {
        return ESP_GSP_ERR_INVALID_ARG;
    }
    *out_handle = NULL;
    if (!ui || !config) {
        return ESP_GSP_ERR_INVALID_ARG;
    }
    struct app_pet_ui *s = calloc(1, sizeof(*s));
    if (!s) {
        return ESP_GSP_ERR_NO_MEM;
    }
    s->ui = ui;
    s->sink = config->sink;
    s->lock = config->lock;
    radio_screen_init(&s->radio, ui, radio_tune, s);
    s->talk_own = config->talk_on_its_own;
    s->shown_mood = PET_MOOD_COUNT;
    s->shown_tummy = s->shown_sleep = s->shown_music = -1;
    const pet_animal_t animal = (unsigned)config->animal < PET_ANIMAL_COUNT ? config->animal : PET_ANIMAL_DOG;
    pet_life_init(&s->life, animal, config->name, config->seed);

    render_greeting(s);
    set_talk_visual(s, false);
    remember(s, gsp_pet_setup_panel_set_visible(ui, false));
    render_status(s, pet_life_mood(&s->life));

    s->news_count = 1;
    snprintf(s->news_items[0], NEWS_ITEM_MAX, "Loading headlines...");
    s->news_list = esp_gsp_list_bind_component(ui, GSP_PET_OBJ_KEY_HEADLINE_LIST, bind_headline, s);
    if (s->news_list != ESP_GSP_LIST_NONE) {
        (void)esp_gsp_list_set_total(ui, s->news_list, s->news_count);
    }
    esp_gsp_err_t err = s->news_list == ESP_GSP_LIST_NONE ? ESP_GSP_ERR_NO_MEM : s->last_error;
    if (err == ESP_GSP_OK) {
        err = esp_gsp_canvas_set_draw_cb(ui, GSP_PET_BIND_PET_CANVAS, draw_canvas, s);
    }
    if (err == ESP_GSP_OK) {
        s->frame_timer = esp_gsp_timer_create(ui, FRAME_MS, frame_tick, s);
        err = s->frame_timer ? ESP_GSP_OK : ESP_GSP_ERR_NO_MEM;
    }
    if (err == ESP_GSP_OK) {
        /* Publish only a fully initialized context; dispatch may start now. */
        err = esp_gsp_on_event(ui, on_event, s);
    }
    if (err != ESP_GSP_OK) {
        if (s->frame_timer) {
            (void)esp_gsp_timer_delete(ui, s->frame_timer);
        }
        (void)esp_gsp_canvas_stop(ui, GSP_PET_BIND_PET_CANVAS);
        (void)esp_gsp_flush(ui, 250);
        free(s);
        return err;
    }
    if (!s->life.name[0]) {
        start_onboarding(s);
    } else {
        play(s, PET_SOUND_GREET);
        snprintf(s->line, sizeof(s->line), "Hi %s! I missed you.", s->life.name);
        pet_life_react(&s->life, PET_MOOD_HAPPY, 4);
        say_line(s, s->line, true);
    }
    *out_handle = s;
    return ESP_GSP_OK;
}

esp_gsp_err_t app_pet_ui_destroy(app_pet_ui_handle_t *handle)
{
    if (!handle) {
        return ESP_GSP_ERR_INVALID_ARG;
    }
    struct app_pet_ui *s = *handle;
    if (!s) {
        return ESP_GSP_OK;
    }
    esp_gsp_err_t err = esp_gsp_on_event(s->ui, NULL, NULL);
    if (err == ESP_GSP_OK && s->frame_timer) {
        err = esp_gsp_timer_delete(s->ui, s->frame_timer);
    }
    if (err == ESP_GSP_OK) {
        err = esp_gsp_canvas_stop(s->ui, GSP_PET_BIND_PET_CANVAS);
    }
    if (err != ESP_GSP_OK) {
        return err;
    }
    free(s);
    *handle = NULL;
    return ESP_GSP_OK;
}
