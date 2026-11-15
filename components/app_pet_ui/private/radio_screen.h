// SPDX-License-Identifier: Apache-2.0
#pragma once

/* Radio screen inside the pet app: a full-screen group the pet scene shows
 * when the owner taps Radio. Owns only tuning state; audio is requested
 * through the tune callback. Single owner: the GSP render task. */

#include <stdbool.h>
#include "esp_gsp.h"
#include "radio_tuner.h"

typedef struct {
    esp_gsp_handle_t ui;
    radio_tuner_t tuner;
    bool music_on;
    int shown_locked;
    uint16_t shown_freq;
    /* station is NULL when the radio should stop. */
    void (*tune)(const char *station, bool on, void *ctx);
    void *ctx;
} radio_screen_t;

void radio_screen_init(radio_screen_t *r, esp_gsp_handle_t ui, void (*tune)(const char *, bool, void *), void *ctx);
/** Show the radio group and redraw from the current state. */
void radio_screen_show(radio_screen_t *r, bool visible);
/** Handle a radio touch event. Returns true when the event was a radio event. */
bool radio_screen_event(radio_screen_t *r, const esp_gsp_event_t *event);
/** Music state reported by the audio service. */
void radio_screen_set_music(radio_screen_t *r, bool on);
