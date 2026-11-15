// SPDX-License-Identifier: Apache-2.0
#include "radio_screen.h"

#include <stdio.h>
#include <string.h>

#include "app_stations.h"
#include "bundle_gsp.h"

#define CHIP_COUNT 5
#define CHIP_CENTER 2
#define COLOR_PILL_PLAYING 0x3B6D11u
#define COLOR_PILL_IDLE 0x3A3935u
#define COLOR_PILL_BUSY 0x854F0Bu

static int wrap_station(int index)
{
    const int n = (int)app_stations_count();
    return ((index % n) + n) % n;
}

static void format_freq(char *buf, size_t size, uint16_t tenths)
{
    snprintf(buf, size, "%u.%u", (unsigned)(tenths / 10), (unsigned)(tenths % 10));
}

static void render_chips(radio_screen_t *r)
{
    static esp_gsp_err_t (*const chip_text[CHIP_COUNT])(esp_gsp_handle_t, const char *) = {
        gsp_pet_chip_0_text_set_text, gsp_pet_chip_1_text_set_text, gsp_pet_chip_2_text_set_text,
        gsp_pet_chip_3_text_set_text, gsp_pet_chip_4_text_set_text,
    };
    const int anchor = r->tuner.locked != APP_STATIONS_NONE ? r->tuner.locked
                       : app_stations_nearest(r->tuner.freq_tenths);
    char buf[8];
    for (int i = 0; i < CHIP_COUNT; ++i) {
        const app_station_t *st = app_stations_get((size_t)wrap_station(anchor - CHIP_CENTER + i));
        format_freq(buf, sizeof(buf), st->freq_tenths);
        (void)chip_text[i](r->ui, buf);
        if (i == CHIP_CENTER) {
            (void)gsp_pet_chip_lock_text_set_text(r->ui, buf);
        }
    }
    (void)gsp_pet_chip_lock_set_visible(r->ui, r->tuner.locked != APP_STATIONS_NONE);
}

static void render_status(radio_screen_t *r)
{
    const char *text = "Searching";
    uint32_t color = COLOR_PILL_IDLE;
    if (r->tuner.locked != APP_STATIONS_NONE) {
        text = r->music_on ? "Playing" : "Tuned, stopped";
        color = r->music_on ? COLOR_PILL_PLAYING : COLOR_PILL_BUSY;
    }
    (void)gsp_pet_status_set_text(r->ui, text);
    (void)gsp_pet_status_pill_set_color_rgb888(r->ui, color);
    (void)gsp_pet_pause_icon_set_visible(r->ui, r->music_on);
    (void)gsp_pet_play_icon_set_visible(r->ui, !r->music_on);
}

static void render_dial(radio_screen_t *r)
{
    const radio_tuner_t *t = &r->tuner;
    if (t->freq_tenths == r->shown_freq && t->locked == r->shown_locked) {
        return;
    }
    char buf[8];
    format_freq(buf, sizeof(buf), t->freq_tenths);
    (void)gsp_pet_freq_set_text(r->ui, buf);
    (void)gsp_pet_dial_strip_set_x(r->ui, radio_tuner_dial_x(t->freq_tenths));
    if (t->locked != r->shown_locked) {
        const app_station_t *st = t->locked == APP_STATIONS_NONE ? NULL : app_stations_get((size_t)t->locked);
        (void)gsp_pet_station_name_set_text(r->ui, st ? st->name : "No station");
        (void)gsp_pet_tagline_set_text(r->ui, st ? st->tagline : "Static - keep sweeping");
    }
    render_chips(r);
    r->shown_freq = t->freq_tenths;
    r->shown_locked = t->locked;
    render_status(r);
}

/* Start or stop audio for the committed station. */
static void commit(radio_screen_t *r)
{
    render_dial(r);
    if (radio_tuner_commit(&r->tuner)) {
        const app_station_t *st = r->tuner.committed == APP_STATIONS_NONE
                                  ? NULL : app_stations_get((size_t)r->tuner.committed);
        r->music_on = st != NULL;
        if (r->tune) {
            r->tune(st ? st->name : NULL, st != NULL, r->ctx);
        }
    }
    render_status(r);
}

void radio_screen_init(radio_screen_t *r, esp_gsp_handle_t ui, void (*tune)(const char *, bool, void *), void *ctx)
{
    memset(r, 0, sizeof(*r));
    r->ui = ui;
    r->tune = tune;
    r->ctx = ctx;
    r->shown_locked = -2;
    radio_tuner_init(&r->tuner, 950, 60);
    r->tuner.committed = APP_STATIONS_NONE;
}

void radio_screen_show(radio_screen_t *r, bool visible)
{
    (void)gsp_pet_radio_screen_set_visible(r->ui, visible);
    if (visible) {
        render_dial(r);
    }
}

void radio_screen_set_music(radio_screen_t *r, bool on)
{
    r->music_on = on;
    render_status(r);
}

bool radio_screen_event(radio_screen_t *r, const esp_gsp_event_t *event)
{
    if (gsp_pet_event_is_dial_jog(event)) {
        radio_tuner_jog(&r->tuner, (int32_t)event->arg);
        render_dial(r);
    } else if (gsp_pet_event_is_dial_release(event)) {
        radio_tuner_drag_end(&r->tuner);
        commit(r);
    } else if (gsp_pet_event_is_seek_up(event)) {
        radio_tuner_seek(&r->tuner, +1);
        commit(r);
    } else if (gsp_pet_event_is_seek_down(event)) {
        radio_tuner_seek(&r->tuner, -1);
        commit(r);
    } else if (gsp_pet_event_is_chip(event)) {
        const int anchor = r->tuner.locked != APP_STATIONS_NONE ? r->tuner.locked
                           : app_stations_nearest(r->tuner.freq_tenths);
        radio_tuner_select(&r->tuner, wrap_station(anchor - CHIP_CENTER + (int)event->arg));
        commit(r);
    } else if (gsp_pet_event_is_play_pause(event)) {
        if (r->music_on) {
            r->music_on = false;
            r->tuner.committed = APP_STATIONS_NONE;
            if (r->tune) {
                r->tune(NULL, false, r->ctx);
            }
        } else {
            const int idx = r->tuner.locked != APP_STATIONS_NONE ? r->tuner.locked
                            : app_stations_nearest(r->tuner.freq_tenths);
            radio_tuner_select(&r->tuner, idx);
            r->tuner.committed = APP_STATIONS_NONE; /* so commit() starts it */
            commit(r);
        }
        render_status(r);
    } else {
        return false;
    }
    return true;
}
