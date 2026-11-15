// SPDX-License-Identifier: Apache-2.0
#pragma once

/* Text handling for model replies and speech transcripts. Portable, pure
 * functions; all outputs are NUL-terminated and truncated to out_size. */

#include <stddef.h>
#include "pet_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PET_ACTION_NONE = 0,
    PET_ACTION_MUSIC,   /**< Play a radio station (argument: station name, may be empty). */
    PET_ACTION_STOP,    /**< Stop music. */
    PET_ACTION_FEED,
    PET_ACTION_SLEEP,
    PET_ACTION_WAKE,
} pet_action_t;

#define PET_ACTION_ARG_MAX 32

typedef struct {
    pet_mood_t mood;                 /**< PET_MOOD_COUNT when the reply had no tag. */
    pet_action_t action;
    char action_arg[PET_ACTION_ARG_MAX];
} pet_reply_meta_t;

/** Turn model output into plain, speakable sentences: drops Markdown
 * (#, *, _, `, >, bullets, tables, links -> link text), emoji and other
 * non-ASCII symbols (smart quotes and dashes become ASCII), wrapping quotes
 * and stray apostrophes, collapses whitespace, capitalizes the first letter
 * and ends with punctuation. Returns the output length. */
size_t pet_text_clean(const char *in, char *out, size_t out_size);

/** Extract and remove [mood:x] / [action:x] / [action:music:Station] tags
 * (case-insensitive, anywhere), then clean the remainder into out. */
size_t pet_reply_parse(const char *raw, pet_reply_meta_t *meta, char *out, size_t out_size);

/** Copy the page starting at text[start] that fits max_lines lines of
 * line_chars characters, breaking at word boundaries (lines joined with
 * '\n'). Returns the start offset of the next page, or strlen(text). */
size_t pet_text_page(const char *text, size_t start, size_t line_chars, size_t max_lines,
                     char *out, size_t out_size);

/** Best-effort name from a transcript such as "Hi, my name is patrick."
 * -> "Patrick". Up to two words, letters/'-' only. Returns 0 when none. */
size_t pet_name_extract(const char *transcript, char *out, size_t out_size);

/** Local intent from what the user said, used when the model gave no
 * action tag. Fills arg (may be empty) for MUSIC. */
pet_action_t pet_intent_detect(const char *user_text, char *arg, size_t arg_size);

#ifdef __cplusplus
}
#endif
