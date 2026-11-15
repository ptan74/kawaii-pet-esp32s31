// SPDX-License-Identifier: Apache-2.0
#include "app_pet_net.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "app_pet_settings.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "net_private.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "pet_net";

/* Vibe Mode keeps the user's network in the retained system NVS; read only. */
#define SHARED_PARTITION "sysmeta"
#define SHARED_NAMESPACE "shared"
#define RETRY_MIN_MS 1000U
#define RETRY_MAX_MS 30000U
#define SETUP_CHANNEL 6

/* Module singleton (one Wi-Fi stack per boot). `status` is written on the
 * default event task and by reconnect(); readers take copies through
 * notify(), serialized by `lock`. */
static struct {
    bool started;
    app_pet_net_status_cb_t cb;
    void *ctx;
    TimerHandle_t retry;
    uint32_t retry_ms;
    portMUX_TYPE lock;
    app_pet_net_status_t status;
    _Atomic bool online;
} s_net = {.lock = portMUX_INITIALIZER_UNLOCKED};

static void notify(void)
{
    app_pet_net_status_t copy;
    portENTER_CRITICAL(&s_net.lock);
    copy = s_net.status;
    portEXIT_CRITICAL(&s_net.lock);
    if (s_net.cb) {
        s_net.cb(&copy, s_net.ctx);
    }
}

bool app_pet_net_online(void)
{
    return atomic_load(&s_net.online);
}

/* ---- station credentials ---------------------------------------------------- */

static bool load_vibe_mode_network(wifi_config_t *cfg)
{
    if (nvs_flash_init_partition(SHARED_PARTITION) != ESP_OK) {
        return false;
    }
    nvs_handle_t h;
    if (nvs_open_from_partition(SHARED_PARTITION, SHARED_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t ssid_len = sizeof(cfg->sta.ssid), pass_len = sizeof(cfg->sta.password);
    esp_err_t err = nvs_get_str(h, "wifi_ssid", (char *)cfg->sta.ssid, &ssid_len);
    if (err == ESP_OK && nvs_get_str(h, "wifi_password", (char *)cfg->sta.password, &pass_len) != ESP_OK) {
        cfg->sta.password[0] = '\0';
    }
    nvs_close(h);
    return err == ESP_OK && cfg->sta.ssid[0];
}

/* Fills cfg from the phone hotspot setting, else Vibe Mode's network. */
static bool station_config(wifi_config_t *cfg)
{
    static app_pet_settings_t s; /* 1.2 KiB; only the event/HTTP path calls this */
    memset(cfg, 0, sizeof(*cfg));
    app_pet_settings_get(&s);
    bool ok = false;
    if (s.hotspot_ssid[0]) {
        strlcpy((char *)cfg->sta.ssid, s.hotspot_ssid, sizeof(cfg->sta.ssid));
        strlcpy((char *)cfg->sta.password, s.hotspot_password, sizeof(cfg->sta.password));
        ok = true;
    } else {
        ok = load_vibe_mode_network(cfg);
    }
    app_pet_settings_wipe(&s);
    if (ok) {
        cfg->sta.threshold.authmode = cfg->sta.password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
        cfg->sta.pmf_cfg.capable = true;
    }
    return ok;
}

static void apply_station(void)
{
    wifi_config_t cfg;
    const bool ok = station_config(&cfg);
    portENTER_CRITICAL(&s_net.lock);
    strlcpy(s_net.status.sta_ssid, ok ? (const char *)cfg.sta.ssid : "", sizeof(s_net.status.sta_ssid));
    s_net.status.online = false;
    s_net.status.sta_ip[0] = '\0';
    portEXIT_CRITICAL(&s_net.lock);
    atomic_store(&s_net.online, false);
    (void)esp_wifi_disconnect();
    if (ok) {
        ESP_LOGI(TAG, "joining \"%s\"", (const char *)cfg.sta.ssid);
        if (esp_wifi_set_config(WIFI_IF_STA, &cfg) == ESP_OK) {
            s_net.retry_ms = RETRY_MIN_MS;
            (void)esp_wifi_connect();
        }
    } else {
        ESP_LOGW(TAG, "no hotspot configured yet; waiting for phone setup");
    }
    memset(&cfg, 0, sizeof(cfg));
    notify();
}

void app_pet_net_reconnect(void)
{
    if (s_net.started) {
        apply_station();
    }
}

/* ---- events ------------------------------------------------------------------ */

static void retry_cb(TimerHandle_t t)
{
    (void)t;
    (void)esp_wifi_connect();
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *ev = data;
        const bool was_online = atomic_exchange(&s_net.online, false);
        portENTER_CRITICAL(&s_net.lock);
        s_net.status.online = false;
        s_net.status.sta_ip[0] = '\0';
        const bool configured = s_net.status.sta_ssid[0] != '\0';
        portEXIT_CRITICAL(&s_net.lock);
        if (configured) {
            ESP_LOGW(TAG, "disconnected (reason %u); retry in %u ms", ev->reason, (unsigned)s_net.retry_ms);
            xTimerChangePeriod(s_net.retry, pdMS_TO_TICKS(s_net.retry_ms), 0);
            s_net.retry_ms = s_net.retry_ms * 2 > RETRY_MAX_MS ? RETRY_MAX_MS : s_net.retry_ms * 2;
        }
        if (was_online) {
            notify();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *ev = data;
        portENTER_CRITICAL(&s_net.lock);
        snprintf(s_net.status.sta_ip, sizeof(s_net.status.sta_ip), IPSTR, IP2STR(&ev->ip_info.ip));
        s_net.status.online = true;
        portEXIT_CRITICAL(&s_net.lock);
        atomic_store(&s_net.online, true);
        s_net.retry_ms = RETRY_MIN_MS;
        ESP_LOGI(TAG, "online, ip " IPSTR, IP2STR(&ev->ip_info.ip));
        notify();
    }
}

/* ---- start ---------------------------------------------------------------------- */

static esp_err_t ok_if_exists(esp_err_t err)
{
    return err == ESP_ERR_INVALID_STATE ? ESP_OK : err;
}

esp_err_t app_pet_net_start(app_pet_net_status_cb_t cb, void *ctx)
{
    ESP_RETURN_ON_FALSE(!s_net.started, ESP_ERR_INVALID_STATE, TAG, "already started");
    s_net.cb = cb;
    s_net.ctx = ctx;
    s_net.retry_ms = RETRY_MIN_MS;
    s_net.retry = xTimerCreate("net_retry", pdMS_TO_TICKS(RETRY_MIN_MS), pdFALSE, NULL, retry_cb);
    ESP_RETURN_ON_FALSE(s_net.retry, ESP_ERR_NO_MEM, TAG, "timer");

    ESP_RETURN_ON_ERROR(ok_if_exists(esp_netif_init()), TAG, "netif");
    ESP_RETURN_ON_ERROR(ok_if_exists(esp_event_loop_create_default()), TAG, "event loop");
    ESP_RETURN_ON_FALSE(esp_netif_create_default_wifi_sta(), ESP_ERR_NO_MEM, TAG, "sta netif");
    ESP_RETURN_ON_FALSE(esp_netif_create_default_wifi_ap(), ESP_ERR_NO_MEM, TAG, "ap netif");
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init), TAG, "wifi init");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, on_event, NULL), TAG, "evt");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL), TAG, "evt");
    /* Credentials live in our own NVS namespace, never in Wi-Fi's flash store. */
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "storage");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "mode");

    /* Setup network: KawaiiPet-XXXX, WPA2 with a per-device password. */
    static app_pet_settings_t s;
    app_pet_settings_get(&s);
    uint8_t mac[6];
    ESP_RETURN_ON_ERROR(esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP), TAG, "mac");
    wifi_config_t ap = {.ap = {.channel = SETUP_CHANNEL, .max_connection = 2,
                               .authmode = WIFI_AUTH_WPA2_PSK, .pmf_cfg = {.capable = true}}};
    snprintf((char *)ap.ap.ssid, sizeof(ap.ap.ssid), "KawaiiPet-%02X%02X", mac[4], mac[5]);
    ap.ap.ssid_len = (uint8_t)strlen((const char *)ap.ap.ssid);
    strlcpy((char *)ap.ap.password, s.setup_password, sizeof(ap.ap.password));
    app_pet_settings_wipe(&s);
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap), TAG, "ap config");
    portENTER_CRITICAL(&s_net.lock);
    strlcpy(s_net.status.setup_ssid, (const char *)ap.ap.ssid, sizeof(s_net.status.setup_ssid));
    strlcpy(s_net.status.setup_password, s.setup_password, sizeof(s_net.status.setup_password));
    strlcpy(s_net.status.setup_ip, "192.168.4.1", sizeof(s_net.status.setup_ip));
    portEXIT_CRITICAL(&s_net.lock);

    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");
    s_net.started = true;
    ESP_RETURN_ON_ERROR(pet_setup_web_start(), TAG, "web");
    ESP_LOGI(TAG, "setup network \"%s\" ready", (const char *)ap.ap.ssid);
    apply_station();
    return ESP_OK;
}
