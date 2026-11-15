// SPDX-License-Identifier: Apache-2.0
#pragma once

/* Procedural animal sounds (barks, meows, panting, purring, snoring, crunching
 * ...), generated as 16-bit mono PCM. No sample assets; portable and pure. */

#include <stddef.h>
#include <stdint.h>
#include "pet_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PET_SYNTH_RATE 16000U
/** Longest effect, in samples at PET_SYNTH_RATE (2 s). */
#define PET_SYNTH_MAX_SAMPLES (PET_SYNTH_RATE * 2U)

/** Render sound for animal into out (capacity max_samples). seed varies the
 * performance slightly. Returns the number of samples written (0 for NONE). */
size_t pet_synth(pet_animal_t animal, pet_sound_t sound, uint32_t seed,
                 int16_t *out, size_t max_samples);

#ifdef __cplusplus
}
#endif
