// SPDX-License-Identifier: Apache-2.0
/* Renders every animal x mood into a PPM contact sheet for visual review. */
#include <stdio.h>
#include <stdlib.h>
#include "pet_render.h"

int main(int argc, char **argv)
{
    const int cols = PET_MOOD_COUNT, rows = PET_ANIMAL_COUNT, cell = PET_CANVAS_SIZE;
    const int W = cols * cell, H = rows * cell;
    unsigned char *rgb = malloc((size_t)W * H * 3);
    uint16_t *px = malloc(sizeof(uint16_t) * cell * cell);
    static pet_scene_t scene;
    for (int a = 0; a < rows; ++a) {
        for (int m = 0; m < cols; ++m) {
            pet_pose_t pose = {.animal = a, .mood = m, .time_s = 1.3f, .mood_time_s = 0.3f,
                               .food_left = 0.7f, .mouth_open = 0};
            pet_scene_build(&scene, &pose, pet_rgb565(0x1B1A2E));
            /* Raster in two halves to exercise partitioned drawing. */
            pet_scene_raster(&scene, px, cell * 2, 0, 0, cell, cell / 2);
            pet_scene_raster(&scene, px + cell * (cell / 2), cell * 2, 0, cell / 2, cell, cell / 2);
            for (int y = 0; y < cell; ++y) for (int x = 0; x < cell; ++x) {
                uint16_t c = px[y * cell + x];
                unsigned char *o = rgb + ((size_t)(a * cell + y) * W + m * cell + x) * 3;
                o[0] = (c >> 11) << 3; o[1] = ((c >> 5) & 0x3F) << 2; o[2] = (c & 0x1F) << 3;
            }
        }
    }
    FILE *f = fopen(argc > 1 ? argv[1] : "sheet.ppm", "wb");
    fprintf(f, "P6 %d %d 255\n", W, H);
    fwrite(rgb, 1, (size_t)W * H * 3, f);
    fclose(f);
    printf("prims max ok, wrote %dx%d\n", W, H);
    return 0;
}
