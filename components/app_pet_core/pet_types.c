// SPDX-License-Identifier: Apache-2.0
#include "pet_types.h"

#include <ctype.h>
#include <stddef.h>
#include <string.h>

static const struct {
    const char *id;
    const char *name;
    const char *food;
} s_animals[PET_ANIMAL_COUNT] = {
    [PET_ANIMAL_DOG] = {"dog", "Dog", "bone"},
    [PET_ANIMAL_CAT] = {"cat", "Cat", "fish"},
    [PET_ANIMAL_RABBIT] = {"rabbit", "Rabbit", "carrot"},
    [PET_ANIMAL_HAMSTER] = {"hamster", "Hamster", "sunflower seeds"},
    [PET_ANIMAL_PANDA] = {"panda", "Panda", "bamboo"},
    [PET_ANIMAL_PENGUIN] = {"penguin", "Penguin", "fish"},
};

static const struct {
    const char *id;
    const char *label;
    pet_sound_t sound;
} s_moods[PET_MOOD_COUNT] = {
    [PET_MOOD_HAPPY] = {"happy", "Happy", PET_SOUND_HAPPY},
    [PET_MOOD_SMILE] = {"smile", "Smiling", PET_SOUND_NONE},
    [PET_MOOD_LAUGH] = {"laugh", "Laughing", PET_SOUND_LAUGH},
    [PET_MOOD_LOVE] = {"love", "In love", PET_SOUND_LOVE},
    [PET_MOOD_SHY] = {"shy", "Shy", PET_SOUND_NONE},
    [PET_MOOD_SURPRISED] = {"surprised", "Surprised", PET_SOUND_SURPRISE},
    [PET_MOOD_SCARED] = {"scared", "Scared", PET_SOUND_SAD},
    [PET_MOOD_ANGRY] = {"angry", "Angry", PET_SOUND_ANGRY},
    [PET_MOOD_SAD] = {"sad", "Sad", PET_SOUND_SAD},
    [PET_MOOD_BORED] = {"bored", "Bored", PET_SOUND_YAWN},
    [PET_MOOD_HUNGRY] = {"hungry", "Hungry", PET_SOUND_SAD},
    [PET_MOOD_EAT] = {"eat", "Eating", PET_SOUND_EAT},
    [PET_MOOD_DANCE] = {"dance", "Dancing", PET_SOUND_HAPPY},
    [PET_MOOD_SLEEP] = {"sleep", "Sleeping", PET_SOUND_SNORE},
    [PET_MOOD_LISTEN] = {"listen", "Listening", PET_SOUND_NONE},
    [PET_MOOD_THINK] = {"think", "Thinking", PET_SOUND_NONE},
    [PET_MOOD_PLAY] = {"play", "Playing", PET_SOUND_GREET},
    [PET_MOOD_CURIOUS] = {"curious", "Curious", PET_SOUND_GREET},
    [PET_MOOD_PROUD] = {"proud", "Proud", PET_SOUND_HAPPY},
    [PET_MOOD_SLEEPY] = {"sleepy_eyes", "Sleepy", PET_SOUND_YAWN},
    [PET_MOOD_CONFUSED] = {"confused", "Confused", PET_SOUND_SURPRISE},
};

static const struct {
    const char *word;
    pet_mood_t mood;
} s_mood_synonyms[] = {
    {"joy", PET_MOOD_HAPPY}, {"excited", PET_MOOD_HAPPY}, {"playful", PET_MOOD_HAPPY},
    {"smiling", PET_MOOD_SMILE}, {"calm", PET_MOOD_SMILE}, {"content", PET_MOOD_SMILE},
    {"laughing", PET_MOOD_LAUGH}, {"funny", PET_MOOD_LAUGH}, {"giggle", PET_MOOD_LAUGH},
    {"loving", PET_MOOD_LOVE}, {"affectionate", PET_MOOD_LOVE},
    {"blush", PET_MOOD_SHY}, {"embarrassed", PET_MOOD_SHY},
    {"surprise", PET_MOOD_SURPRISED}, {"shocked", PET_MOOD_SURPRISED},
    {"afraid", PET_MOOD_SCARED}, {"fear", PET_MOOD_SCARED},
    {"mad", PET_MOOD_ANGRY}, {"grumpy", PET_MOOD_ANGRY}, {"annoyed", PET_MOOD_ANGRY},
    {"unhappy", PET_MOOD_SAD}, {"lonely", PET_MOOD_SAD},
    {"tired", PET_MOOD_SLEEP}, {"sleepy", PET_MOOD_SLEEP}, {"sleeping", PET_MOOD_SLEEP},
    {"eating", PET_MOOD_EAT}, {"dancing", PET_MOOD_DANCE}, {"thinking", PET_MOOD_THINK},
    {"running", PET_MOOD_PLAY}, {"fetch", PET_MOOD_PLAY}, {"puzzled", PET_MOOD_CONFUSED},
    {"wondering", PET_MOOD_CURIOUS}, {"confident", PET_MOOD_PROUD}, {"drowsy", PET_MOOD_SLEEPY},
};

static bool equal_nocase(const char *a, const char *b)
{
    for (; *a && *b; ++a, ++b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) {
            return false;
        }
    }
    return *a == '\0' && *b == '\0';
}

const char *pet_animal_id(pet_animal_t animal)
{
    return (unsigned)animal < PET_ANIMAL_COUNT ? s_animals[animal].id : NULL;
}

const char *pet_animal_name(pet_animal_t animal)
{
    return (unsigned)animal < PET_ANIMAL_COUNT ? s_animals[animal].name : "Pet";
}

const char *pet_animal_food(pet_animal_t animal)
{
    return (unsigned)animal < PET_ANIMAL_COUNT ? s_animals[animal].food : "treats";
}

pet_animal_t pet_animal_parse(const char *text)
{
    if (!text) {
        return PET_ANIMAL_COUNT;
    }
    for (int i = 0; i < PET_ANIMAL_COUNT; ++i) {
        if (equal_nocase(text, s_animals[i].id)) {
            return (pet_animal_t)i;
        }
    }
    return PET_ANIMAL_COUNT;
}

const char *pet_mood_id(pet_mood_t mood)
{
    return (unsigned)mood < PET_MOOD_COUNT ? s_moods[mood].id : "happy";
}

const char *pet_mood_label(pet_mood_t mood)
{
    return (unsigned)mood < PET_MOOD_COUNT ? s_moods[mood].label : "Happy";
}

pet_mood_t pet_mood_parse(const char *text)
{
    if (!text) {
        return PET_MOOD_COUNT;
    }
    for (int i = 0; i < PET_MOOD_COUNT; ++i) {
        if (equal_nocase(text, s_moods[i].id)) {
            return (pet_mood_t)i;
        }
    }
    for (size_t i = 0; i < sizeof(s_mood_synonyms) / sizeof(s_mood_synonyms[0]); ++i) {
        if (equal_nocase(text, s_mood_synonyms[i].word)) {
            return s_mood_synonyms[i].mood;
        }
    }
    return PET_MOOD_COUNT;
}

pet_sound_t pet_mood_sound(pet_mood_t mood)
{
    return (unsigned)mood < PET_MOOD_COUNT ? s_moods[mood].sound : PET_SOUND_NONE;
}
