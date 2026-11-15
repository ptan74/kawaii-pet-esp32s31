// SPDX-License-Identifier: Apache-2.0
/* Host tests for pet_text: cc -I components/app_pet_core/include ... */
#include <stdio.h>
#include <string.h>
#include "pet_text.h"

static int failures;
#define EXPECT_STR(got, want) do { if (strcmp((got), (want)) != 0) { \
    printf("FAIL %s:%d\n  got:  [%s]\n  want: [%s]\n", __FILE__, __LINE__, (got), (want)); ++failures; } } while (0)
#define EXPECT(cond) do { if (!(cond)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

int main(void)
{
    char out[512];
    pet_text_clean("## Hello **Patrick**!\n\n- I *love* `bones`\n- and walks", out, sizeof(out));
    EXPECT_STR(out, "Hello Patrick! I love bones. And walks.");
    pet_text_clean("'woof woof' i'm so happy\xE2\x80\x94really \xF0\x9F\x90\xB6\xF0\x9F\x90\xB6", out, sizeof(out));
    EXPECT_STR(out, "Woof woof I'm so happy - really.");
    pet_text_clean("Check [this](https://x.com) out https://y.com/z ok", out, sizeof(out));
    EXPECT_STR(out, "Check this out ok.");
    pet_text_clean("  \xE2\x80\x9CYes,\xE2\x80\x9D she said\xE2\x80\xA6  ", out, sizeof(out));
    EXPECT_STR(out, "Yes, she said...");
    pet_text_clean("| a | b |\n|---|---|\n| 1 | 2 |", out, sizeof(out));
    EXPECT(strchr(out, '|') == NULL && strchr(out, '#') == NULL);
    pet_text_clean("it is 3.5 kg . great ,", out, sizeof(out));
    EXPECT_STR(out, "It is 3.5 kg. Great.");
    pet_text_clean("Caf\xC3\xA9 time", out, sizeof(out));
    EXPECT_STR(out, "Cafe time.");

    pet_reply_meta_t m;
    pet_reply_parse("[mood:laugh] Haha, that's **funny**! [action:music:Class 95]", &m, out, sizeof(out));
    EXPECT(m.mood == PET_MOOD_LAUGH);
    EXPECT(m.action == PET_ACTION_MUSIC);
    EXPECT_STR(m.action_arg, "Class 95");
    EXPECT_STR(out, "Haha, that's funny!");
    pet_reply_parse("No tags here", &m, out, sizeof(out));
    EXPECT(m.mood == PET_MOOD_COUNT && m.action == PET_ACTION_NONE);
    pet_reply_parse("[Mood: Grumpy] Hmph.", &m, out, sizeof(out));
    EXPECT(m.mood == PET_MOOD_ANGRY);

    const char *t = "The quick brown fox jumps over the lazy dog and keeps running far away";
    size_t next = pet_text_page(t, 0, 20, 2, out, sizeof(out));
    EXPECT_STR(out, "The quick brown fox\njumps over the lazy");
    next = pet_text_page(t, next, 20, 2, out, sizeof(out));
    EXPECT_STR(out, "dog and keeps\nrunning far away");
    EXPECT(next == strlen(t));

    pet_name_extract("Hi, my name is patrick.", out, sizeof(out));
    EXPECT_STR(out, "Patrick");
    pet_name_extract("um I'm Mary Jane Watson", out, sizeof(out));
    EXPECT_STR(out, "Mary Jane");
    pet_name_extract("Patrick", out, sizeof(out));
    EXPECT_STR(out, "Patrick");
    pet_name_extract("Hello. Call me Tan!", out, sizeof(out));
    EXPECT_STR(out, "Tan");
    EXPECT(pet_name_extract("...", out, sizeof(out)) == 0);

    char arg[32];
    EXPECT(pet_intent_detect("Can you play some music?", arg, sizeof(arg)) == PET_ACTION_MUSIC);
    EXPECT(pet_intent_detect("please stop the music", arg, sizeof(arg)) == PET_ACTION_STOP);
    EXPECT(pet_intent_detect("Good night buddy", arg, sizeof(arg)) == PET_ACTION_SLEEP);
    EXPECT(pet_intent_detect("How are you?", arg, sizeof(arg)) == PET_ACTION_NONE);

    printf(failures ? "%d FAILURE(S)\n" : "pet_text: all tests passed\n", failures);
    return failures != 0;
}
