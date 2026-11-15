// SPDX-License-Identifier: Apache-2.0
#pragma once

/* Shared vocabulary for the kawaii pet: animals, moods and sounds. Portable C. */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PET_ANIMAL_DOG = 0,
    PET_ANIMAL_CAT,
    PET_ANIMAL_RABBIT,
    PET_ANIMAL_HAMSTER,
    PET_ANIMAL_PANDA,
    PET_ANIMAL_PENGUIN,
    PET_ANIMAL_COUNT,
} pet_animal_t;

typedef enum {
    PET_MOOD_HAPPY = 0,
    PET_MOOD_SMILE,
    PET_MOOD_LAUGH,
    PET_MOOD_LOVE,
    PET_MOOD_SHY,
    PET_MOOD_SURPRISED,
    PET_MOOD_SCARED,
    PET_MOOD_ANGRY,
    PET_MOOD_SAD,
    PET_MOOD_BORED,
    PET_MOOD_HUNGRY,
    PET_MOOD_EAT,
    PET_MOOD_DANCE,
    PET_MOOD_SLEEP,
    PET_MOOD_LISTEN,
    PET_MOOD_THINK,
    PET_MOOD_PLAY,      /**< Running after a toy. */
    PET_MOOD_CURIOUS,
    PET_MOOD_PROUD,
    PET_MOOD_SLEEPY,
    PET_MOOD_CONFUSED,
    PET_MOOD_COUNT,
} pet_mood_t;

/** Toy the pet plays with while roaming. */
typedef enum {
    PET_TOY_NONE = 0,
    PET_TOY_BALL,
    PET_TOY_BONE,
} pet_toy_t;

/** Short procedurally synthesized effects (see pet_synth.h). */
typedef enum {
    PET_SOUND_NONE = 0,
    PET_SOUND_GREET,   /**< Bark / meow / squeak / honk, per animal. */
    PET_SOUND_HAPPY,   /**< Pant, purr, trill... */
    PET_SOUND_LAUGH,
    PET_SOUND_ANGRY,   /**< Growl / hiss. */
    PET_SOUND_SAD,     /**< Whimper / mew. */
    PET_SOUND_EAT,     /**< Crunch-crunch. */
    PET_SOUND_SNORE,
    PET_SOUND_YAWN,
    PET_SOUND_SURPRISE,
    PET_SOUND_LOVE,
    PET_SOUND_COUNT,
} pet_sound_t;

/** Lower-case identifier ("dog"); NULL when out of range. */
const char *pet_animal_id(pet_animal_t animal);
/** Display name ("Dog"). */
const char *pet_animal_name(pet_animal_t animal);
/** Favourite food ("bone"). */
const char *pet_animal_food(pet_animal_t animal);
/** Parse an identifier or name, case-insensitive; returns COUNT when unknown. */
pet_animal_t pet_animal_parse(const char *text);

/** Lower-case identifier ("happy"). */
const char *pet_mood_id(pet_mood_t mood);
/** Display label ("Happy"). */
const char *pet_mood_label(pet_mood_t mood);
/** Parse an identifier or common synonym; returns COUNT when unknown. */
pet_mood_t pet_mood_parse(const char *text);
/** Sound that suits a mood, or PET_SOUND_NONE. */
pet_sound_t pet_mood_sound(pet_mood_t mood);

#ifdef __cplusplus
}
#endif
