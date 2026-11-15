// SPDX-License-Identifier: Apache-2.0
#include "app_pet_market.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

static const char *TAG = "pet_market";

#define TASK_STACK 6144
#define TASK_PRIO 2
#define BUF_MAX (32 * 1024)
#define REFRESH_S (5 * 60)
#define RETRY_S 60
#define HEADLINES 12
#define NEWS_MAX 2048   /* headlines text, "\n"-separated */
#define START_DELAY_S 15  /* let Wi-Fi and SNTP come up first */

/* Large, influential firms and the main market gauges only. */
typedef struct {
    const char *symbol;
    const char *label;
    const char *unit;
} quote_t;

static const quote_t s_quotes[] = {
    {"^GSPC", "S&P 500", ""},
    {"GC=F", "Gold", "/oz"},
    {"SGD=X", "USD/SGD", ""},
    {"AAPL", "Apple", ""},
    {"MSFT", "Microsoft", ""},
    {"NVDA", "Nvidia", ""},
    {"GOOGL", "Alphabet", ""},
    {"AMZN", "Amazon", ""},
    {"JPM", "JPMorgan", ""},
};
#define QUOTE_COUNT (sizeof(s_quotes) / sizeof(s_quotes[0]))

#define QUOTE_URL "https://query1.finance.yahoo.com/v8/finance/chart/%s?range=1d&interval=1d"
#define NEWS_URL "https://feeds.bbci.co.uk/news/business/rss.xml"

static app_pet_market_cb_t s_cb;
static void *s_ctx;

/* GET url into buf (NUL-terminated); browsers' User-Agent is needed by the quote service. */
static int http_get(const char *url, char *buf, size_t cap)
{
    esp_http_client_config_t hc = {
        .url = url,
        .timeout_ms = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 1024,
    };
    esp_http_client_handle_t c = esp_http_client_init(&hc);
    if (!c) {
        return -1;
    }
    esp_http_client_set_header(c, "User-Agent", "Mozilla/5.0 (kawaii-pet)");
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

/* One quote row: "Apple|230.12  +1.2%" (name, then value; the UI splits on the bar).
 * Returns false when the quote is missing. */
static bool quote_line(const char *body, const quote_t *q, char *out, size_t cap)
{
    cJSON *j = cJSON_Parse(body);
    const cJSON *meta = NULL;
    const cJSON *result = j ? cJSON_GetObjectItemCaseSensitive(j, "chart") : NULL;
    const cJSON *arr = result ? cJSON_GetObjectItemCaseSensitive(result, "result") : NULL;
    if (cJSON_IsArray(arr)) {
        meta = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(arr, 0), "meta");
    }
    const cJSON *price = meta ? cJSON_GetObjectItemCaseSensitive(meta, "regularMarketPrice") : NULL;
    const cJSON *prev = meta ? cJSON_GetObjectItemCaseSensitive(meta, "chartPreviousClose") : NULL;
    bool ok = false;
    if (cJSON_IsNumber(price) && cJSON_IsNumber(prev) && prev->valuedouble != 0) {
        const double pct = (price->valuedouble - prev->valuedouble) / prev->valuedouble * 100.0;
        snprintf(out, cap, "%s|%.2f%s  %c%.2f%%", q->label, price->valuedouble, q->unit,
                 pct >= 0 ? '+' : '-', pct >= 0 ? pct : -pct);
        ok = true;
    }
    cJSON_Delete(j);
    return ok;
}

/* Replace the XML entities that headlines commonly contain. */
static void unescape(char *s)
{
    char *o = s;
    for (char *p = s; *p;) {
        if (strncmp(p, "&amp;", 5) == 0) { *o++ = '&'; p += 5; }
        else if (strncmp(p, "&quot;", 6) == 0) { *o++ = '"'; p += 6; }
        else if (strncmp(p, "&#39;", 5) == 0) { *o++ = '\''; p += 5; }
        else if (strncmp(p, "&apos;", 6) == 0) { *o++ = '\''; p += 6; }
        else if (strncmp(p, "&#8217;", 7) == 0) { *o++ = '\''; p += 7; }
        else if (strncmp(p, "&#8220;", 7) == 0 || strncmp(p, "&#8221;", 7) == 0) { *o++ = '"'; p += 7; }
        else { *o++ = *p++; }
    }
    *o = '\0';
}

/* Headlines from an RSS body: titles of the first HEADLINES items, '\n'-separated. */
static size_t headlines(const char *xml, char *out, size_t cap)
{
    size_t o = 0;
    int count = 0;
    const char *p = strstr(xml, "<item>");
    while (p && count < HEADLINES) {
        const char *t = strstr(p, "<title>");
        const char *next = strstr(p + 6, "<item>");
        if (!t || (next && t > next)) {
            p = next;
            continue;
        }
        t += 7;
        if (strncmp(t, "<![CDATA[", 9) == 0) {
            t += 9;
        }
        const char *end = strstr(t, "</title>");
        if (!end) {
            break;
        }
        char title[160];
        size_t n = (size_t)(end - t);
        if (n >= sizeof(title)) {
            n = sizeof(title) - 1;
        }
        memcpy(title, t, n);
        title[n] = '\0';
        char *cd = strstr(title, "]]>");
        if (cd) {
            *cd = '\0';
        }
        unescape(title);
        if (o + strlen(title) + 2 < cap) {
            o += (size_t)snprintf(out + o, cap - o, "%s%s", count ? "\n" : "", title);
            count++;
        }
        p = next;
    }
    return o;
}

static void refresh(char *buf, char *quotes, char *news)
{
    quotes[0] = '\0';
    size_t qo = 0;
    for (size_t i = 0; i < QUOTE_COUNT; ++i) {
        char url[160], line[64];
        snprintf(url, sizeof(url), QUOTE_URL, s_quotes[i].symbol);
        if (http_get(url, buf, BUF_MAX) == 200 && quote_line(buf, &s_quotes[i], line, sizeof(line))) {
            qo += (size_t)snprintf(quotes + qo, 512 - qo, "%s%s", i ? "\n" : "", line);
        } else {
            qo += (size_t)snprintf(quotes + qo, 512 - qo, "%s%s|--", i ? "\n" : "", s_quotes[i].label);
        }
        if (qo >= 511) {
            break;
        }
    }
    news[0] = '\0';
    if (http_get(NEWS_URL, buf, BUF_MAX) == 200) {
        headlines(buf, news, NEWS_MAX);
    }
    if (s_cb) {
        s_cb(quotes, news, s_ctx);
    }
}

static void market_task(void *arg)
{
    (void)arg;
    char *buf = heap_caps_malloc(BUF_MAX, MALLOC_CAP_SPIRAM);
    char *quotes = heap_caps_malloc(512, MALLOC_CAP_SPIRAM);
    char *news = heap_caps_malloc(NEWS_MAX, MALLOC_CAP_SPIRAM);
    if (!buf || !quotes || !news) {
        ESP_LOGE(TAG, "no memory for market data");
        vTaskDelete(NULL);
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(START_DELAY_S * 1000));
    for (;;) {
        refresh(buf, quotes, news);
        ESP_LOGI(TAG, "markets refreshed");
        vTaskDelay(pdMS_TO_TICKS((strlen(news) ? REFRESH_S : RETRY_S) * 1000));
    }
}

esp_err_t app_pet_market_start(app_pet_market_cb_t cb, void *ctx)
{
    ESP_RETURN_ON_FALSE(cb && !s_cb, ESP_ERR_INVALID_STATE, TAG, "bad state");
    s_cb = cb;
    s_ctx = ctx;
    ESP_RETURN_ON_FALSE(xTaskCreateWithCaps(market_task, "pet_market", TASK_STACK, NULL, TASK_PRIO, NULL,
                                            MALLOC_CAP_SPIRAM) == pdPASS, ESP_ERR_NO_MEM, TAG, "task");
    return ESP_OK;
}
