// SPDX-License-Identifier: Apache-2.0
/* Host tests: synth output sanity for every animal x sound, and a simulated
 * day of pet life. Optionally writes WAVs to argv[1] for listening. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "pet_life.h"
#include "pet_synth.h"

static int failures;
#define EXPECT(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static void write_wav(const char *path, const int16_t *s, size_t n)
{
    FILE *f = fopen(path, "wb");
    if (!f) return;
    uint32_t rate = PET_SYNTH_RATE, bytes = (uint32_t)(n * 2), v32;
    uint16_t v16;
    fwrite("RIFF", 1, 4, f); v32 = 36 + bytes; fwrite(&v32, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    v32 = 16; fwrite(&v32, 4, 1, f); v16 = 1; fwrite(&v16, 2, 1, f); fwrite(&v16, 2, 1, f);
    fwrite(&rate, 4, 1, f); v32 = rate * 2; fwrite(&v32, 4, 1, f); v16 = 2; fwrite(&v16, 2, 1, f);
    v16 = 16; fwrite(&v16, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&bytes, 4, 1, f);
    fwrite(s, 2, n, f); fclose(f);
}

int main(int argc, char **argv)
{
    static int16_t buf[PET_SYNTH_MAX_SAMPLES];
    for (int a = 0; a < PET_ANIMAL_COUNT; ++a) {
        for (int s = 1; s < PET_SOUND_COUNT; ++s) {
            const size_t n = pet_synth(a, s, 1234u + (uint32_t)s, buf, PET_SYNTH_MAX_SAMPLES);
            int peak = 0, clipped = 0;
            double energy = 0;
            for (size_t i = 0; i < n; ++i) {
                const int v = buf[i] < 0 ? -buf[i] : buf[i];
                peak = v > peak ? v : peak;
                clipped += v >= 32767;
                energy += (double)buf[i] * buf[i];
            }
            const double rms = n ? sqrt(energy / n) : 0;
            if (!(n > 800 && peak > 2500 && rms > 300 && clipped < (int)(n / 200))) {
                printf("FAIL sound animal=%s sound=%d n=%zu peak=%d rms=%.0f clipped=%d\n",
                       pet_animal_id(a), s, n, peak, rms, clipped);
                ++failures;
            }
            if (argc > 1) {
                char path[256];
                snprintf(path, sizeof(path), "%s/%s_%02d.wav", argv[1], pet_animal_id(a), s);
                write_wav(path, buf, n);
            }
        }
    }
    EXPECT(pet_synth(PET_ANIMAL_DOG, PET_SOUND_NONE, 1, buf, PET_SYNTH_MAX_SAMPLES) == 0);

    /* A day in the life, 0.5 s steps. */
    pet_life_t l;
    pet_life_event_t e;
    pet_life_init(&l, PET_ANIMAL_DOG, "Patrick", 42);
    int snores = 0, chatter = 0, naps = 0, hungry_seen = 0, blinks = 0;
    bool was_sleeping = false;
    float prev_blink = 0;
    for (int step = 0; step < 2 * 3600 * 6; ++step) {
        pet_life_tick(&l, 0.5f, &e);
        snores += e.sound == PET_SOUND_SNORE;
        chatter += e.say[0] != '\0';
        if (l.sleeping && !was_sleeping) ++naps;
        was_sleeping = l.sleeping;
        hungry_seen |= pet_life_mood(&l) == PET_MOOD_HUNGRY;
        blinks += l.blink >= 1.0f && prev_blink < 1.0f;
        prev_blink = l.blink;
        if (e.say[0]) {
            EXPECT(strchr(e.say, '*') == NULL && strchr(e.say, '%') == NULL);
            if (step < 2 * 3600 && chatter <= 3) printf("  says: %s\n", e.say);
        }
    }
    printf("6h idle: naps=%d snores=%d chatter=%d hungry=%d blinks=%d hunger=%.2f energy=%.2f\n",
           naps, snores, chatter, hungry_seen, blinks, l.hunger, l.energy);
    EXPECT(naps >= 1 && snores > 50 && chatter > 5 && hungry_seen && blinks > 1000);

    pet_life_interact(&l);
    pet_life_feed(&l, &e);
    printf("  feed: %s\n", e.say);
    EXPECT(pet_life_mood(&l) == PET_MOOD_EAT && e.sound == PET_SOUND_EAT && strstr(e.say, "Patrick"));
    for (int i = 0; i < 10; ++i) pet_life_tick(&l, 0.5f, &e);
    EXPECT(pet_life_mood(&l) == PET_MOOD_LOVE);
    pet_life_sleep(&l);
    pet_life_tick(&l, 0.5f, &e);
    EXPECT(pet_life_mood(&l) == PET_MOOD_SLEEP);
    pet_life_pet(&l, &e);
    EXPECT(!l.sleeping && e.sound == PET_SOUND_YAWN);
    pet_life_set_music(&l, true);
    for (int i = 0; i < 10; ++i) pet_life_tick(&l, 0.5f, &e);
    EXPECT(pet_life_mood(&l) == PET_MOOD_DANCE);

    printf(failures ? "%d FAILURE(S)\n" : "pet_life + pet_synth: all tests passed\n", failures);
    return failures != 0;
}
