// SPDX-License-Identifier: Apache-2.0
#include "app_pet_info.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

static const char *TAG = "pet_info";

#define TASK_STACK 6144
#define TASK_PRIO 2
#define RESP_MAX 4096
#define REFRESH_S (15 * 60)
#define RETRY_S 60
#define HTTP_TIMEOUT_MS 15000

/* Location from IP (no key), current weather from Open-Meteo (no key). */
#define LOCATION_URL "http://ip-api.com/json/?fields=status,city,lat,lon"
#define WEATHER_FMT "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f" \
                    "&current=temperature_2m,weather_code&timezone=auto"

static struct {
    app_pet_info_cb_t cb;
    void *ctx;
    _Atomic int utc_offset;
} s_info;

/* GET url; body copied into buf (NUL-terminated). Returns HTTP status or -1. */
static int http_get(const char *url, char *buf, size_t cap)
{
    esp_http_client_config_t hc = {
        .url = url,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 1024,
    };
    esp_http_client_handle_t c = esp_http_client_init(&hc);
    if (!c) {
        return -1;
    }
    int status = -1;
    size_t len = 0;
    if (esp_http_client_open(c, 0) == ESP_OK && esp_http_client_fetch_headers(c) >= 0) {
        status = esp_http_client_get_status_code(c);
        while (len + 1 < cap) {
            const int r = esp_http_client_read(c, buf + len, (int)(cap - 1 - len));
            if (r <= 0) {
                break;
            }
            len += (size_t)r;
        }
    }
    buf[len] = '\0';
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return status;
}

/* WMO weather codes (Open-Meteo) to short English descriptions. */
static const char *describe(int code)
{
    if (code == 0) return "Clear";
    if (code <= 2) return "Partly cloudy";
    if (code == 3) return "Cloudy";
    if (code <= 48) return "Fog";
    if (code <= 67) return "Rain";
    if (code <= 77) return "Snow";
    if (code <= 82) return "Showers";
    return "Thunderstorm";
}

static bool refresh(char *buf)
{
    if (http_get(LOCATION_URL, buf, RESP_MAX) != 200) {
        return false;
    }
    cJSON *loc = cJSON_Parse(buf);
    const cJSON *city = loc ? cJSON_GetObjectItemCaseSensitive(loc, "city") : NULL;
    const cJSON *lat = loc ? cJSON_GetObjectItemCaseSensitive(loc, "lat") : NULL;
    const cJSON *lon = loc ? cJSON_GetObjectItemCaseSensitive(loc, "lon") : NULL;
    if (!cJSON_IsString(city) || !cJSON_IsNumber(lat) || !cJSON_IsNumber(lon)) {
        cJSON_Delete(loc);
        return false;
    }
    char place[48];
    snprintf(place, sizeof(place), "%s", city->valuestring);
    char url[224];
    snprintf(url, sizeof(url), WEATHER_FMT, lat->valuedouble, lon->valuedouble);
    cJSON_Delete(loc);

    if (http_get(url, buf, RESP_MAX) != 200) {
        return false;
    }
    cJSON *w = cJSON_Parse(buf);
    const cJSON *cur = w ? cJSON_GetObjectItemCaseSensitive(w, "current") : NULL;
    const cJSON *temp = cur ? cJSON_GetObjectItemCaseSensitive(cur, "temperature_2m") : NULL;
    const cJSON *code = cur ? cJSON_GetObjectItemCaseSensitive(cur, "weather_code") : NULL;
    const cJSON *off = w ? cJSON_GetObjectItemCaseSensitive(w, "utc_offset_seconds") : NULL;
    if (!cJSON_IsNumber(temp) || !cJSON_IsNumber(code)) {
        cJSON_Delete(w);
        return false;
    }
    if (cJSON_IsNumber(off)) {
        atomic_store(&s_info.utc_offset, (int)off->valuedouble);
    }
    char line[96];
    snprintf(line, sizeof(line), "%s %dC %s", place, (int)(temp->valuedouble + (temp->valuedouble >= 0 ? 0.5 : -0.5)),
             describe((int)code->valuedouble));
    cJSON_Delete(w);
    if (s_info.cb) {
        s_info.cb(line, s_info.ctx);
    }
    return true;
}

static void info_task(void *arg)
{
    (void)arg;
    char *buf = heap_caps_malloc(RESP_MAX, MALLOC_CAP_SPIRAM);
    if (!buf) {
        vTaskDelete(NULL);
        return;
    }
    for (;;) {
        const bool ok = refresh(buf);
        ESP_LOGI(TAG, "weather refresh %s", ok ? "ok" : "failed");
        vTaskDelay(pdMS_TO_TICKS((ok ? REFRESH_S : RETRY_S) * 1000));
    }
}

esp_err_t app_pet_info_start(app_pet_info_cb_t cb, void *ctx)
{
    ESP_RETURN_ON_FALSE(!s_info.cb && !s_info.ctx && cb, ESP_ERR_INVALID_STATE, TAG, "already started");
    s_info.cb = cb;
    s_info.ctx = ctx;
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();
    ESP_RETURN_ON_FALSE(xTaskCreateWithCaps(info_task, "pet_info", TASK_STACK, NULL, TASK_PRIO, NULL,
                                            MALLOC_CAP_SPIRAM) == pdPASS, ESP_ERR_NO_MEM, TAG, "task");
    return ESP_OK;
}

int app_pet_info_utc_offset(void)
{
    return atomic_load(&s_info.utc_offset);
}
