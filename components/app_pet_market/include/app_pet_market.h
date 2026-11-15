// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Called with two '\n'-separated blocks: market quotes (one line each) and
 * the newest business headlines. Either block may be "" when a fetch failed. */
typedef void (*app_pet_market_cb_t)(const char *quotes, const char *headlines, void *ctx);

/** Start the market and news task (once). Refreshes every 5 minutes. */
esp_err_t app_pet_market_start(app_pet_market_cb_t cb, void *ctx);

#ifdef __cplusplus
}
#endif
