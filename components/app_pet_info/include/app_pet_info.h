// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Called with a short line such as "Singapore 31C Partly cloudy". */
typedef void (*app_pet_info_cb_t)(const char *weather_line, void *ctx);

/** Start network time (SNTP) and the location/weather refresh task (once).
 * The weather line is reported first from the task, then every 15 minutes. */
esp_err_t app_pet_info_start(app_pet_info_cb_t cb, void *ctx);

/** Seconds to add to UTC for local time; 0 until the first weather fetch. */
int app_pet_info_utc_offset(void);

#ifdef __cplusplus
}
#endif
