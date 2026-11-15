// SPDX-License-Identifier: Apache-2.0
#pragma once

/* "Alive" behaviour: needs (hunger, energy, company), autonomous moods,
 * blinking, eating and sleeping, and things the pet says on its own.
 * Portable and deterministic for a given seed; single owner (the UI task). */

#include <stdbool.h>
#include <stdint.h>
#include "pet_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PET_LIFE_SAY_MAX 128
#define PET_NAME_MAX 32
#define PET_POOP_MAX 3   /**< Messes on the floor at once; more are not made until cleaned. */

typedef struct {
    pet_sound_t sound;               /**< PET_SOUND_NONE when nothing to play. */
    char say[PET_LIFE_SAY_MAX];      /**< Spontaneous line, "" when none. */
} pet_life_event_t;

typedef struct {
    pet_animal_t animal;
    char name[PET_NAME_MAX];   /**< Owner's name, "" until known. */
    float hunger;              /**< 1 full .. 0 starving. */
    float energy;              /**< 1 rested .. 0 exhausted. */
    float lonely;              /**< 0 just played .. 1 ignored for long. */
    bool sleeping;
    bool music;                /**< Dancing while music plays. */
    bool busy;                 /**< Listening/thinking/talking: no autonomy. */
    pet_mood_t temp_mood;      /**< Short-lived mood, or PET_MOOD_COUNT. */
    float temp_left_s;
    pet_mood_t shown_mood;     /**< Last mood returned by pet_life_mood(). */
    float mood_age_s;
    float eat_left_s;
    float petting;
    float blink;               /**< Current eyelid, 0..1. */
    float next_blink_s;
    float next_sound_s;
    float next_chatter_s;
    float idle_s;              /**< Seconds since the user last interacted. */
    float roam_cool_s;         /**< Idle seconds until the next play episode. */
    float roam_left_s;         /**< Time left in the current episode. */
    float roam_x;              /**< Pet offset from centre, art units. */
    float roam_target;
    float roam_hold_s;         /**< Pause after catching the toy. */
    float toy_x;
    pet_toy_t toy;
    bool toy_held;
    int roam_state;            /**< ROAM_* in pet_life.c. */
    float poop_timer_s;        /**< Seconds until the next mess while awake. */
    uint8_t poop_count;        /**< Messes on the floor, 0..PET_POOP_MAX. */
    float poop_x[PET_POOP_MAX]; /**< Where each mess lies, art units from centre. */
    uint32_t rng;
} pet_life_t;

/** Start awake, fed and rested. seed varies idle behaviour. */
void pet_life_init(pet_life_t *life, pet_animal_t animal, const char *name, uint32_t seed);

/** Advance by dt seconds; fills *event (sound and/or line) and returns true
 * when the pet does something worth playing or showing. */
bool pet_life_tick(pet_life_t *life, float dt, pet_life_event_t *event);

/** Mood to draw right now (temp mood > eating > sleep > needs > baseline). */
pet_mood_t pet_life_mood(const pet_life_t *life);
/** Seconds the current mood has been shown. */
float pet_life_mood_age(const pet_life_t *life);
/** Bowl contents while eating, 0..1. */
float pet_life_food_left(const pet_life_t *life);

void pet_life_set_animal(pet_life_t *life, pet_animal_t animal);
void pet_life_set_name(pet_life_t *life, const char *name);
/** Show mood for seconds (reply emotions, reactions). */
void pet_life_react(pet_life_t *life, pet_mood_t mood, float seconds);
/** Any touch or speech: wakes the pet and resets loneliness. */
void pet_life_interact(pet_life_t *life);
/** Start eating; fills *event with the first crunch and thank-you line. */
void pet_life_feed(pet_life_t *life, pet_life_event_t *event);
void pet_life_sleep(pet_life_t *life);
void pet_life_wake(pet_life_t *life);
/** Head stroke: hearts and a happy sound in *event. */
void pet_life_pet(pet_life_t *life, pet_life_event_t *event);
void pet_life_set_music(pet_life_t *life, bool playing);
void pet_life_set_busy(pet_life_t *life, bool busy);

/* Play episodes: the pet runs about and fetches a toy when left idle. Offsets
 * are in art units from the centre (220 units wide). */
float pet_life_roam_x(const pet_life_t *life);
bool pet_life_running(const pet_life_t *life);
pet_toy_t pet_life_toy(const pet_life_t *life);
float pet_life_toy_x(const pet_life_t *life);
bool pet_life_toy_held(const pet_life_t *life);

/** Messes the pet has left: count and each position (art units from centre). */
uint8_t pet_life_poop_count(const pet_life_t *life);
float pet_life_poop_x(const pet_life_t *life, uint8_t index);
/** Clean up every mess. Returns false when there was nothing to clean. */
bool pet_life_clean(pet_life_t *life, pet_life_event_t *event);

#ifdef __cplusplus
}
#endif
