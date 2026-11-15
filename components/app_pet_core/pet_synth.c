// SPDX-License-Identifier: Apache-2.0
#include "pet_synth.h"

#include <math.h>
#include <stdbool.h>
#include <string.h>

/* Every sound is a short score of segments. A segment is a pitched voice
 * (four harmonics shaped like a vowel) mixed with filtered noise, with a pitch
 * glide, attack/decay envelope and optional amplitude wobble (purr, pant,
 * snore rumble). */

#define TWO_PI 6.2831853f

typedef struct {
    float start_s;     /* offset from sound start */
    float dur_s;
    float f0, f1;      /* pitch glide, Hz (0 = noise only) */
    float voice;       /* 0..1 pitched level */
    float noise;       /* 0..1 noise level */
    float bright;      /* 0 dull .. 1 buzzy (harmonic weights / noise filter) */
    float attack_s;
    float release_s;
    float wobble_hz;   /* amplitude modulation rate */
    float wobble;      /* 0..1 depth */
    float vibrato;     /* fractional pitch vibrato depth */
} seg_t;

#define MAX_SEGS 12

typedef struct {
    seg_t s[MAX_SEGS];
    int n;
    float pitch;       /* per-animal pitch scale */
} score_t;

static void add(score_t *sc, seg_t seg)
{
    if (sc->n < MAX_SEGS) {
        sc->s[sc->n++] = seg;
    }
}

/* ---- per-animal voices -------------------------------------------------- */

static float animal_pitch(pet_animal_t a)
{
    static const float p[PET_ANIMAL_COUNT] = {1.0f, 1.25f, 1.8f, 2.4f, 0.75f, 0.9f};
    return (unsigned)a < PET_ANIMAL_COUNT ? p[a] : 1.0f;
}

static void greet(score_t *sc, pet_animal_t a)
{
    switch (a) {
    case PET_ANIMAL_DOG: /* "woof woof" */
        for (int i = 0; i < 2; ++i) {
            add(sc, (seg_t){.start_s = i * 0.28f, .dur_s = 0.16f, .f0 = 520, .f1 = 300, .voice = 0.9f,
                            .noise = 0.35f, .bright = 0.8f, .attack_s = 0.008f, .release_s = 0.09f});
        }
        break;
    case PET_ANIMAL_CAT: /* "mi-aow" */
        add(sc, (seg_t){.dur_s = 0.62f, .f0 = 480, .f1 = 380, .voice = 0.8f, .noise = 0.05f, .bright = 0.45f,
                        .attack_s = 0.06f, .release_s = 0.25f, .vibrato = 0.02f});
        add(sc, (seg_t){.start_s = 0.12f, .dur_s = 0.22f, .f0 = 600, .f1 = 760, .voice = 0.5f, .bright = 0.6f,
                        .attack_s = 0.05f, .release_s = 0.12f});
        break;
    case PET_ANIMAL_RABBIT: /* soft thump + nose squeak */
        add(sc, (seg_t){.dur_s = 0.08f, .f0 = 90, .f1 = 60, .voice = 0.9f, .noise = 0.3f, .bright = 0.1f,
                        .attack_s = 0.002f, .release_s = 0.07f});
        add(sc, (seg_t){.start_s = 0.18f, .dur_s = 0.12f, .f0 = 1500, .f1 = 1900, .voice = 0.35f,
                        .bright = 0.2f, .attack_s = 0.01f, .release_s = 0.06f});
        break;
    case PET_ANIMAL_HAMSTER: /* squeak squeak */
        for (int i = 0; i < 3; ++i) {
            add(sc, (seg_t){.start_s = i * 0.13f, .dur_s = 0.08f, .f0 = 2200, .f1 = 2700, .voice = 0.5f,
                            .bright = 0.3f, .attack_s = 0.005f, .release_s = 0.04f});
        }
        break;
    case PET_ANIMAL_PANDA: /* bleat */
        add(sc, (seg_t){.dur_s = 0.5f, .f0 = 260, .f1 = 220, .voice = 0.8f, .noise = 0.15f, .bright = 0.5f,
                        .attack_s = 0.04f, .release_s = 0.2f, .wobble_hz = 9, .wobble = 0.5f});
        break;
    default: /* penguin honk honk */
        for (int i = 0; i < 2; ++i) {
            add(sc, (seg_t){.start_s = i * 0.3f, .dur_s = 0.2f, .f0 = 380, .f1 = 340, .voice = 0.85f,
                            .bright = 1.0f, .attack_s = 0.01f, .release_s = 0.08f, .vibrato = 0.03f});
        }
        break;
    }
}

static void happy(score_t *sc, pet_animal_t a)
{
    if (a == PET_ANIMAL_DOG) { /* panting: hah-hah-hah */
        for (int i = 0; i < 5; ++i) {
            add(sc, (seg_t){.start_s = i * 0.2f, .dur_s = 0.12f, .noise = 0.55f, .bright = 0.55f,
                            .attack_s = 0.02f, .release_s = 0.08f});
        }
    } else if (a == PET_ANIMAL_CAT) { /* purr */
        add(sc, (seg_t){.dur_s = 1.4f, .f0 = 26, .f1 = 24, .voice = 0.5f, .noise = 0.45f, .bright = 0.15f,
                        .attack_s = 0.2f, .release_s = 0.3f, .wobble_hz = 26, .wobble = 0.9f});
    } else { /* little trill */
        for (int i = 0; i < 4; ++i) {
            add(sc, (seg_t){.start_s = i * 0.09f, .dur_s = 0.08f, .f0 = 700 + i * 120.0f, .f1 = 820 + i * 120.0f,
                            .voice = 0.5f, .bright = 0.4f, .attack_s = 0.01f, .release_s = 0.04f});
        }
    }
}

static void laugh(score_t *sc)
{
    /* "he-he-he-he": descending, voiced with breath. */
    for (int i = 0; i < 5; ++i) {
        add(sc, (seg_t){.start_s = i * 0.14f, .dur_s = 0.1f, .f0 = 620 - i * 40.0f, .f1 = 560 - i * 40.0f,
                        .voice = 0.7f, .noise = 0.3f, .bright = 0.55f, .attack_s = 0.01f, .release_s = 0.06f});
    }
}

static void angry(score_t *sc, pet_animal_t a)
{
    if (a == PET_ANIMAL_CAT) { /* hiss */
        add(sc, (seg_t){.dur_s = 0.8f, .noise = 0.45f, .bright = 1.0f, .attack_s = 0.03f, .release_s = 0.3f});
    } else { /* growl / grumble */
        add(sc, (seg_t){.dur_s = 0.9f, .f0 = 95, .f1 = 85, .voice = 0.8f, .noise = 0.25f, .bright = 0.9f,
                        .attack_s = 0.08f, .release_s = 0.25f, .wobble_hz = 18, .wobble = 0.6f});
    }
}

static void sad(score_t *sc)
{
    /* whimper / mew */
    add(sc, (seg_t){.dur_s = 0.55f, .f0 = 900, .f1 = 620, .voice = 0.55f, .noise = 0.05f, .bright = 0.25f,
                    .attack_s = 0.05f, .release_s = 0.25f, .vibrato = 0.03f});
    add(sc, (seg_t){.start_s = 0.65f, .dur_s = 0.35f, .f0 = 780, .f1 = 600, .voice = 0.4f, .bright = 0.25f,
                    .attack_s = 0.04f, .release_s = 0.2f, .vibrato = 0.03f});
}

static void eat(score_t *sc, uint32_t seed)
{
    /* crunch-crunch: short bright noise bursts with irregular spacing. */
    float t = 0;
    for (int i = 0; i < 4; ++i) {
        const float jitter = (float)((seed >> (i * 3)) & 7) * 0.012f;
        add(sc, (seg_t){.start_s = t, .dur_s = 0.07f, .noise = 0.8f, .bright = 0.85f, .attack_s = 0.002f,
                        .release_s = 0.05f, .wobble_hz = 140, .wobble = 0.7f});
        t += 0.13f + jitter;
    }
}

static void snore(score_t *sc)
{
    /* Rumbling inhale, then a soft whistling exhale. */
    add(sc, (seg_t){.dur_s = 1.1f, .f0 = 70, .f1 = 85, .voice = 0.55f, .noise = 0.45f, .bright = 0.35f,
                    .attack_s = 0.35f, .release_s = 0.2f, .wobble_hz = 32, .wobble = 0.8f});
    add(sc, (seg_t){.start_s = 1.25f, .dur_s = 0.6f, .f0 = 900, .f1 = 700, .voice = 0.12f, .noise = 0.2f,
                    .bright = 0.2f, .attack_s = 0.15f, .release_s = 0.3f});
}

static void yawn(score_t *sc)
{
    add(sc, (seg_t){.dur_s = 1.0f, .f0 = 480, .f1 = 230, .voice = 0.6f, .noise = 0.3f, .bright = 0.35f,
                    .attack_s = 0.15f, .release_s = 0.4f});
}

static void surprise(score_t *sc)
{
    add(sc, (seg_t){.dur_s = 0.22f, .f0 = 380, .f1 = 1150, .voice = 0.7f, .noise = 0.1f, .bright = 0.5f,
                    .attack_s = 0.01f, .release_s = 0.08f});
}

static void love(score_t *sc, pet_animal_t a)
{
    if (a == PET_ANIMAL_CAT) {
        happy(sc, a);
        return;
    }
    /* "awww" */
    add(sc, (seg_t){.dur_s = 0.7f, .f0 = 520, .f1 = 680, .voice = 0.6f, .noise = 0.08f, .bright = 0.3f,
                    .attack_s = 0.1f, .release_s = 0.3f, .vibrato = 0.025f});
}

/* ---- renderer ------------------------------------------------------------- */

static float noise_next(uint32_t *s)
{
    *s ^= *s << 13;
    *s ^= *s >> 17;
    *s ^= *s << 5;
    return (float)(int32_t)*s / 2147483648.0f;
}

size_t pet_synth(pet_animal_t animal, pet_sound_t sound, uint32_t seed, int16_t *out, size_t max_samples)
{
    if (!out || max_samples == 0 || sound == PET_SOUND_NONE || sound >= PET_SOUND_COUNT) {
        return 0;
    }
    score_t sc = {.pitch = animal_pitch(animal)};
    switch (sound) {
    case PET_SOUND_GREET: greet(&sc, animal); sc.pitch = 1.0f; break;
    case PET_SOUND_HAPPY: happy(&sc, animal); if (animal == PET_ANIMAL_DOG || animal == PET_ANIMAL_CAT) sc.pitch = 1.0f; break;
    case PET_SOUND_LAUGH: laugh(&sc); break;
    case PET_SOUND_ANGRY: angry(&sc, animal); if (animal == PET_ANIMAL_CAT) sc.pitch = 1.0f; break;
    case PET_SOUND_SAD: sad(&sc); break;
    case PET_SOUND_EAT: eat(&sc, seed); sc.pitch = 1.0f; break;
    case PET_SOUND_SNORE: snore(&sc); sc.pitch = 0.6f + 0.4f * sc.pitch; break;
    case PET_SOUND_YAWN: yawn(&sc); break;
    case PET_SOUND_SURPRISE: surprise(&sc); break;
    case PET_SOUND_LOVE: love(&sc, animal); if (animal == PET_ANIMAL_CAT) sc.pitch = 1.0f; break;
    default: return 0;
    }
    /* Slight per-play variation keeps repeated sounds from feeling canned. */
    sc.pitch *= 1.0f + ((float)(seed % 9) - 4.0f) * 0.012f;

    float end_s = 0;
    for (int i = 0; i < sc.n; ++i) {
        const float e = sc.s[i].start_s + sc.s[i].dur_s;
        end_s = e > end_s ? e : end_s;
    }
    size_t total = (size_t)(end_s * PET_SYNTH_RATE) + 1;
    total = total > max_samples ? max_samples : total;
    memset(out, 0, total * sizeof(int16_t));

    uint32_t rng = seed | 1u;
    const float dt = 1.0f / PET_SYNTH_RATE;
    for (int i = 0; i < sc.n; ++i) {
        const seg_t *g = &sc.s[i];
        const size_t first = (size_t)(g->start_s * PET_SYNTH_RATE);
        const size_t count = (size_t)(g->dur_s * PET_SYNTH_RATE);
        /* Harmonic weights: dull voices are nearly sine, bright ones buzzy. */
        const float h2 = 0.2f + 0.5f * g->bright, h3 = 0.05f + 0.45f * g->bright, h4 = 0.3f * g->bright;
        const float norm = 1.0f / (1.0f + h2 + h3 + h4);
        /* One-pole filter coefficient for the noise colour. */
        const float lp = 0.05f + 0.9f * g->bright;
        float phase = 0, lp_state = 0, prev = 0;
        for (size_t n = 0; n < count && first + n < total; ++n) {
            const float t = n * dt;
            const float u = g->dur_s > 0 ? t / g->dur_s : 0;
            float env = 1.0f;
            if (g->attack_s > 0 && t < g->attack_s) {
                env = t / g->attack_s;
            }
            const float rel_start = g->dur_s - g->release_s;
            if (g->release_s > 0 && t > rel_start) {
                const float r = (t - rel_start) / g->release_s;
                env *= (1.0f - r) * (1.0f - r);
            }
            if (g->wobble > 0) {
                env *= 1.0f - g->wobble * 0.5f * (1.0f + sinf(TWO_PI * g->wobble_hz * t));
            }
            float s = 0;
            if (g->f0 > 0 && g->voice > 0) {
                float f = (g->f0 + (g->f1 - g->f0) * u) * sc.pitch;
                f *= 1.0f + g->vibrato * sinf(TWO_PI * 6.0f * t);
                phase += TWO_PI * f * dt;
                if (phase > TWO_PI) {
                    phase -= TWO_PI;
                }
                s += g->voice * norm * (sinf(phase) + h2 * sinf(2 * phase) + h3 * sinf(3 * phase) +
                                        h4 * sinf(4 * phase));
            }
            if (g->noise > 0) {
                const float w = noise_next(&rng);
                lp_state += lp * (w - lp_state);
                /* Bright noise keeps some high-pass content (hiss, crunch). */
                const float hp = lp_state - prev;
                prev = lp_state;
                s += g->noise * (g->bright > 0.7f ? 0.45f * lp_state + 0.6f * hp : lp_state);
            }
            const float v = s * env * 0.6f * 32767.0f;
            const int32_t mixed = (int32_t)out[first + n] + (int32_t)v;
            out[first + n] = (int16_t)(mixed > 32767 ? 32767 : mixed < -32768 ? -32768 : mixed);
        }
    }
    return total;
}
