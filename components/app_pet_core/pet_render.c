// SPDX-License-Identifier: Apache-2.0
#include "pet_render.h"

#include <math.h>
#include <stdbool.h>
#include <string.h>

/* Artwork is authored in a 220-unit square (the approved mockup) and scaled
 * to the canvas. All geometry below is in those units. */
#define ART_UNITS 220.0f
#define DEG (3.14159265f / 180.0f)
#define PI_F 3.14159265f

#define INK 0x26215Cu
#define MOUTH_RED 0x791F1Fu
#define TONGUE 0xED93B1u
#define BLUSH 0xF09595u
#define HEART 0xD4537Eu
#define TEAR 0x85B7EBu
#define WHITE 0xFFFFFFu
#define FX_SOFT 0xAFA9ECu
#define FX_NOTE 0x5DCAA5u
#define FX_ALERT 0xFAC775u
#define BOWL 0x7F77DDu

typedef struct {
    uint32_t fur;     /* main colour */
    uint32_t dark;    /* ears, tail, accents */
    uint32_t belly;
} palette_t;

static const palette_t s_palette[PET_ANIMAL_COUNT] = {
    [PET_ANIMAL_DOG] = {0xEF9F27, 0xBA7517, 0xFAEEDA},
    [PET_ANIMAL_CAT] = {0xB4B2A9, 0x5F5E5A, 0xF1EFE8},
    [PET_ANIMAL_RABBIT] = {0xF4C0D1, 0xD4537E, 0xFBEAF0},
    [PET_ANIMAL_HAMSTER] = {0xFAC775, 0xEF9F27, 0xFAEEDA},
    [PET_ANIMAL_PANDA] = {0xF1EFE8, 0x2C2C2A, 0xFFFFFF},
    [PET_ANIMAL_PENGUIN] = {0x2C2C2A, 0x185FA5, 0xF1EFE8},
};

static const uint32_t s_food_color[PET_ANIMAL_COUNT] = {
    [PET_ANIMAL_DOG] = 0xBA7517, [PET_ANIMAL_CAT] = 0x85B7EB, [PET_ANIMAL_RABBIT] = 0xEF9F27,
    [PET_ANIMAL_HAMSTER] = 0x888780, [PET_ANIMAL_PANDA] = 0x97C459, [PET_ANIMAL_PENGUIN] = 0x85B7EB,
};

uint16_t pet_rgb565(uint32_t c)
{
    /* Round each channel (as the GSP compiler does) so canvas pixels match
     * scene colours exactly. */
    const uint32_t r = (((c >> 16) & 0xFFu) * 31u + 127u) / 255u;
    const uint32_t g = (((c >> 8) & 0xFFu) * 63u + 127u) / 255u;
    const uint32_t b = ((c & 0xFFu) * 31u + 127u) / 255u;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

/* ---- builder: global transform + primitive emitters ---------------------- */

typedef struct {
    pet_scene_t *scene;
    float k;              /* art units -> pixels */
    float px, py;         /* pivot for squish/rotation (art units) */
    float sx, sy;         /* squish */
    float c, s;           /* rotation */
    float ox, oy;         /* translation (art units) */
} builder_t;

static void xform(const builder_t *b, float x, float y, float *ox, float *oy)
{
    float dx = (x - b->px) * b->sx;
    float dy = (y - b->py) * b->sy;
    const float rx = dx * b->c - dy * b->s;
    const float ry = dx * b->s + dy * b->c;
    *ox = (b->px + rx + b->ox) * b->k;
    *oy = (b->py + ry + b->oy) * b->k;
}

static pet_prim_t *emit(builder_t *b, pet_prim_kind_t kind, uint32_t color, uint8_t alpha)
{
    pet_scene_t *s = b->scene;
    if (s->count >= PET_SCENE_MAX_PRIMS || alpha == 0) {
        return NULL;
    }
    pet_prim_t *p = &s->prims[s->count++];
    memset(p, 0, sizeof(*p));
    p->kind = (uint8_t)kind;
    p->color = pet_rgb565(color);
    p->alpha = alpha;
    return p;
}

static void set_bbox(pet_scene_t *s, pet_prim_t *p, float x0, float y0, float x1, float y1)
{
    int ix0 = (int)floorf(x0) - 1, iy0 = (int)floorf(y0) - 1;
    int ix1 = (int)ceilf(x1) + 1, iy1 = (int)ceilf(y1) + 1;
    ix0 = ix0 < 0 ? 0 : ix0;
    iy0 = iy0 < 0 ? 0 : iy0;
    ix1 = ix1 > PET_CANVAS_SIZE - 1 ? PET_CANVAS_SIZE - 1 : ix1;
    iy1 = iy1 > PET_CANVAS_SIZE - 1 ? PET_CANVAS_SIZE - 1 : iy1;
    p->bbox[0] = (int16_t)ix0;
    p->bbox[1] = (int16_t)iy0;
    p->bbox[2] = (int16_t)ix1;
    p->bbox[3] = (int16_t)iy1;
    if (ix0 > ix1 || iy0 > iy1) {
        s->count--; /* fully off-canvas */
        return;
    }
    if (ix0 < s->dirty[0]) s->dirty[0] = (int16_t)ix0;
    if (iy0 < s->dirty[1]) s->dirty[1] = (int16_t)iy0;
    if (ix1 + 1 > s->dirty[2]) s->dirty[2] = (int16_t)(ix1 + 1);
    if (iy1 + 1 > s->dirty[3]) s->dirty[3] = (int16_t)(iy1 + 1);
}

static void ell_a(builder_t *b, float cx, float cy, float rx, float ry, float rot_deg,
                  uint32_t color, uint8_t alpha)
{
    pet_prim_t *p = emit(b, PET_PRIM_ELLIPSE, color, alpha);
    if (!p) {
        return;
    }
    float x, y;
    xform(b, cx, cy, &x, &y);
    const float r = rot_deg * DEG + atan2f(b->s, b->c);
    const float erx = rx * b->sx * b->k, ery = ry * b->sy * b->k;
    p->a[0] = x;
    p->a[1] = y;
    p->a[2] = erx > 0.5f ? erx : 0.5f;
    p->a[3] = ery > 0.5f ? ery : 0.5f;
    p->a[4] = cosf(r);
    p->a[5] = sinf(r);
    const float m = (erx > ery ? erx : ery);
    set_bbox(b->scene, p, x - m, y - m, x + m, y + m);
}

static void ell(builder_t *b, float cx, float cy, float rx, float ry, float rot, uint32_t color)
{
    ell_a(b, cx, cy, rx, ry, rot, color, 255);
}

static void tri_a(builder_t *b, float x0, float y0, float x1, float y1, float x2, float y2,
                  uint32_t color, uint8_t alpha)
{
    pet_prim_t *p = emit(b, PET_PRIM_TRIANGLE, color, alpha);
    if (!p) {
        return;
    }
    xform(b, x0, y0, &p->a[0], &p->a[1]);
    xform(b, x1, y1, &p->a[2], &p->a[3]);
    xform(b, x2, y2, &p->a[4], &p->a[5]);
    const float mnx = fminf(p->a[0], fminf(p->a[2], p->a[4]));
    const float mxx = fmaxf(p->a[0], fmaxf(p->a[2], p->a[4]));
    const float mny = fminf(p->a[1], fminf(p->a[3], p->a[5]));
    const float mxy = fmaxf(p->a[1], fmaxf(p->a[3], p->a[5]));
    set_bbox(b->scene, p, mnx, mny, mxx, mxy);
}

static void tri(builder_t *b, float x0, float y0, float x1, float y1, float x2, float y2, uint32_t color)
{
    tri_a(b, x0, y0, x1, y1, x2, y2, color, 255);
}

static void line_a(builder_t *b, float x0, float y0, float x1, float y1, float width,
                   uint32_t color, uint8_t alpha)
{
    pet_prim_t *p = emit(b, PET_PRIM_CAPSULE, color, alpha);
    if (!p) {
        return;
    }
    xform(b, x0, y0, &p->a[0], &p->a[1]);
    xform(b, x1, y1, &p->a[2], &p->a[3]);
    const float hw = width * 0.5f * b->k;
    p->a[4] = hw;
    set_bbox(b->scene, p, fminf(p->a[0], p->a[2]) - hw, fminf(p->a[1], p->a[3]) - hw,
             fmaxf(p->a[0], p->a[2]) + hw, fmaxf(p->a[1], p->a[3]) + hw);
}

static void line(builder_t *b, float x0, float y0, float x1, float y1, float width, uint32_t color)
{
    line_a(b, x0, y0, x1, y1, width, color, 255);
}

/* Quadratic Bezier stroke as a short capsule chain. */
static void curve_a(builder_t *b, float x0, float y0, float cx, float cy, float x1, float y1,
                    float width, uint32_t color, uint8_t alpha)
{
    enum { SEGMENTS = 6 };
    float lx = x0, ly = y0;
    for (int i = 1; i <= SEGMENTS; ++i) {
        const float t = (float)i / SEGMENTS, u = 1.0f - t;
        const float x = u * u * x0 + 2 * u * t * cx + t * t * x1;
        const float y = u * u * y0 + 2 * u * t * cy + t * t * y1;
        line_a(b, lx, ly, x, y, width, color, alpha);
        lx = x;
        ly = y;
    }
}

static void curve(builder_t *b, float x0, float y0, float cx, float cy, float x1, float y1,
                  float width, uint32_t color)
{
    curve_a(b, x0, y0, cx, cy, x1, y1, width, color, 255);
}

/* Lower half of an ellipse (open mouths). */
static void half_ell(builder_t *b, float cx, float cy, float rx, float ry, uint32_t color)
{
    enum { STEPS = 8 };
    for (int i = 0; i < STEPS; ++i) {
        const float a0 = PI_F * i / STEPS, a1 = PI_F * (i + 1) / STEPS;
        tri(b, cx, cy, cx + rx * cosf(a0), cy + ry * sinf(a0), cx + rx * cosf(a1), cy + ry * sinf(a1), color);
    }
}

static void heart_a(builder_t *b, float x, float y, float size, uint32_t color, uint8_t alpha)
{
    const float r = size * 0.3f;
    ell_a(b, x - r * 0.85f, y, r, r, 0, color, alpha);
    ell_a(b, x + r * 0.85f, y, r, r, 0, color, alpha);
    tri_a(b, x - size * 0.53f, y + r * 0.35f, x + size * 0.53f, y + r * 0.35f, x, y + size * 0.62f, color, alpha);
}

/* ---- pet parts ------------------------------------------------------------ */

static bool mood_in(pet_mood_t m, const pet_mood_t *set, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        if (set[i] == m) {
            return true;
        }
    }
    return false;
}
#define MOOD_IN(m, ...) mood_in((m), (const pet_mood_t[]){__VA_ARGS__}, \
    sizeof((const pet_mood_t[]){__VA_ARGS__}) / sizeof(pet_mood_t))

static void draw_tail(builder_t *b, const pet_pose_t *p, const palette_t *c)
{
    const float wag_speed = MOOD_IN(p->mood, PET_MOOD_HAPPY, PET_MOOD_LAUGH, PET_MOOD_LOVE, PET_MOOD_DANCE,
                                    PET_MOOD_EAT, PET_MOOD_PLAY) ? 14.0f : 3.0f;
    const float wag = sinf(p->time_s * wag_speed);
    switch (p->animal) {
    case PET_ANIMAL_DOG:
        if (p->mood == PET_MOOD_SAD || p->mood == PET_MOOD_SCARED) {
            ell(b, 156, 204, 7, 18, 20, c->dark);
        } else {
            ell(b, 162 + wag * 4, 182, 8, 20, 45 + wag * 25, c->dark);
        }
        break;
    case PET_ANIMAL_CAT: {
        const float sway = wag * 10;
        curve(b, 148, 202, 196, 200 + sway * 0.3f, 184 + sway, 160, 10, c->fur);
        line(b, 184 + sway, 160, 185 + sway, 156, 10, c->dark);
        break;
    }
    case PET_ANIMAL_RABBIT:
        ell(b, 156, 198, 11, 10, 0, WHITE);
        break;
    default:
        break;
    }
}

static void draw_body(builder_t *b, const pet_pose_t *p, const palette_t *c)
{
    const bool penguin = p->animal == PET_ANIMAL_PENGUIN;
    ell(b, 110, 190, penguin ? 52.0f : 46.0f, penguin ? 34.0f : 28.0f, 0, c->fur);
    ell(b, 110, 194, 28, 20, 0, c->belly);
    if (penguin) {
        const bool flap = MOOD_IN(p->mood, PET_MOOD_DANCE, PET_MOOD_SURPRISED, PET_MOOD_LAUGH, PET_MOOD_HAPPY);
        const float f = flap ? sinf(p->time_s * 12) * 20 - 30 : 15;
        ell(b, 60, 182, 10, 24, f, c->fur);
        ell(b, 160, 182, 10, 24, -f, c->fur);
        ell(b, 94, 216, 12, 5, 0, 0xEF9F27);
        ell(b, 126, 216, 12, 5, 0, 0xEF9F27);
        return;
    }
    const uint32_t paw = p->animal == PET_ANIMAL_PANDA ? c->dark : c->fur;
    const float step = p->running ? sinf(p->time_s * 16) * 6
                                  : p->mood == PET_MOOD_DANCE ? sinf(p->time_s * 9) * 4 : 0;
    ell(b, 86, 214 - step, 13, 8, 0, paw);
    ell(b, 134, 214 + step, 13, 8, 0, paw);
    if (p->animal == PET_ANIMAL_PANDA) {
        ell(b, 66, 186, 12, 18, 20, c->dark);
        ell(b, 154, 186, 12, 18, -20, c->dark);
    }
}

static void draw_ears(builder_t *b, const pet_pose_t *p, const palette_t *c)
{
    const bool droop = MOOD_IN(p->mood, PET_MOOD_SAD, PET_MOOD_BORED, PET_MOOD_SCARED, PET_MOOD_SLEEP,
                               PET_MOOD_SLEEPY);
    const bool perk = MOOD_IN(p->mood, PET_MOOD_LISTEN, PET_MOOD_SURPRISED);
    switch (p->animal) {
    case PET_ANIMAL_DOG: {
        const float flop = droop ? 10.0f : perk ? -14.0f : sinf(p->time_s * 2.5f) * 3;
        ell(b, 52, 88, 20, 38, 20 + flop, c->dark);
        ell(b, 168, 88, 20, 38, -20 - flop, c->dark);
        break;
    }
    case PET_ANIMAL_CAT: {
        const float d = droop ? 8.0f : 0.0f;
        tri(b, 48, 78, 58 - d, 30 + d, 92, 62, c->fur);
        tri(b, 172, 78, 162 + d, 30 + d, 128, 62, c->fur);
        tri(b, 60, 66, 63 - d, 44 + d, 80, 60, 0xF4C0D1);
        tri(b, 160, 66, 157 + d, 44 + d, 140, 60, 0xF4C0D1);
        break;
    }
    case PET_ANIMAL_RABBIT: {
        const float r = droop ? 55.0f : perk ? -4.0f : sinf(p->time_s * 1.7f) * 4;
        const float twitch = (p->mood == PET_MOOD_HAPPY && fmodf(p->time_s, 3.0f) < 0.2f) ? -10.0f : 0.0f;
        ell(b, 80, 34, 15, 44, -6 + twitch, c->fur);
        ell(b, 80, 36, 7, 32, -6 + twitch, 0xFBEAF0);
        /* Right ear folds over when sad. */
        const float cx = 140 + sinf(r * DEG) * 30, cy = 34 + (1 - cosf(r * DEG)) * 30;
        ell(b, cx, cy, 15, 44, 6 + r, c->fur);
        ell(b, cx, cy + 2, 7, 32, 6 + r, 0xFBEAF0);
        break;
    }
    case PET_ANIMAL_HAMSTER:
        ell(b, 62, 60, 16, 16, 0, c->dark);
        ell(b, 158, 60, 16, 16, 0, c->dark);
        ell(b, 62, 60, 8, 8, 0, 0xF4C0D1);
        ell(b, 158, 60, 8, 8, 0, 0xF4C0D1);
        break;
    case PET_ANIMAL_PANDA:
        ell(b, 58, 58, 20, 20, 0, c->dark);
        ell(b, 162, 58, 20, 20, 0, c->dark);
        break;
    default:
        break;
    }
}

static void draw_head(builder_t *b, const pet_pose_t *p, const palette_t *c)
{
    ell(b, 110, 112, 72, 66, 0, c->fur);
    if (p->animal == PET_ANIMAL_PENGUIN) {
        ell(b, 110, 122, 54, 44, 0, c->belly);
    } else if (p->animal == PET_ANIMAL_HAMSTER) {
        const float puff = p->mood == PET_MOOD_EAT ? 8 + sinf(p->time_s * 10) * 2 : 0;
        ell(b, 110, 136, 58 + puff, 34, 0, c->belly);
    }
}

static void eye_line(builder_t *b, float x, float y, float bend, uint32_t ink)
{
    curve(b, x - 11, y, x, y + bend, x + 11, y, 5, ink);
}

static void draw_eyes(builder_t *b, const pet_pose_t *p)
{
    const float L = 78, R = 142, Y = 112;
    const bool panda = p->animal == PET_ANIMAL_PANDA;
    if (panda) {
        ell(b, L, Y, 17, 20, -20, 0x2C2C2A);
        ell(b, R, Y, 17, 20, 20, 0x2C2C2A);
    }
    const uint32_t ink = panda ? WHITE : INK;
    const pet_mood_t m = p->mood;
    if (m == PET_MOOD_SLEEP || m == PET_MOOD_BORED) {
        eye_line(b, L, Y + 2, 8, ink);
        eye_line(b, R, Y + 2, 8, ink);
        return;
    }
    if (MOOD_IN(m, PET_MOOD_LAUGH, PET_MOOD_SMILE, PET_MOOD_DANCE)) {
        eye_line(b, L, Y + 4, -14, ink);
        eye_line(b, R, Y + 4, -14, ink);
        return;
    }
    if (m == PET_MOOD_LOVE) {
        const float beat = 1.0f + 0.12f * sinf(p->time_s * 8);
        heart_a(b, L, Y - 4, 26 * beat, HEART, 255);
        heart_a(b, R, Y - 4, 26 * beat, HEART, 255);
        return;
    }
    if (m == PET_MOOD_SHY) {
        line(b, L - 10, Y, L + 10, Y + 3, 5, ink);
        line(b, R + 10, Y, R - 10, Y + 3, 5, ink);
        return;
    }
    if (m == PET_MOOD_PROUD) {
        eye_line(b, L, Y + 4, -14, ink);
        eye_line(b, R, Y + 4, -14, ink);
        return;
    }
    if (m == PET_MOOD_SLEEPY) {
        line(b, L - 11, Y + 2, L + 11, Y + 2, 4, ink);
        line(b, R - 11, Y + 2, R + 11, Y + 2, 4, ink);
        return;
    }
    if (m == PET_MOOD_CONFUSED) {
        eye_line(b, L, Y - 3, -6, ink);
        eye_line(b, R, Y + 3, 6, ink);
        return;
    }
    if (p->blink > 0.5f) {
        eye_line(b, L, Y + 2, 4, ink);
        eye_line(b, R, Y + 2, 4, ink);
        return;
    }
    const bool small = MOOD_IN(m, PET_MOOD_SURPRISED, PET_MOOD_SCARED);
    const float rx = small ? 10.0f : 14.0f;
    const float ry = (small ? 14.0f : 17.0f) * (1.0f - 0.7f * p->blink) + (m == PET_MOOD_LISTEN ? 2 : 0);
    /* Pupils glance around gently. */
    const float gx = sinf(p->time_s * 0.7f) * 2.0f, gy = m == PET_MOOD_THINK ? -4.0f : 0.0f;
    for (int i = 0; i < 2; ++i) {
        const float x = i ? R : L;
        if (panda) {
            ell(b, x, Y, rx, ry, 0, WHITE);
            ell(b, x + gx, Y + gy, rx - 4, ry - 4, 0, INK);
        } else {
            ell(b, x + gx, Y + gy, rx, ry, 0, INK);
        }
        ell(b, x + gx - 4, Y + gy - 6, rx / 2.4f, rx / 2.4f, 0, WHITE);
        ell(b, x + gx + 4, Y + gy + 5, 2.5f, 2.5f, 0, WHITE);
    }
    if (m == PET_MOOD_ANGRY) {
        line(b, L - 14, Y - 22, L + 10, Y - 12, 5, panda ? WHITE : INK);
        line(b, R + 14, Y - 22, R - 10, Y - 12, 5, panda ? WHITE : INK);
    } else if (MOOD_IN(m, PET_MOOD_SAD, PET_MOOD_HUNGRY, PET_MOOD_SCARED)) {
        line(b, L - 12, Y - 18, L + 8, Y - 24, 4, panda ? WHITE : INK);
        line(b, R + 12, Y - 18, R - 8, Y - 24, 4, panda ? WHITE : INK);
    }
    if (m == PET_MOOD_SAD || m == PET_MOOD_SCARED) {
        const float drop = fmodf(p->mood_time_s * 18, 26);
        ell(b, L - 10, Y + 20 + drop, 4, 6, 0, TEAR);
    }
}

static void draw_cheeks(builder_t *b, const pet_pose_t *p)
{
    const bool shy = p->mood == PET_MOOD_SHY;
    if (!(shy || p->petting > 0 || MOOD_IN(p->mood, PET_MOOD_HAPPY, PET_MOOD_SMILE, PET_MOOD_LAUGH,
                                           PET_MOOD_LOVE, PET_MOOD_DANCE))) {
        return;
    }
    const float w = shy ? 14 : 10, h = shy ? 8 : 6;
    ell_a(b, 62, 134, w, h, 0, BLUSH, 190);
    ell_a(b, 158, 134, w, h, 0, BLUSH, 190);
}

static void draw_whiskers(builder_t *b, const pet_pose_t *p)
{
    if (p->animal != PET_ANIMAL_CAT && p->animal != PET_ANIMAL_RABBIT) {
        return;
    }
    const uint32_t col = 0x5F5E5A;
    line(b, 44, 130, 72, 134, 2, col);
    line(b, 44, 142, 72, 140, 2, col);
    line(b, 176, 130, 148, 134, 2, col);
    line(b, 176, 142, 148, 140, 2, col);
}

static void draw_nose(builder_t *b, const pet_pose_t *p)
{
    switch (p->animal) {
    case PET_ANIMAL_DOG:
        ell(b, 110, 130, 9, 6, 0, INK);
        ell(b, 107, 128, 3, 1.5f, 0, WHITE);
        break;
    case PET_ANIMAL_PANDA:
        ell(b, 110, 130, 7, 5, 0, 0x2C2C2A);
        break;
    case PET_ANIMAL_PENGUIN:
        break;
    default: {
        const float twitch = p->animal == PET_ANIMAL_RABBIT ? sinf(p->time_s * 14) * 0.8f : 0;
        tri(b, 104, 127 + twitch, 116, 127 + twitch, 110, 134 + twitch, HEART);
        break;
    }
    }
}

static void draw_mouth(builder_t *b, const pet_pose_t *p)
{
    const pet_mood_t m = p->mood;
    if (p->animal == PET_ANIMAL_PENGUIN) {
        const float open = fmaxf(p->mouth_open, MOOD_IN(m, PET_MOOD_LAUGH, PET_MOOD_SURPRISED, PET_MOOD_EAT,
                                                       PET_MOOD_SCARED) ? 0.7f : 0.0f);
        tri(b, 98, 136 - open * 4, 122, 136 - open * 4, 110, 128, 0xBA7517);
        tri(b, 98, 132, 122, 132, 110, 144 + open * 6, 0xEF9F27);
        return;
    }
    const bool talking = p->mouth_open > 0.08f && m != PET_MOOD_SLEEP && m != PET_MOOD_EAT;
    if (talking) {
        const float ry = 3 + 9 * p->mouth_open;
        half_ell(b, 110, 138, 9 + 3 * p->mouth_open, ry, MOUTH_RED);
        ell(b, 110, 138 + ry * 0.65f, 5, ry * 0.3f, 0, TONGUE);
        return;
    }
    switch (m) {
    case PET_MOOD_HAPPY:
        curve(b, 98, 138, 104, 147, 110, 138, 4, INK);
        curve(b, 110, 138, 116, 147, 122, 138, 4, INK);
        break;
    case PET_MOOD_SMILE:
    case PET_MOOD_LOVE:
        curve(b, 98, 138, 110, 149, 122, 138, 4, INK);
        break;
    case PET_MOOD_LAUGH:
    case PET_MOOD_DANCE: {
        const float bounce = m == PET_MOOD_LAUGH ? fabsf(sinf(p->time_s * 14)) * 3 : 0;
        half_ell(b, 110, 137, 14, 14 + bounce, MOUTH_RED);
        ell(b, 110, 148 + bounce * 0.6f, 7, 4, 0, TONGUE);
        break;
    }
    case PET_MOOD_SHY:
        curve(b, 104, 142, 110, 137, 116, 142, 3, INK);
        break;
    case PET_MOOD_SURPRISED:
        ell(b, 110, 146, 7, 9, 0, MOUTH_RED);
        break;
    case PET_MOOD_SCARED:
        line(b, 98, 146, 104, 140, 3, INK);
        line(b, 104, 140, 110, 146, 3, INK);
        line(b, 110, 146, 116, 140, 3, INK);
        line(b, 116, 140, 122, 146, 3, INK);
        break;
    case PET_MOOD_ANGRY:
        curve(b, 98, 147, 110, 136, 122, 147, 4, INK);
        break;
    case PET_MOOD_SAD:
        curve(b, 100, 147, 110, 139, 120, 147, 4, INK);
        break;
    case PET_MOOD_BORED: {
        /* Slow yawn. */
        const float y = fmaxf(0.0f, sinf(p->time_s * 0.8f));
        ell(b, 110, 145, 6 + 3 * y, 3 + 7 * y, 0, MOUTH_RED);
        break;
    }
    case PET_MOOD_HUNGRY:
        curve(b, 100, 141, 110, 147, 120, 141, 4, INK);
        ell(b, 118, 150 + fmodf(p->time_s * 4, 6), 3, 5, 0, TEAR);
        break;
    case PET_MOOD_EAT: {
        const float chew = fabsf(sinf(p->time_s * 10));
        ell(b, 110, 144, 10, 3 + 5 * chew, 0, MOUTH_RED);
        break;
    }
    case PET_MOOD_SLEEP: {
        const float snore = 3 + 2 * sinf(p->time_s * 1.6f);
        ell(b, 110, 144, snore, snore, 0, INK);
        break;
    }
    case PET_MOOD_THINK:
        line(b, 102, 143, 118, 141, 4, INK);
        break;
    case PET_MOOD_PLAY:
        half_ell(b, 110, 137, 12, 11 + 2 * fabsf(sinf(p->time_s * 12)), MOUTH_RED);
        ell(b, 110, 146, 6, 4, 0, TONGUE);
        break;
    case PET_MOOD_PROUD:
        curve(b, 98, 138, 110, 148, 122, 138, 4, INK);
        break;
    case PET_MOOD_CURIOUS:
        ell(b, 110, 144, 4, 5, 0, INK);
        break;
    case PET_MOOD_SLEEPY:
        curve(b, 104, 143, 110, 141, 116, 143, 3, INK);
        break;
    case PET_MOOD_CONFUSED:
        curve(b, 100, 143, 105, 139, 110, 143, 3, INK);
        curve(b, 110, 143, 115, 147, 120, 143, 3, INK);
        break;
    default:
        ell(b, 110, 143, 5, 4, 0, INK);
        break;
    }
    if (p->animal == PET_ANIMAL_DOG && MOOD_IN(m, PET_MOOD_HAPPY, PET_MOOD_DANCE, PET_MOOD_HUNGRY)) {
        const float pant = fabsf(sinf(p->time_s * 9)) * 3;
        ell(b, 110, 149 + pant, 6, 8 + pant, 0, TONGUE);
    }
    if ((p->animal == PET_ANIMAL_RABBIT || p->animal == PET_ANIMAL_HAMSTER) &&
            !MOOD_IN(m, PET_MOOD_LAUGH, PET_MOOD_SURPRISED, PET_MOOD_DANCE, PET_MOOD_BORED)) {
        ell(b, 107, 142, 3, 4, 0, WHITE);
        ell(b, 113, 142, 3, 4, 0, WHITE);
    }
}

static void draw_paws_over_face(builder_t *b, const pet_pose_t *p, const palette_t *c)
{
    if (p->mood != PET_MOOD_SHY && p->mood != PET_MOOD_SCARED) {
        return;
    }
    const uint32_t paw = p->animal == PET_ANIMAL_PANDA ? c->dark : p->animal == PET_ANIMAL_PENGUIN ? c->fur : c->dark;
    const float peek = p->mood == PET_MOOD_SHY ? sinf(p->time_s * 1.5f) * 6 : 10;
    ell(b, 80 - peek, 122, 16, 12, 0, paw);
    ell(b, 140 + peek, 122, 16, 12, 0, paw);
}

static void draw_bowl(builder_t *b, const pet_pose_t *p)
{
    if (p->mood != PET_MOOD_EAT && p->mood != PET_MOOD_HUNGRY) {
        return;
    }
    /* Bowl sits just under the bowed head, so the muzzle reaches into the food. */
    tri(b, 150, 168, 214, 168, 204, 184, BOWL);
    tri(b, 150, 168, 204, 184, 160, 184, BOWL);
    tri(b, 150, 168, 214, 168, 214, 170, 0x534AB7);
    if (p->mood != PET_MOOD_EAT) {
        return;
    }
    /* Food shrinks as it is eaten: a full dome down to a few crumbs. */
    const float food = p->food_left;
    if (food > 0.02f) {
        ell(b, 182, 170, 26 * (0.25f + 0.75f * food), 2 + 5 * food, 0, s_food_color[p->animal]);
    }
    /* Crumbs fly off while the pet chews. */
    for (int i = 0; i < 3; ++i) {
        const float phase = fmodf(p->time_s * 1.6f + i * 0.33f, 1.0f);
        const float x = 168 + i * 9 + phase * 10, y = 166 - phase * 18;
        ell_a(b, x, y, 2.2f, 2.2f, 0, s_food_color[p->animal], (uint8_t)(255 * (1.0f - phase)));
    }
}

/* ---- floating effects (drawn without the pet's sway) ------------------- */

static void draw_fx(builder_t *b, const pet_pose_t *p)
{
    const float t = p->time_s;
    const pet_mood_t m = p->mood;
    if (m == PET_MOOD_LOVE || p->petting > 0) {
        for (int i = 0; i < 3; ++i) {
            const float rise = fmodf(t * 22 + i * 24, 72);
            const uint8_t a = (uint8_t)(255 * (1 - rise / 72));
            heart_a(b, 160 + i * 16 + sinf(t * 3 + i) * 4, 70 - rise, 14 - i * 2, HEART, a);
        }
    }
    if (m == PET_MOOD_SLEEP) {
        for (int i = 0; i < 3; ++i) {
            const float rise = fmodf(t * 10 + i * 18, 54);
            const float s = 6 + rise * 0.15f;
            const float x = 160 + rise * 0.5f, y = 80 - rise;
            const uint8_t a = (uint8_t)(255 * (1 - rise / 54));
            line_a(b, x - s, y - s, x + s, y - s, 3, FX_SOFT, a);
            line_a(b, x + s, y - s, x - s, y + s, 3, FX_SOFT, a);
            line_a(b, x - s, y + s, x + s, y + s, 3, FX_SOFT, a);
        }
    }
    if (m == PET_MOOD_DANCE || m == PET_MOOD_LISTEN) {
        for (int i = 0; i < 2; ++i) {
            const float rise = fmodf(t * 16 + i * 30, 60);
            const float x = (i ? 34.0f : 176.0f) + sinf(t * 4 + i) * 5, y = 72 - rise;
            const uint8_t a = (uint8_t)(255 * (1 - rise / 60));
            ell_a(b, x, y, 5, 4, -20, FX_NOTE, a);
            line_a(b, x + 4, y - 1, x + 4, y - 18, 2.5f, FX_NOTE, a);
            line_a(b, x + 4, y - 18, x + 11, y - 13, 2.5f, FX_NOTE, a);
        }
    }
    if (m == PET_MOOD_SURPRISED || m == PET_MOOD_SCARED) {
        const float pop = fminf(1.0f, p->mood_time_s * 4);
        line(b, 182, 26, 180, 50 * pop, 6, FX_ALERT);
        ell(b, 180, 60, 3.5f, 3.5f, 0, FX_ALERT);
        if (m == PET_MOOD_SCARED) {
            ell(b, 168, 84, 5, 7, 0, TEAR);
            tri(b, 163, 82, 173, 82, 168, 72, TEAR);
        }
    }
    if (m == PET_MOOD_ANGRY) {
        const float s = 9 + sinf(t * 10) * 1.5f, x = 168, y = 56;
        curve(b, x - s, y - 3, x - 3, y - 3, x - 3, y - s, 3, 0xE24B4A);
        curve(b, x + s, y - 3, x + 3, y - 3, x + 3, y - s, 3, 0xE24B4A);
        curve(b, x - s, y + 3, x - 3, y + 3, x - 3, y + s, 3, 0xE24B4A);
        curve(b, x + s, y + 3, x + 3, y + 3, x + 3, y + s, 3, 0xE24B4A);
    }
    if (m == PET_MOOD_BORED || m == PET_MOOD_THINK) {
        for (int i = 0; i < 3; ++i) {
            const float pulse = 0.5f + 0.5f * sinf(t * 4 - i * 0.8f);
            ell_a(b, 158 + i * 12, 46, 3.5f, 3.5f, 0, FX_SOFT, (uint8_t)(90 + 165 * pulse));
        }
    }
    if (m == PET_MOOD_HUNGRY) {
        const float bob = sinf(t * 3) * 2;
        curve(b, 168, 40 + bob, 168, 26 + bob, 180, 28 + bob, 4, FX_ALERT);
        curve(b, 180, 28 + bob, 190, 32 + bob, 180, 44 + bob, 4, FX_ALERT);
        line(b, 180, 44 + bob, 180, 50 + bob, 4, FX_ALERT);
        ell(b, 180, 58 + bob, 2.5f, 2.5f, 0, FX_ALERT);
    }
    const float rx = p->roam_x;
    if (m == PET_MOOD_CURIOUS) {
        curve(b, 170 + rx, 38, 172 + rx, 22, 184 + rx, 30, 4, FX_NOTE);
        curve(b, 184 + rx, 30, 190 + rx, 38, 178 + rx, 46, 4, FX_NOTE);
        line(b, 178 + rx, 46, 178 + rx, 52, 4, FX_NOTE);
        ell(b, 178 + rx, 60, 2.5f, 2.5f, 0, FX_NOTE);
    }
    if (m == PET_MOOD_CONFUSED) {
        for (int i = 0; i < 3; ++i) {
            const float a = t * 4 + i * 2.1f;
            ell_a(b, 110 + rx + cosf(a) * 42, 40 + sinf(a) * 8, 4, 4, 0, FX_SOFT, 230);
        }
    }
    if (m == PET_MOOD_PROUD) {
        for (int i = 0; i < 2; ++i) {
            const float s = 6 + 3 * sinf(t * 6 + i * 2.0f);
            const float x = (i ? 170.0f : 58.0f) + rx, y = 60;
            line_a(b, x - s, y, x + s, y, 2.5f, FX_ALERT, 230);
            line_a(b, x, y - s, x, y + s, 2.5f, FX_ALERT, 230);
        }
    }
    if (m == PET_MOOD_PLAY && p->running) {
        for (int i = 0; i < 2; ++i) {
            const float ph = fmodf(t * 2 + i * 0.5f, 1.0f);
            const float dir = i ? 1.0f : -1.0f;
            ell_a(b, 110 + rx - dir * (20 + 40 * ph), 214 - 6 * ph, 5 + 6 * ph, 3, 0, FX_SOFT,
                  (uint8_t)(200 * (1 - ph)));
        }
    }
}

/* Ball or bone on the ground, or in the mouth when held. Drawn without the pet's sway. */
#define BALL_RED 0xE24B4A
#define BONE_WHITE 0xF1EFE8
static void draw_toy(builder_t *b, const pet_pose_t *p)
{
    if (p->toy == PET_TOY_NONE) {
        return;
    }
    const float x = 110 + p->toy_x, y = p->toy_held ? 160 : 208;
    if (p->toy == PET_TOY_BALL) {
        ell(b, x, y - 9, 9, 9, 0, BALL_RED);
        ell(b, x - 3, y - 12, 2.5f, 2.5f, 0, WHITE);
        return;
    }
    line(b, x - 9, y - 2, x + 9, y - 2, 5, BONE_WHITE);
    ell(b, x - 10, y - 6, 4, 4, 0, BONE_WHITE);
    ell(b, x - 10, y + 2, 4, 4, 0, BONE_WHITE);
    ell(b, x + 10, y - 6, 4, 4, 0, BONE_WHITE);
    ell(b, x + 10, y + 2, 4, 4, 0, BONE_WHITE);
}

/* A heap of mess on the floor, one per entry in poop_x. Light tan with a dark
 * outline so it reads against the dark background and the ground shadow. */
#define POOP_TAN 0xD9A066
#define POOP_OUTLINE 0x2A1A0E
#define POOP_TIP 0xF2D2A0
static void draw_poop(builder_t *b, const pet_pose_t *p)
{
    for (uint8_t i = 0; i < p->poop_count && i < PET_SCENE_POOP_MAX; ++i) {
        const float x = 110 + p->poop_x[i], y = 212;
        /* Dark outline first, then the tan heap on top. */
        ell(b, x, y, 19, 11, 0, POOP_OUTLINE);
        ell(b, x, y - 12, 14, 9, 0, POOP_OUTLINE);
        ell(b, x, y - 23, 9, 7, 0, POOP_OUTLINE);
        ell(b, x, y, 17, 9, 0, POOP_TAN);
        ell(b, x, y - 12, 12, 7, 0, POOP_TAN);
        ell(b, x, y - 23, 7, 5, 0, POOP_TAN);
        ell(b, x - 3, y - 25, 2.5f, 2.5f, 0, POOP_TIP);
    }
}

/* ---- scene assembly ----------------------------------------------------------- */

static void pose_motion(builder_t *b, const pet_pose_t *p)
{
    const float t = p->time_s, mt = p->mood_time_s;
    float ang = 0, ox = 0, oy = sinf(t * 3.2f) * 3.0f;
    float sy = 1.0f + 0.015f * sinf(t * 2.2f), sx = 1.0f;
    switch (p->mood) {
    case PET_MOOD_DANCE:
        ang = sinf(t * 9) * 9;
        oy = -fabsf(sinf(t * 9)) * 8;
        break;
    case PET_MOOD_ANGRY:
    case PET_MOOD_SCARED:
        ox = sinf(t * 45) * 2.5f;
        oy = 0;
        break;
    case PET_MOOD_SURPRISED:
        oy = mt < 0.6f ? -sinf(mt / 0.6f * PI_F) * 24 : 0;
        break;
    case PET_MOOD_SAD:
    case PET_MOOD_BORED:
        oy = 5 + sinf(t * 1.2f) * 1.5f;
        break;
    case PET_MOOD_SLEEP:
        oy = 6;
        sy = 1.0f + 0.03f * sinf(t * 1.6f);
        break;
    case PET_MOOD_LAUGH:
        oy = -fabsf(sinf(t * 12)) * 4;
        break;
    case PET_MOOD_EAT:
        /* Lean forward and bow the head down to the bowl, bobbing as it chews. */
        ox = 22;
        oy = 6 + fabsf(sinf(t * 10)) * 2;
        ang = 8 + sinf(t * 10) * 2;
        sy = 0.9f; /* a bowed, slightly shorter body keeps the paws on screen */
        break;
    case PET_MOOD_SHY:
        ang = sinf(t * 1.5f) * 4;
        break;
    case PET_MOOD_PLAY:
        oy = -fabsf(sinf(t * 8)) * 10;
        ang = sinf(t * 8) * 5;
        break;
    case PET_MOOD_PROUD:
        sy = 1.03f;
        break;
    case PET_MOOD_CURIOUS:
        ang = 10;
        break;
    case PET_MOOD_SLEEPY:
        oy = 4 + sinf(t * 0.9f) * 1.5f;
        ang = sinf(t * 0.5f) * 3;
        break;
    case PET_MOOD_CONFUSED:
        ang = sinf(t * 3) * 10;
        break;
    default:
        break;
    }
    if (p->petting > 0) {
        sy *= 1.0f - 0.05f * p->petting;
        sx *= 1.0f + 0.04f * p->petting;
    }
    b->c = cosf(ang * DEG);
    b->s = sinf(ang * DEG);
    b->ox = ox + p->roam_x;
    b->oy = oy;
    b->sx = sx;
    b->sy = sy;
}

void pet_scene_build(pet_scene_t *scene, const pet_pose_t *pose, uint16_t background)
{
    scene->count = 0;
    scene->background = background;
    scene->dirty[0] = scene->dirty[1] = PET_CANVAS_SIZE;
    scene->dirty[2] = scene->dirty[3] = 0;
    const pet_animal_t animal = (unsigned)pose->animal < PET_ANIMAL_COUNT ? pose->animal : PET_ANIMAL_DOG;
    pet_pose_t p = *pose;
    p.animal = animal;
    const palette_t *c = &s_palette[animal];

    builder_t b = {.scene = scene, .k = PET_CANVAS_SIZE / ART_UNITS, .px = 110, .py = 215,
                   .sx = 1, .sy = 1, .c = 1, .s = 0};
    /* Ground shadow does not sway. */
    ell_a(&b, 110 + pose->roam_x, 216, 54, 6, 0, 0x000000, 70);
    pose_motion(&b, &p);
    draw_tail(&b, &p, c);
    draw_body(&b, &p, c);
    draw_ears(&b, &p, c);
    draw_head(&b, &p, c);
    draw_eyes(&b, &p);
    draw_cheeks(&b, &p);
    draw_whiskers(&b, &p);
    draw_nose(&b, &p);
    draw_mouth(&b, &p);
    draw_paws_over_face(&b, &p, c);
    builder_t still = b;
    still.c = 1;
    still.s = 0;
    still.ox = still.oy = 0;
    still.sx = still.sy = 1;
    draw_bowl(&still, &p);
    draw_fx(&still, &p);
    draw_poop(&still, &p);
    draw_toy(&still, &p);
}

/* ---- rasterizer ------------------------------------------------------------ */

static inline float clamp01(float v)
{
    return v < 0 ? 0 : v > 1 ? 1 : v;
}

static float coverage(const pet_prim_t *p, float x, float y)
{
    switch (p->kind) {
    case PET_PRIM_ELLIPSE: {
        const float dx = x - p->a[0], dy = y - p->a[1];
        const float u = dx * p->a[4] + dy * p->a[5];
        const float v = -dx * p->a[5] + dy * p->a[4];
        const float rx2 = p->a[2] * p->a[2], ry2 = p->a[3] * p->a[3];
        const float f = u * u / rx2 + v * v / ry2;
        const float gu = u / rx2, gv = v / ry2;
        const float g = 2.0f * sqrtf(gu * gu + gv * gv);
        if (g < 1e-6f) {
            return 1.0f;
        }
        return clamp01(0.5f - (f - 1.0f) / g);
    }
    case PET_PRIM_TRIANGLE: {
        float d = -1e9f;
        const float area = (p->a[2] - p->a[0]) * (p->a[5] - p->a[1]) - (p->a[3] - p->a[1]) * (p->a[4] - p->a[0]);
        const float sign = area >= 0 ? 1.0f : -1.0f;
        for (int i = 0; i < 3; ++i) {
            const float x0 = p->a[i * 2], y0 = p->a[i * 2 + 1];
            const float x1 = p->a[((i + 1) % 3) * 2], y1 = p->a[((i + 1) % 3) * 2 + 1];
            const float ex = x1 - x0, ey = y1 - y0;
            const float len = sqrtf(ex * ex + ey * ey);
            if (len < 1e-6f) {
                continue;
            }
            const float e = -sign * (ex * (y - y0) - ey * (x - x0)) / len;
            d = e > d ? e : d;
        }
        return clamp01(0.5f - d);
    }
    default: {
        const float x0 = p->a[0], y0 = p->a[1], ex = p->a[2] - x0, ey = p->a[3] - y0;
        const float l2 = ex * ex + ey * ey;
        float t = l2 > 1e-6f ? ((x - x0) * ex + (y - y0) * ey) / l2 : 0.0f;
        t = clamp01(t);
        const float qx = x - (x0 + t * ex), qy = y - (y0 + t * ey);
        return clamp01(0.5f - (sqrtf(qx * qx + qy * qy) - p->a[4]));
    }
    }
}

static inline uint16_t blend565(uint16_t dst, uint16_t src, int a /*0..256*/)
{
    const int dr = dst >> 11, dg = (dst >> 5) & 0x3F, db = dst & 0x1F;
    const int sr = src >> 11, sg = (src >> 5) & 0x3F, sb = src & 0x1F;
    const int r = dr + (((sr - dr) * a) >> 8);
    const int g = dg + (((sg - dg) * a) >> 8);
    const int bl = db + (((sb - db) * a) >> 8);
    return (uint16_t)(((r & 0x1F) << 11) | ((g & 0x3F) << 5) | (bl & 0x1F));
}

void pet_scene_raster(const pet_scene_t *scene, uint16_t *pixels, size_t stride_bytes,
                      int x, int y, int w, int h)
{
    const size_t stride = stride_bytes / sizeof(uint16_t);
    for (int row = 0; row < h; ++row) {
        uint16_t *line_px = pixels + (size_t)row * stride;
        for (int col = 0; col < w; ++col) {
            line_px[col] = scene->background;
        }
    }
    for (uint16_t i = 0; i < scene->count; ++i) {
        const pet_prim_t *p = &scene->prims[i];
        const int x0 = p->bbox[0] > x ? p->bbox[0] : x;
        const int y0 = p->bbox[1] > y ? p->bbox[1] : y;
        const int x1 = p->bbox[2] < x + w - 1 ? p->bbox[2] : x + w - 1;
        const int y1 = p->bbox[3] < y + h - 1 ? p->bbox[3] : y + h - 1;
        for (int py = y0; py <= y1; ++py) {
            uint16_t *line_px = pixels + (size_t)(py - y) * stride - x;
            for (int px = x0; px <= x1; ++px) {
                const float cov = coverage(p, (float)px + 0.5f, (float)py + 0.5f);
                if (cov <= 0.0f) {
                    continue;
                }
                const int a = (int)(cov * (float)p->alpha * (256.0f / 255.0f) + 0.5f);
                line_px[px] = a >= 256 ? p->color : blend565(line_px[px], p->color, a);
            }
        }
    }
}

/* ---- clock face ---------------------------------------------------------- */

void pet_clock_build(pet_scene_t *scene, const pet_clock_t *clock)
{
    scene->count = 0;
    scene->dirty[0] = scene->dirty[1] = PET_CANVAS_SIZE;
    scene->dirty[2] = scene->dirty[3] = 0;
    const int h = clock->hour % 24;
    const bool night = h >= 20 || h < 5;
    const uint32_t sky = night ? 0x1B1F4B : h < 9 ? 0xFFB38A : h < 17 ? 0x7EC8E3 : 0x8E5BA8;
    const uint32_t band = night ? 0x2A2F6B : h < 9 ? 0xFFD2A6 : h < 17 ? 0xA9DDF0 : 0xB57BC8;
    scene->background = pet_rgb565(sky);
    builder_t b = {.scene = scene, .k = PET_CANVAS_SIZE / ART_UNITS, .px = 110, .py = 110,
                   .sx = 1, .sy = 1, .c = 1, .s = 0};
    /* Soft wallpaper bands and, at night, stars. */
    for (int i = 0; i < 4; ++i) {
        ell_a(&b, 110, 30 + i * 62.0f, 320, 26, 0, band, 150);
    }
    if (night) {
        static const float stars[8][2] = {{22, 26}, {188, 20}, {40, 190}, {196, 170},
                                          {170, 44}, {28, 110}, {200, 118}, {120, 206}};
        for (int i = 0; i < 8; ++i) {
            ell_a(&b, stars[i][0], stars[i][1], 1.6f, 1.6f, 0, 0xFFFFFF, 220);
        }
    }
    /* Face: white rim, cream dial. */
    ell_a(&b, 110, 110, 96, 96, 0, 0xFFFFFF, 235);
    ell_a(&b, 110, 110, 90, 90, 0, 0xF7F5F0, 255);
    /* Hour ticks: longer every quarter. */
    for (int i = 0; i < 12; ++i) {
        const float a = (i * 30.0f - 90.0f) * DEG;
        const float r1 = i % 3 == 0 ? 74.0f : 80.0f, r2 = 86.0f;
        line_a(&b, 110 + cosf(a) * r1, 110 + sinf(a) * r1, 110 + cosf(a) * r2, 110 + sinf(a) * r2,
               i % 3 == 0 ? 3.0f : 1.5f, 0x26215C, 255);
    }
    /* Hands: hour, minute, and a thin red second hand. */
    const float sec = (float)clock->second, min = (float)clock->minute;
    const float hour_a = ((float)(h % 12) + min / 60.0f) * 30.0f - 90.0f;
    const float min_a = (min + sec / 60.0f) * 6.0f - 90.0f;
    const float sec_a = sec * 6.0f - 90.0f;
    line_a(&b, 110 + cosf((hour_a + 180) * DEG) * 6, 110 + sinf((hour_a + 180) * DEG) * 6,
           110 + cosf(hour_a * DEG) * 48, 110 + sinf(hour_a * DEG) * 48, 6.0f, 0x26215C, 255);
    line_a(&b, 110 + cosf((min_a + 180) * DEG) * 8, 110 + sinf((min_a + 180) * DEG) * 8,
           110 + cosf(min_a * DEG) * 72, 110 + sinf(min_a * DEG) * 72, 4.0f, 0x26215C, 255);
    line_a(&b, 110 + cosf((sec_a + 180) * DEG) * 14, 110 + sinf((sec_a + 180) * DEG) * 14,
           110 + cosf(sec_a * DEG) * 82, 110 + sinf(sec_a * DEG) * 82, 1.5f, 0xE24B4A, 255);
    ell_a(&b, 110, 110, 4.5f, 4.5f, 0, 0xE24B4A, 255);
}
