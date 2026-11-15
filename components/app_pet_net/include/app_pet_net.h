// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Network snapshot for the on-screen setup instructions. */
typedef struct {
    char setup_ssid[33];     /**< The pet's own setup network. */
    char setup_password[16];
    char setup_ip[16];       /**< Usually 192.168.4.1 */
    char sta_ssid[33];       /**< Hotspot being joined, "" when none. */
    bool online;             /**< Joined a network with an IP. */
    char sta_ip[16];
} app_pet_net_status_t;

/** Runs on the event or HTTP task; must not block. */
typedef void (*app_pet_net_status_cb_t)(const app_pet_net_status_t *status, void *ctx);

/** Start the setup access point, join the configured hotspot (or Vibe Mode's
 * saved network) and serve the setup page on port 80. Requires
 * app_pet_settings_init(). Once per boot. */
esp_err_t app_pet_net_start(app_pet_net_status_cb_t cb, void *ctx);

/** Re-read hotspot settings and reconnect (after a save). Thread-safe. */
void app_pet_net_reconnect(void);

/** True when the station has an IP. Thread-safe. */
bool app_pet_net_online(void);

#ifdef __cplusplus
}
#endif
