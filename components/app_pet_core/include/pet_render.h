// SPDX-License-Identifier: Apache-2.0
#pragma once

/* Procedural kawaii pet renderer. pet_scene_build() turns a pose into a list
 * of anti-aliased vector primitives; pet_scene_raster() paints any rectangle
 * of the PET_CANVAS_SIZE square RGB565 canvas from that list, so a renderer
 * that delivers the canvas in partitions sees one coherent frame. Portable. */

#include <stddef.h>
#include <stdint.h>
#include "pet_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PET_CANVAS_SIZE 240
#define PET_SCENE_MAX_PRIMS 160
#define PET_SCENE_POOP_MAX 3 /**< Keep equal to PET_POOP_MAX in pet_life.h. */

/** Animation inputs; all fractions are 0..1. */
typedef struct {
    pet_animal_t animal;
    pet_mood_t mood;
    float time_s;        /**< Monotonic animation clock. */
    float mood_time_s;   /**< Seconds since the mood started (jump/fx timing). */
    float blink;         /**< 0 open .. 1 closed. */
    float mouth_open;    /**< Talking lip-sync, 0 closed .. 1 open. */
    float food_left;     /**< Bowl contents while eating. */
    float petting;       /**< >0 while being stroked: hearts and squish. */
    float roam_x;        /**< Horizontal offset in art units while playing. */
    pet_toy_t toy;       /**< Toy on screen, or PET_TOY_NONE. */
    float toy_x;         /**< Toy offset from centre, art units. */
    bool toy_held;       /**< Toy in the pet's mouth. */
    bool running;        /**< Legs cycle faster. */
    uint8_t poop_count;  /**< Messes on the floor, 0..PET_SCENE_POOP_MAX. */
    float poop_x[PET_SCENE_POOP_MAX]; /**< Mess offsets from centre, art units. */
} pet_pose_t;

typedef enum {
    PET_PRIM_ELLIPSE = 0,
    PET_PRIM_TRIANGLE,
    PET_PRIM_CAPSULE,   /**< Thick line segment with round caps. */
} pet_prim_kind_t;

typedef struct {
    uint8_t kind;
    uint8_t alpha;      /**< 0..255 */
    uint16_t color;     /**< RGB565 */
    float a[6];         /**< Ellipse: cx, cy, rx, ry, cos, sin. Triangle: x0..y2.
                         *   Capsule: x0, y0, x1, y1, half-width. */
    int16_t bbox[4];    /**< x0, y0, x1, y1 inclusive, canvas pixels. */
} pet_prim_t;

typedef struct {
    pet_prim_t prims[PET_SCENE_MAX_PRIMS];
    uint16_t count;
    uint16_t background; /**< RGB565 */
    int16_t dirty[4];    /**< Union bbox of all prims (x0, y0, x1, y1 exclusive). */
} pet_scene_t;

/** Build the frame for pose; background is RGB565. */
void pet_scene_build(pet_scene_t *scene, const pet_pose_t *pose, uint16_t background);

/** Paint canvas-local rectangle (x, y, w, h) into pixels (RGB565, row
 * stride stride_bytes, pixels points at (x, y)). Fills every pixel. */
void pet_scene_raster(const pet_scene_t *scene, uint16_t *pixels, size_t stride_bytes,
                      int x, int y, int w, int h);

/** Wall clock shown on the clock screen. */
typedef struct {
    int hour;    /**< 0..23 */
    int minute;  /**< 0..59 */
    int second;  /**< 0..59 */
} pet_clock_t;

/** Build the wallpaper (by time of day), analog face and hands. */
void pet_clock_build(pet_scene_t *scene, const pet_clock_t *clock);

/** 0xRRGGBB -> RGB565. */
uint16_t pet_rgb565(uint32_t rgb888);

#ifdef __cplusplus
}
#endif
