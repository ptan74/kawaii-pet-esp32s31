// SPDX-License-Identifier: Apache-2.0
#include "pet_life.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* Need dynamics (per second). Tuned so a pet left alone gets hungry in about
 * three hours, sleepy in about four, and bored within a few minutes. */
#define HUNGER_DRAIN (1.0f / 10800.0f)
#define ENERGY_DRAIN (1.0f / 14400.0f)
#define ENERGY_REST (1.0f / 1200.0f)
#define LONELY_RISE (1.0f / 600.0f)

#define HUNGRY_BELOW 0.30f
#define SLEEP_BELOW 0.15f
#define NAP_IDLE_S 600.0f
#define NAP_ENERGY_BELOW 0.55f
#define BORED_ABOVE 0.6f

#define EAT_SECONDS 4.0f
#define SNORE_PERIOD_S 3.6f
#define CHATTER_MIN_S 90.0f
#define CHATTER_SPAN_S 120.0f

/* Play episodes: start after idling, run to a toy, fetch it, repeat, then go home. */
#define ROAM_IDLE_S 8.0f
#define ROAM_LEN_S 18.0f
#define ROAM_SPEED 55.0f      /* art units per second */
#define ROAM_REACH 70.0f      /* how far from centre the toy lands */
#define ROAM_WANDER_REACH 95.0f /* how far a plain run-about goes */
#define ROAM_WANDER_CHANCE 0.4f /* share of episodes that are plain run-abouts */
#define ROAM_HOLD_S 1.0f
#define ROAM_WANDER_PAUSE_S 1.5f
#define ROAM_COOL_MIN_S 15.0f
#define ROAM_COOL_SPAN_S 25.0f
#define ROAM_FIRST_S 10.0f

/* Messes: made now and then while awake, until someone cleans them up. */
#define POOP_FIRST_S 60.0f
#define POOP_MIN_S 300.0f
#define POOP_SPAN_S 300.0f

enum { ROAM_OFF = 0, ROAM_TO_TOY, ROAM_HOLD, ROAM_RETREAT };

static float frand(pet_life_t *l)
{
    l->rng = l->rng * 1664525u + 1013904223u;
    return (float)(l->rng >> 8) / 16777216.0f;
}

static const char *who(const pet_life_t *l)
{
    return l->name[0] ? l->name : "friend";
}

#define SAY(e, ...) snprintf((e)->say, sizeof((e)->say), __VA_ARGS__)

static void roam_end(pet_life_t *l)
{
    if (l->roam_state == ROAM_OFF || l->roam_state == ROAM_RETREAT) {
        return;
    }
    l->roam_state = ROAM_RETREAT;
    l->toy = PET_TOY_NONE;
    l->toy_held = false;
    l->roam_target = 0;
}

static void move_toward(pet_life_t *l, float dt)
{
    const float d = l->roam_target - l->roam_x;
    const float step = ROAM_SPEED * dt;
    if (fabsf(d) <= step) {
        l->roam_x = l->roam_target;
    } else {
        l->roam_x += d > 0 ? step : -step;
    }
}

static bool roam_eligible(const pet_life_t *l)
{
    return !l->busy && !l->sleeping && !l->music && l->eat_left_s <= 0 && l->temp_left_s <= 0 &&
           l->energy > 0.4f && l->hunger > 0.3f && l->idle_s > ROAM_IDLE_S;
}

static void roam_start(pet_life_t *l, pet_life_event_t *e)
{
    l->roam_state = ROAM_TO_TOY;
    l->roam_left_s = ROAM_LEN_S;
    l->toy_held = false;
    e->sound = PET_SOUND_GREET;
    if (frand(l) < ROAM_WANDER_CHANCE) {
        /* Plain run-about: no toy, just a lap of the room and back. */
        l->toy = PET_TOY_NONE;
        l->roam_target = (frand(l) * 2.0f - 1.0f) * ROAM_WANDER_REACH;
        SAY(e, "Let me stretch my legs, %s!", who(l));
        return;
    }
    l->toy = frand(l) < 0.5f ? PET_TOY_BALL : PET_TOY_BONE;
    l->toy_x = (frand(l) < 0.5f ? -1.0f : 1.0f) * ROAM_REACH;
    l->roam_target = l->toy_x;
    SAY(e, "Fetch, %s! Watch me go!", who(l));
}

/* Make a mess where the pet stands, now and then, while awake. */
static void poop_tick(pet_life_t *l, float dt, pet_life_event_t *e)
{
    if (l->sleeping || l->busy || l->poop_count >= PET_POOP_MAX) {
        return;
    }
    l->poop_timer_s -= dt;
    if (l->poop_timer_s > 0) {
        return;
    }
    l->poop_timer_s = POOP_MIN_S + POOP_SPAN_S * frand(l);
    l->poop_x[l->poop_count++] = l->roam_x;
    if (e->say[0] == '\0') {
        SAY(e, "Oops, I made a mess, %s! Could you clean it up?", who(l));
    }
}

/* Called once per tick while awake and not talking; may start an episode. */
static void roam_tick(pet_life_t *l, float dt, pet_life_event_t *e)
{
    if (l->roam_state == ROAM_OFF) {
        if (roam_eligible(l) && (l->roam_cool_s -= dt) <= 0) {
            l->roam_cool_s = ROAM_COOL_MIN_S + ROAM_COOL_SPAN_S * frand(l);
            roam_start(l, e);
        }
        return;
    }
    if (l->roam_state == ROAM_RETREAT) {
        move_toward(l, dt);
        if (fabsf(l->roam_x) < 0.5f) {
            l->roam_x = 0;
            l->roam_state = ROAM_OFF;
        }
        return;
    }
    l->roam_left_s -= dt;
    if (l->roam_left_s <= 0) {
        roam_end(l);
        return;
    }
    if (l->roam_state == ROAM_HOLD) {
        l->roam_hold_s -= dt;
        if (l->roam_hold_s <= 0 && l->toy == PET_TOY_NONE) {
            roam_end(l);
            return;
        }
        if (l->roam_hold_s <= 0) {
            /* Throw it back to the far side. */
            l->toy_held = false;
            l->toy_x = (l->roam_x < 0 ? 1.0f : -1.0f) * ROAM_REACH;
            l->roam_target = l->toy_x;
            l->roam_state = ROAM_TO_TOY;
        }
        return;
    }
    move_toward(l, dt);
    if (l->roam_x == l->roam_target) {
        /* Caught it (or reached the far end of a run-about): pause, then go on. */
        l->toy_held = l->toy != PET_TOY_NONE;
        l->toy_x = l->roam_x;
        l->roam_state = ROAM_HOLD;
        l->roam_hold_s = l->toy != PET_TOY_NONE ? ROAM_HOLD_S : ROAM_WANDER_PAUSE_S;
    }
}

void pet_life_init(pet_life_t *l, pet_animal_t animal, const char *name, uint32_t seed)
{
    memset(l, 0, sizeof(*l));
    l->animal = animal;
    pet_life_set_name(l, name);
    l->hunger = 0.8f;
    l->energy = 0.9f;
    l->temp_mood = PET_MOOD_COUNT;
    l->shown_mood = PET_MOOD_COUNT;
    l->rng = seed ? seed : 0x1234567u;
    l->next_blink_s = 2.0f;
    l->next_sound_s = 8.0f;
    l->next_chatter_s = CHATTER_MIN_S;
    l->roam_cool_s = ROAM_FIRST_S;
    l->poop_timer_s = POOP_FIRST_S;
}

void pet_life_set_animal(pet_life_t *l, pet_animal_t animal)
{
    l->animal = animal;
}

void pet_life_set_name(pet_life_t *l, const char *name)
{
    snprintf(l->name, sizeof(l->name), "%s", name ? name : "");
}

pet_mood_t pet_life_mood(const pet_life_t *l)
{
    if (l->eat_left_s > 0) {
        return PET_MOOD_EAT;
    }
    if (l->temp_left_s > 0 && l->temp_mood < PET_MOOD_COUNT) {
        return l->temp_mood;
    }
    if (l->sleeping) {
        return PET_MOOD_SLEEP;
    }
    if (l->roam_state == ROAM_TO_TOY || l->roam_state == ROAM_RETREAT) {
        return PET_MOOD_PLAY;
    }
    if (l->roam_state == ROAM_HOLD) {
        return PET_MOOD_HAPPY;
    }
    if (l->music) {
        return PET_MOOD_DANCE;
    }
    if (l->hunger < HUNGRY_BELOW) {
        return PET_MOOD_HUNGRY;
    }
    if (l->lonely > BORED_ABOVE) {
        return l->lonely > 0.9f ? PET_MOOD_SAD : PET_MOOD_BORED;
    }
    return l->energy < 0.3f ? PET_MOOD_SMILE : PET_MOOD_HAPPY;
}

float pet_life_mood_age(const pet_life_t *l)
{
    return l->mood_age_s;
}

float pet_life_food_left(const pet_life_t *l)
{
    return l->eat_left_s > 0 ? l->eat_left_s / EAT_SECONDS : 0.0f;
}

void pet_life_react(pet_life_t *l, pet_mood_t mood, float seconds)
{
    if ((unsigned)mood >= PET_MOOD_COUNT) {
        return;
    }
    if (mood == PET_MOOD_SLEEP) {
        pet_life_sleep(l);
        return;
    }
    l->temp_mood = mood;
    l->temp_left_s = seconds;
}

void pet_life_interact(pet_life_t *l)
{
    roam_end(l);
    l->idle_s = 0;
    l->lonely = 0;
    if (l->sleeping) {
        pet_life_wake(l);
    }
}

void pet_life_feed(pet_life_t *l, pet_life_event_t *e)
{
    pet_life_interact(l);
    memset(e, 0, sizeof(*e));
    if (l->hunger > 0.95f) {
        l->temp_mood = PET_MOOD_SHY;
        l->temp_left_s = 2.5f;
        e->sound = PET_SOUND_HAPPY;
        SAY(e, "I'm already full, %s. Maybe later!", who(l));
        return;
    }
    l->eat_left_s = EAT_SECONDS;
    l->hunger = 1.0f;
    l->next_sound_s = 1.0f;
    e->sound = PET_SOUND_EAT;
    SAY(e, "Yummy %s! Thank you, %s.", pet_animal_food(l->animal), who(l));
}

void pet_life_sleep(pet_life_t *l)
{
    roam_end(l);
    l->sleeping = true;
    l->temp_left_s = 0;
    l->next_sound_s = 1.5f;
}

void pet_life_wake(pet_life_t *l)
{
    if (!l->sleeping) {
        return;
    }
    l->sleeping = false;
    l->temp_mood = PET_MOOD_SURPRISED;
    l->temp_left_s = 1.2f;
    l->idle_s = 0;
}

void pet_life_pet(pet_life_t *l, pet_life_event_t *e)
{
    const bool was_sleeping = l->sleeping;
    pet_life_interact(l);
    memset(e, 0, sizeof(*e));
    l->petting = 1.0f;
    if (was_sleeping) {
        e->sound = PET_SOUND_YAWN;
        SAY(e, "Mmm... oh, hi %s! I was having a lovely nap.", who(l));
        return;
    }
    l->temp_mood = frand(l) < 0.5f ? PET_MOOD_LOVE : PET_MOOD_SHY;
    l->temp_left_s = 2.5f;
    e->sound = PET_SOUND_LOVE;
}

void pet_life_set_music(pet_life_t *l, bool playing)
{
    l->music = playing;
    if (playing) {
        pet_life_interact(l);
    }
}

void pet_life_set_busy(pet_life_t *l, bool busy)
{
    l->busy = busy;
    if (busy) {
        pet_life_interact(l);
    }
}

/* Spontaneous lines, chosen from the strongest need. */
static void chatter(pet_life_t *l, pet_life_event_t *e)
{
    const char *n = who(l);
    const float r = frand(l);
    if (l->hunger < HUNGRY_BELOW) {
        e->sound = PET_SOUND_SAD;
        if (r < 0.5f) {
            SAY(e, "%s, my tummy is rumbling. Can I have some %s?", n, pet_animal_food(l->animal));
        } else {
            SAY(e, "I'm so hungry, %s. Some %s would be perfect.", n, pet_animal_food(l->animal));
        }
    } else if (l->lonely > BORED_ABOVE) {
        e->sound = PET_SOUND_SAD;
        if (r < 0.5f) {
            SAY(e, "I'm bored, %s. Talk to me?", n);
        } else {
            SAY(e, "Hey %s, do you want to play with me?", n);
        }
    } else if (l->energy < 0.3f) {
        e->sound = PET_SOUND_YAWN;
        SAY(e, "Yaaawn... I'm getting sleepy, %s.", n);
    } else {
        e->sound = PET_SOUND_GREET;
        switch ((unsigned)(r * 4) & 3) {
        case 0: SAY(e, "I'm happy you're here, %s.", n); break;
        case 1: SAY(e, "Did you know you're my favourite human, %s?", n); break;
        case 2: SAY(e, "Let's have fun today, %s!", n); break;
        default: SAY(e, "Pat my head, %s. I love that.", n); break;
        }
    }
}

bool pet_life_tick(pet_life_t *l, float dt, pet_life_event_t *e)
{
    memset(e, 0, sizeof(*e));
    if (dt <= 0) {
        return false;
    }
    if (dt > 5.0f) {
        dt = 5.0f; /* resumed after a stall: don't fast-forward needs */
    }

    /* Needs. */
    l->hunger -= HUNGER_DRAIN * dt;
    l->hunger = l->hunger < 0 ? 0 : l->hunger;
    if (l->sleeping) {
        l->energy += ENERGY_REST * dt;
        if (l->energy >= 1.0f) {
            l->energy = 1.0f;
            pet_life_wake(l);
            e->sound = PET_SOUND_YAWN;
            SAY(e, "Good morning, %s! I feel so refreshed.", who(l));
        }
    } else {
        l->energy -= ENERGY_DRAIN * dt;
        l->energy = l->energy < 0 ? 0 : l->energy;
    }
    if (!l->busy && !l->music) {
        l->idle_s += dt;
        l->lonely += LONELY_RISE * dt;
        l->lonely = l->lonely > 1 ? 1 : l->lonely;
    }

    /* Timers. */
    if (l->temp_left_s > 0) {
        l->temp_left_s -= dt;
    }
    if (l->eat_left_s > 0) {
        l->eat_left_s -= dt;
        if (l->eat_left_s <= 0) {
            l->temp_mood = PET_MOOD_LOVE;
            l->temp_left_s = 2.5f;
            e->sound = PET_SOUND_LOVE;
        }
    }
    if (l->petting > 0) {
        l->petting -= dt / 1.5f;
        l->petting = l->petting < 0 ? 0 : l->petting;
    }

    /* Falling asleep on its own. */
    if (!l->sleeping && !l->busy && !l->music && l->eat_left_s <= 0 &&
            (l->energy < SLEEP_BELOW || (l->idle_s > NAP_IDLE_S && l->energy < NAP_ENERGY_BELOW))) {
        pet_life_sleep(l);
        e->sound = PET_SOUND_YAWN;
        SAY(e, "I'm going to take a little nap, %s.", who(l));
    }

    /* Mood age for jump/fx timing. */
    const pet_mood_t mood = pet_life_mood(l);
    if (mood != l->shown_mood) {
        l->shown_mood = mood;
        l->mood_age_s = 0;
    } else {
        l->mood_age_s += dt;
    }

    /* Blinking: quick close/open every 2-6 s while awake. */
    l->next_blink_s -= dt;
    if (l->next_blink_s <= 0) {
        l->blink = 1.0f;
        l->next_blink_s = 2.0f + 4.0f * frand(l);
        if (frand(l) < 0.2f) {
            l->next_blink_s = 0.25f; /* occasional double blink */
        }
    } else if (l->blink > 0) {
        l->blink -= dt / 0.15f;
        l->blink = l->blink < 0 ? 0 : l->blink;
    }

    /* Ambient sounds: crunching while eating, snoring while asleep. */
    l->next_sound_s -= dt;
    if (l->next_sound_s <= 0 && e->sound == PET_SOUND_NONE && !l->busy) {
        if (l->eat_left_s > 0) {
            e->sound = PET_SOUND_EAT;
            l->next_sound_s = 1.1f;
        } else if (l->sleeping) {
            e->sound = PET_SOUND_SNORE;
            l->next_sound_s = SNORE_PERIOD_S + frand(l) * 0.8f;
        } else {
            l->next_sound_s = 20.0f + 25.0f * frand(l);
            if (!l->music && frand(l) < 0.5f) {
                e->sound = pet_mood_sound(mood);
            }
        }
    }

    poop_tick(l, dt, e);

    /* Play episodes only start when nothing else is being said. */
    if (e->say[0] == '\0') {
        roam_tick(l, dt, e);
    }

    /* Spontaneous chatter. */
    l->next_chatter_s -= dt;
    if (l->next_chatter_s <= 0) {
        l->next_chatter_s = CHATTER_MIN_S + CHATTER_SPAN_S * frand(l);
        if (!l->busy && !l->sleeping && !l->music && l->eat_left_s <= 0 && e->say[0] == '\0') {
            chatter(l, e);
        }
    }
    return e->sound != PET_SOUND_NONE || e->say[0] != '\0';
}

float pet_life_roam_x(const pet_life_t *l)
{
    return l->roam_x;
}

bool pet_life_running(const pet_life_t *l)
{
    return l->roam_state == ROAM_TO_TOY || l->roam_state == ROAM_RETREAT;
}

pet_toy_t pet_life_toy(const pet_life_t *l)
{
    return l->toy;
}

float pet_life_toy_x(const pet_life_t *l)
{
    return l->toy_x;
}

bool pet_life_toy_held(const pet_life_t *l)
{
    return l->toy_held;
}

uint8_t pet_life_poop_count(const pet_life_t *l)
{
    return l->poop_count;
}

float pet_life_poop_x(const pet_life_t *l, uint8_t index)
{
    return index < l->poop_count ? l->poop_x[index] : 0.0f;
}

bool pet_life_clean(pet_life_t *l, pet_life_event_t *e)
{
    memset(e, 0, sizeof(*e));
    if (l->poop_count == 0) {
        SAY(e, "Nothing to clean up, %s. I'm already tidy!", who(l));
        return false;
    }
    l->poop_count = 0;
    l->poop_timer_s = POOP_MIN_S + POOP_SPAN_S * frand(l);
    e->sound = PET_SOUND_GREET;
    SAY(e, "All clean, thank you, %s!", who(l));
    pet_life_react(l, PET_MOOD_HAPPY, 3.0f);
    return true;
}
