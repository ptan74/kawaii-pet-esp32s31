// SPDX-License-Identifier: Apache-2.0
#include "pet_text.h"

#include <ctype.h>
#include <stdbool.h>
#include <string.h>
#include <strings.h>

/* ---- small helpers ----------------------------------------------------- */

typedef struct {
    char *buf;
    size_t size;
    size_t len;
} sink_t;

static void put(sink_t *s, char c)
{
    if (s->len + 1 < s->size) {
        s->buf[s->len++] = c;
        s->buf[s->len] = '\0';
    }
}

static char last(const sink_t *s)
{
    return s->len ? s->buf[s->len - 1] : '\0';
}

static bool is_word(char c)
{
    return isalnum((unsigned char)c);
}

/* Map a UTF-8 sequence to an ASCII replacement; returns bytes consumed and
 * sets *rep ("" to drop). Handles the punctuation models commonly emit.
 * one[2] is caller storage for single-letter replacements. */
/* Set *rep to NULL to keep the original bytes (CJK text). */
static size_t utf8_map(const unsigned char *p, const char **rep, char one[2])
{
    size_t n = 1;
    if (p[0] >= 0xF0) {
        n = 4;
    } else if (p[0] >= 0xE0) {
        n = 3;
    } else if (p[0] >= 0xC0) {
        n = 2;
    }
    for (size_t i = 1; i < n; ++i) {
        if (p[i] == '\0') {
            *rep = "";
            return i;
        }
    }
    *rep = "";
    if (n == 3) {
        const unsigned cp = ((unsigned)(p[0] & 0x0F) << 12) | ((unsigned)(p[1] & 0x3F) << 6) | (p[2] & 0x3F);
        const bool cjk = (cp >= 0x3000 && cp <= 0x30FF) || (cp >= 0x3400 && cp <= 0x9FFF) ||
                         (cp >= 0xAC00 && cp <= 0xD7AF) || (cp >= 0xFF00 && cp <= 0xFFEF);
        if (cjk) {
            *rep = NULL; /* keep: Chinese, Japanese kana/kanji, Korean hangul */
            return n;
        }
    }
    if (n == 3 && p[0] == 0xE2 && p[1] == 0x80) {
        switch (p[2]) {
        case 0x98: case 0x99: case 0xB2: *rep = "'"; break;        /* ‘ ’ ′ */
        case 0x9C: case 0x9D: *rep = "\""; break;                  /* “ ” */
        case 0x93: case 0x94: case 0x95: *rep = " - "; break;      /* – — ― */
        case 0xA6: *rep = "..."; break;                            /* … */
        case 0xA2: *rep = " "; break;                              /* • */
        default: break;
        }
    } else if (n == 2 && p[0] == 0xC2 && p[1] == 0xA0) {
        *rep = " ";                                               /* nbsp */
    } else if (n == 2 && p[0] == 0xC3) {
        /* Latin-1 letters: keep a plain base letter so names survive. */
        static const char map[] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
        const unsigned idx = (unsigned)p[1] - 0x80u;
        if (idx < sizeof(map) - 1) {
            one[0] = map[idx];
            one[1] = '\0';
            *rep = one;
        }
    }
    return n;
}

/* ---- cleaning ---------------------------------------------------------- */

/* Pass 1: structural Markdown -> plain text with spaces. */
static void strip_markdown(const char *in, sink_t *s)
{
    bool line_start = true;
    for (const char *p = in; *p;) {
        const char c = *p;
        if (line_start) {
            /* Leading whitespace, headings, quotes, bullets, numbered lists. */
            while (*p == ' ' || *p == '\t') {
                ++p;
            }
            while (*p == '#' || *p == '>') {
                ++p;
            }
            if ((*p == '-' || *p == '*' || *p == '+') && p[1] == ' ') {
                p += 2;
            } else if (isdigit((unsigned char)*p)) {
                const char *q = p;
                while (isdigit((unsigned char)*q)) {
                    ++q;
                }
                if ((*q == '.' || *q == ')') && q[1] == ' ') {
                    p = q + 2;
                }
            }
            line_start = false;
            continue;
        }
        if (c == '\n' || c == '\r') {
            /* Sentence-ish break: keep a separator so words don't merge. */
            const char prev = last(s);
            if (prev && prev != ' ' && prev != '.' && prev != '!' && prev != '?' && prev != ':' && prev != ',') {
                put(s, '.');
            }
            put(s, ' ');
            line_start = true;
            ++p;
            continue;
        }
        if (c == '[') {
            /* [text](url) -> text */
            const char *close = strchr(p, ']');
            if (close && close[1] == '(' && strchr(close, ')')) {
                for (const char *q = p + 1; q < close; ++q) {
                    put(s, *q);
                }
                p = strchr(close, ')') + 1;
                continue;
            }
        }
        if (c == '(' && strncmp(p, "(http", 5) == 0) {
            const char *close = strchr(p, ')');
            p = close ? close + 1 : p + strlen(p);
            continue;
        }
        if (strncmp(p, "http://", 7) == 0 || strncmp(p, "https://", 8) == 0) {
            while (*p && *p != ' ' && *p != '\n') {
                ++p;
            }
            put(s, ' ');
            continue;
        }
        if (c == '*' || c == '_' || c == '`' || c == '#' || c == '~' || c == '|' ||
                c == '<' || c == '>' || c == '{' || c == '}' || c == '\\' || c == '^') {
            /* Emphasis markers vanish; keep intra-word underscores as spaces. */
            if (c == '_' && is_word(last(s)) && is_word(p[1])) {
                put(s, ' ');
            }
            if (c == '|') {
                put(s, ' ');
            }
            ++p;
            continue;
        }
        if ((unsigned char)c >= 0x80) {
            const char *rep;
            char one[2];
            const unsigned char *u = (const unsigned char *)p;
            const size_t used = utf8_map(u, &rep, one);
            if (rep == NULL) {
                for (size_t i = 0; i < used && p[i]; ++i) {
                    put(s, p[i]);
                }
            } else {
                for (; *rep; ++rep) {
                    put(s, *rep);
                }
            }
            p += used;
            continue;
        }
        if ((unsigned char)c < 0x20) {
            put(s, ' ');
            ++p;
            continue;
        }
        put(s, c);
        ++p;
    }
}

/* Pass 2: whitespace, quotes, punctuation spacing, capitalization. */
static size_t tidy(const char *in, sink_t *s)
{
    const size_t n = strlen(in);
    for (size_t i = 0; i < n; ++i) {
        char c = in[i];
        const char prev = last(s);
        const char next = in[i + 1];
        if (c == ' ') {
            if (prev == '\0' || prev == ' ') {
                continue;
            }
            put(s, ' ');
            continue;
        }
        if (c == '\'') {
            /* Keep apostrophes inside words (I'm, dog's); drop stray quotes. */
            if (!(is_word(prev) && is_word(next))) {
                continue;
            }
        }
        if (c == '"') {
            continue;
        }
        if (c == '.' || c == ',' || c == '!' || c == '?' || c == ':' || c == ';') {
            if (prev == ' ') {
                s->buf[--s->len] = '\0';
            }
            if (s->len == 0) {
                continue; /* no leading punctuation */
            }
            /* Collapse ". ." and ",." runs but keep "..." and "?!". */
            const char p2 = last(s);
            if ((c == '.' && (p2 == ',' || p2 == ':' || p2 == ';')) ||
                    (c == ',' && (p2 == '.' || p2 == '!' || p2 == '?' || p2 == ','))) {
                continue;
            }
            if (c == '.' && (p2 == '!' || p2 == '?')) {
                continue;
            }
            put(s, c);
            if (next && next != ' ' && next != '.' && next != '!' && next != '?' &&
                    !(c == '.' && isdigit((unsigned char)next)) && !(c == ':' && isdigit((unsigned char)next))) {
                put(s, ' ');
            }
            continue;
        }
        put(s, c);
    }
    while (s->len && s->buf[s->len - 1] == ' ') {
        s->buf[--s->len] = '\0';
    }
    /* Capitalize sentence starts. */
    bool cap = true;
    for (size_t i = 0; i < s->len; ++i) {
        char *c = &s->buf[i];
        if (cap && isalpha((unsigned char)*c)) {
            *c = (char)toupper((unsigned char)*c);
            cap = false;
        } else if (*c == '.' || *c == '!' || *c == '?') {
            cap = true;
        } else if (isalnum((unsigned char)*c)) {
            cap = false;
        }
    }
    /* Standalone lower-case "i" -> "I". */
    for (size_t i = 0; i < s->len; ++i) {
        if (s->buf[i] == 'i' && (i == 0 || !is_word(s->buf[i - 1])) &&
                (i + 1 == s->len || !is_word(s->buf[i + 1]))) {
            s->buf[i] = 'I';
        }
    }
    const char end = last(s);
    if (s->len && end != '.' && end != '!' && end != '?') {
        if (end == ',' || end == ':' || end == ';' || end == '-') {
            s->buf[--s->len] = '\0';
        }
        put(s, '.');
    }
    return s->len;
}

size_t pet_text_clean(const char *in, char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return 0;
    }
    out[0] = '\0';
    if (!in) {
        return 0;
    }
    /* Work buffer bounded by the output size: cleaning never grows text by
     * more than the "..." / " - " expansions, so 2x is ample. Stack budget:
     * this and pet_reply_parse() use ~2 KiB; callers are the brain task
     * (12 KiB stack) and the host simulator. */
    char work[1024];
    sink_t w = {work, sizeof(work) < out_size * 2 ? sizeof(work) : out_size * 2, 0};
    work[0] = '\0';
    strip_markdown(in, &w);
    sink_t s = {out, out_size, 0};
    return tidy(work, &s);
}

/* ---- tags ---------------------------------------------------------------- */

static pet_action_t action_parse(const char *word)
{
    static const struct {
        const char *w;
        pet_action_t a;
    } map[] = {
        {"music", PET_ACTION_MUSIC}, {"play", PET_ACTION_MUSIC}, {"radio", PET_ACTION_MUSIC},
        {"stop", PET_ACTION_STOP}, {"stop_music", PET_ACTION_STOP},
        {"feed", PET_ACTION_FEED}, {"eat", PET_ACTION_FEED},
        {"sleep", PET_ACTION_SLEEP}, {"wake", PET_ACTION_WAKE},
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); ++i) {
        size_t k = 0;
        while (map[i].w[k] && tolower((unsigned char)word[k]) == map[i].w[k]) {
            ++k;
        }
        if (map[i].w[k] == '\0' && word[k] == '\0') {
            return map[i].a;
        }
    }
    return PET_ACTION_NONE;
}

size_t pet_reply_parse(const char *raw, pet_reply_meta_t *meta, char *out, size_t out_size)
{
    pet_reply_meta_t local = {.mood = PET_MOOD_COUNT};
    char stripped[1024];
    sink_t s = {stripped, sizeof(stripped), 0};
    stripped[0] = '\0';
    for (const char *p = raw ? raw : ""; *p;) {
        if (*p == '[') {
            const char *close = strchr(p, ']');
            if (close && close - p < 64) {
                char tag[64];
                const size_t n = (size_t)(close - p - 1);
                memcpy(tag, p + 1, n);
                tag[n] = '\0';
                char *colon = strchr(tag, ':');
                if (colon) {
                    *colon = '\0';
                    char *value = colon + 1;
                    while (*value == ' ') {
                        ++value;
                    }
                    for (char *k = tag; *k; ++k) {
                        *k = (char)tolower((unsigned char)*k);
                    }
                    if (strcmp(tag, "mood") == 0 || strcmp(tag, "emotion") == 0) {
                        local.mood = pet_mood_parse(value);
                        p = close + 1;
                        continue;
                    }
                    if (strcmp(tag, "action") == 0) {
                        char *arg = strchr(value, ':');
                        if (arg) {
                            *arg++ = '\0';
                            while (*arg == ' ') {
                                ++arg;
                            }
                            strncpy(local.action_arg, arg, sizeof(local.action_arg) - 1);
                        }
                        local.action = action_parse(value);
                        p = close + 1;
                        continue;
                    }
                }
            }
        }
        put(&s, *p++);
    }
    if (meta) {
        *meta = local;
    }
    return pet_text_clean(stripped, out, out_size);
}

/* ---- paging ---------------------------------------------------------------- */

size_t pet_text_page(const char *text, size_t start, size_t line_chars, size_t max_lines,
                     char *out, size_t out_size)
{
    const size_t len = text ? strlen(text) : 0;
    sink_t s = {out, out_size, 0};
    if (out_size) {
        out[0] = '\0';
    }
    if (start >= len || line_chars == 0 || max_lines == 0) {
        return len;
    }
    size_t pos = start;
    for (size_t line = 0; line < max_lines && pos < len; ++line) {
        while (pos < len && text[pos] == ' ') {
            ++pos;
        }
        size_t end = pos + line_chars;
        if (end >= len) {
            end = len;
        } else {
            size_t brk = end;
            while (brk > pos && text[brk] != ' ') {
                --brk;
            }
            end = brk > pos ? brk : end; /* hard-break a very long word */
        }
        if (line) {
            put(&s, '\n');
        }
        for (size_t i = pos; i < end; ++i) {
            put(&s, text[i]);
        }
        pos = end;
    }
    while (pos < len && text[pos] == ' ') {
        ++pos;
    }
    return pos;
}

/* ---- names and intents ------------------------------------------------- */

static bool starts_word(const char *p, const char *w)
{
    while (*w) {
        if (tolower((unsigned char)*p++) != *w++) {
            return false;
        }
    }
    return !isalpha((unsigned char)*p);
}

size_t pet_name_extract(const char *transcript, char *out, size_t out_size)
{
    static const char *const lead[] = {
        "my name is", "my name's", "name is", "i am", "i'm", "im", "call me", "it's", "its",
        "this is", "you can call me", "they call me",
    };
    static const char *const skip[] = {
        "hi", "hello", "hey", "um", "uh", "oh", "well", "so", "okay", "ok", "yes", "yeah",
        "the", "a", "just", "and", "name",
    };
    if (!out || out_size == 0) {
        return 0;
    }
    out[0] = '\0';
    if (!transcript) {
        return 0;
    }
    /* Korean and Japanese introductions: 제 이름은X / 저는X / 名前はX / 私はX. */
    static const char *const kj_lead[] = {"제 이름은", "이름은", "저는", "나는", "名前は", "私は", "僕は"};
    for (size_t i = 0; i < sizeof(kj_lead) / sizeof(kj_lead[0]); ++i) {
        const char *hit = strstr(transcript, kj_lead[i]);
        if (!hit) {
            continue;
        }
        const char *q = hit + strlen(kj_lead[i]);
        while (*q == ' ') {
            ++q;
        }
        size_t o = 0, chars = 0;
        while (*q && chars < 8 && o + 3 < out_size) {
            const unsigned char c0 = (unsigned char)*q;
            if (c0 < 0xE3 || c0 > 0xED) {
                break; /* ASCII, spaces and CJK punctuation end the name */
            }
            /* Japanese particles and CJK punctuation end the name. */
            if (strncmp(q, "\xE3\x81\xA7", 3) == 0 || strncmp(q, "\xE3\x81\xA8", 3) == 0 ||
                strncmp(q, "\xE3\x80\x82", 3) == 0 || strncmp(q, "\xE3\x80\x81", 3) == 0 ||
                strncmp(q, "\xEF\xBC\x8C", 3) == 0 || strncmp(q, "\xEF\xBC\x81", 3) == 0 ||
                strncmp(q, "\xEF\xBC\x9F", 3) == 0) {
                break;
            }
            memcpy(out + o, q, 3);
            o += 3;
            q += 3;
            chars++;
        }
        /* Drop Korean copulas: 입니다 / 이에요 / 예요 / 이야 / 야. */
        static const char *const copulas[] = {"\xEC\x9E\x85\xEB\x8B\x88\xEB\x8B\xA4", "\xEC\x9D\xB4\xEC\x97\x90\xEC\x9A\x94",
                                              "\xEC\x98\x88\xEC\x9A\x94", "\xEC\x9D\xB4\xEC\x95\xBC"};
        for (size_t k = 0; k < sizeof(copulas) / sizeof(copulas[0]); ++k) {
            const size_t cl = strlen(copulas[k]);
            if (o > cl && memcmp(out + o - cl, copulas[k], cl) == 0) {
                o -= cl;
                break;
            }
        }
        out[o] = '\0';
        if (o) {
            return o;
        }
    }
    /* Chinese: 我叫X / 名字是X / 名字叫X - take up to four CJK characters. */
    static const char *const zh_lead[] = {"我叫", "名字是", "名字叫", "叫我"};
    for (size_t i = 0; i < sizeof(zh_lead) / sizeof(zh_lead[0]); ++i) {
        const char *hit = strstr(transcript, zh_lead[i]);
        if (hit) {
            const char *q = hit + strlen(zh_lead[i]);
            size_t o = 0, chars = 0;
            while (*q && chars < 4) {
                const unsigned char c0 = (unsigned char)*q;
                if (c0 < 0xE3 || c0 > 0xE9 || o + 3 >= out_size) {
                    break; /* stop at punctuation, spaces or the end */
                }
                memcpy(out + o, q, 3);
                o += 3;
                q += 3;
                chars++;
            }
            out[o] = '\0';
            if (o) {
                return o;
            }
        }
    }
    char clean[160];
    pet_text_clean(transcript, clean, sizeof(clean));
    const char *p = clean;
    /* Prefer the words after an introduction phrase. */
    for (const char *q = clean; *q; ++q) {
        if (q != clean && isalpha((unsigned char)q[-1])) {
            continue;
        }
        for (size_t i = 0; i < sizeof(lead) / sizeof(lead[0]); ++i) {
            if (starts_word(q, lead[i])) {
                p = q + strlen(lead[i]);
                goto found;
            }
        }
    }
found:;
    sink_t s = {out, out_size, 0};
    int words = 0;
    while (*p && words < 2) {
        while (*p && !isalpha((unsigned char)*p)) {
            if (*p == '.' || *p == '!' || *p == '?' || *p == ',') {
                if (words) {
                    return s.len;
                }
            }
            ++p;
        }
        const char *w = p;
        while (isalpha((unsigned char)*p) || *p == '-' || *p == '\'') {
            ++p;
        }
        const size_t wl = (size_t)(p - w);
        if (wl == 0) {
            break;
        }
        bool skipped = false;
        if (!words) {
            for (size_t i = 0; i < sizeof(skip) / sizeof(skip[0]); ++i) {
                if (strlen(skip[i]) == wl && starts_word(w, skip[i])) {
                    skipped = true;
                    break;
                }
            }
        }
        if (skipped) {
            continue;
        }
        if (words) {
            put(&s, ' ');
        }
        for (size_t i = 0; i < wl; ++i) {
            put(&s, (char)(i == 0 ? toupper((unsigned char)w[i]) : tolower((unsigned char)w[i])));
        }
        ++words;
    }
    return s.len;
}

static bool contains_word(const char *hay, const char *word)
{
    const size_t wl = strlen(word);
    for (const char *p = hay; *p; ++p) {
        if ((p == hay || !isalpha((unsigned char)p[-1])) && strncasecmp(p, word, wl) == 0 &&
                !isalpha((unsigned char)p[wl])) {
            return true;
        }
    }
    return false;
}

pet_action_t pet_intent_detect(const char *user_text, char *arg, size_t arg_size)
{
    if (arg && arg_size) {
        arg[0] = '\0';
    }
    if (!user_text) {
        return PET_ACTION_NONE;
    }
    if ((contains_word(user_text, "stop") || contains_word(user_text, "turn off")) &&
            (contains_word(user_text, "music") || contains_word(user_text, "radio") ||
             contains_word(user_text, "song") || contains_word(user_text, "playing"))) {
        return PET_ACTION_STOP;
    }
    if (contains_word(user_text, "music") || contains_word(user_text, "radio") ||
            contains_word(user_text, "song") || contains_word(user_text, "songs") ||
            (contains_word(user_text, "play") && contains_word(user_text, "fm"))) {
        return PET_ACTION_MUSIC;
    }
    if (contains_word(user_text, "go to sleep") || contains_word(user_text, "goodnight") ||
            contains_word(user_text, "good night") || contains_word(user_text, "bedtime")) {
        return PET_ACTION_SLEEP;
    }
    return PET_ACTION_NONE;
}
