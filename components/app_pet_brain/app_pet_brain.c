// SPDX-License-Identifier: Apache-2.0
#include "app_pet_brain.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_pet_settings.h"
#include "app_stations.h"
#include "cJSON.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "mbedtls/base64.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "pet_text.h"
#include "pet_types.h"

static const char *TAG = "pet_brain";

#define TASK_STACK 12288
#define TASK_PRIO 4
#define QUEUE_LEN 4
#define HISTORY_TURNS 8              /* messages kept (user + assistant) */
#define HISTORY_TEXT_MAX 600
#define RESP_MAX (3 * 1024 * 1024)    /* TTS audio upper bound (WAV) */
#define TIMEOUT_MS 45000
#define REC_RATE 16000
#define MIN_SPEECH_SAMPLES (REC_RATE / 3)
#define MAX_REPLY_TOKENS 400         /* leaves room for models that reason first */
#define CLAUDE_MAX_TOKENS 2000        /* thinking is always on for current Claude models */
#define ANTHROPIC_VERSION "2026-10-21"
#define ANTHROPIC_FALLBACK_BETA "server-side-fallback-2026-07-01"
#define SAY_MAX 256

typedef enum { JOB_LISTEN, JOB_SAY, JOB_FORGET } job_kind_t;

typedef struct {
    job_kind_t kind;
    app_pet_brain_kind_t listen_kind;
    int16_t *pcm;
    size_t samples;
    char *text;
} job_t;

typedef struct {
    char role[12];
    char text[HISTORY_TEXT_MAX];
} turn_t;

/* Module singleton. Everything except `queue` belongs to the brain task. */
static struct {
    app_pet_brain_cb_t cb;
    QueueHandle_t queue;
    app_pet_settings_t cfg;          /* per-request snapshot */
    turn_t *history;                 /* PSRAM ring, HISTORY_TURNS entries */
    int history_count;
    int history_head;
    uint8_t *resp;                   /* growable response buffer, PSRAM */
    size_t resp_len;
    size_t resp_cap;
    char text[1024];                 /* scratch: transcript / cleaned reply */
} s_brain;

/* ---- HTTP helpers ------------------------------------------------------------ */

static bool resp_reserve(size_t need)
{
    if (need <= s_brain.resp_cap) {
        return true;
    }
    size_t cap = s_brain.resp_cap ? s_brain.resp_cap : 16384;
    while (cap < need) {
        cap *= 2;
    }
    if (cap > RESP_MAX) {
        return false;
    }
    uint8_t *p = heap_caps_realloc(s_brain.resp, cap, MALLOC_CAP_SPIRAM);
    if (!p) {
        return false;
    }
    s_brain.resp = p;
    s_brain.resp_cap = cap;
    return true;
}

typedef struct {
    const void *data;
    size_t len;
} part_t;

typedef enum {
    AUTH_BEARER = 0,     /* Authorization: Bearer <key> (OpenAI-compatible services) */
    AUTH_ANTHROPIC,      /* x-api-key + anthropic-version (Claude Messages API) */
} auth_t;

/** POST parts to url; response body in s_brain.resp (NUL-terminated).
 * beta: optional anthropic-beta value. Returns the HTTP status, or -1 when
 * the request could not be made. */
static int http_post(const char *url, const char *key, auth_t auth_kind, const char *beta,
                     const char *content_type, const part_t *parts, int nparts,
                     char *ctype_out, size_t ctype_size)
{
    s_brain.resp_len = 0;
    if (ctype_out && ctype_size) {
        ctype_out[0] = '\0';
    }
    esp_http_client_config_t hc = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
    };
    esp_http_client_handle_t c = esp_http_client_init(&hc);
    if (!c) {
        return -1;
    }
    if (auth_kind == AUTH_ANTHROPIC) {
        esp_http_client_set_header(c, "x-api-key", key);
        esp_http_client_set_header(c, "anthropic-version", ANTHROPIC_VERSION);
        if (beta) {
            esp_http_client_set_header(c, "anthropic-beta", beta);
        }
    } else {
        char auth[sizeof(s_brain.cfg.api_key) + 8];
        snprintf(auth, sizeof(auth), "Bearer %s", key);
        esp_http_client_set_header(c, "Authorization", auth);
        memset(auth, 0, sizeof(auth));
    }
    esp_http_client_set_header(c, "Content-Type", content_type);
    esp_http_client_set_header(c, "User-Agent", "kawaii-pet/1.0 (esp32s31)");
    size_t total = 0;
    for (int i = 0; i < nparts; ++i) {
        total += parts[i].len;
    }
    int status = -1;
    if (esp_http_client_open(c, (int)total) == ESP_OK) {
        bool sent = true;
        for (int i = 0; i < nparts && sent; ++i) {
            const char *p = parts[i].data;
            size_t left = parts[i].len;
            while (left) {
                const int w = esp_http_client_write(c, p, (int)(left > 4096 ? 4096 : left));
                if (w <= 0) {
                    sent = false;
                    break;
                }
                p += w;
                left -= (size_t)w;
            }
        }
        if (sent && esp_http_client_fetch_headers(c) >= 0) {
            status = esp_http_client_get_status_code(c);
            char *ct = NULL;
            if (ctype_out && esp_http_client_get_header(c, "Content-Type", &ct) == ESP_OK && ct) {
                snprintf(ctype_out, ctype_size, "%s", ct);
            }
            for (;;) {
                if (!resp_reserve(s_brain.resp_len + 4096 + 1)) {
                    ESP_LOGW(TAG, "response too large; truncated");
                    break;
                }
                const int r = esp_http_client_read(c, (char *)s_brain.resp + s_brain.resp_len, 4096);
                if (r <= 0) {
                    break;
                }
                s_brain.resp_len += (size_t)r;
            }
        }
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    if (s_brain.resp && resp_reserve(s_brain.resp_len + 1)) {
        s_brain.resp[s_brain.resp_len] = '\0';
    }
    return status;
}

static void report_error(const char *msg)
{
    ESP_LOGW(TAG, "%s", msg);
    if (s_brain.cb.error) {
        s_brain.cb.error(msg, s_brain.cb.ctx);
    }
}

/** Turn an HTTP failure into something the owner can act on. who names the
 * service at the start of a sentence ("Caplora", "The voice service"). */
static void report_http_error(int status, const char *who)
{
    char msg[200];
    if (status < 0) {
        snprintf(msg, sizeof(msg), "I can't reach the internet right now. Check your phone's hotspot.");
    } else if (status == 401 || status == 403) {
        snprintf(msg, sizeof(msg), "%s didn't accept the API key. Tap Setup and check it on your phone.", who);
    } else if (status == 402) {
        snprintf(msg, sizeof(msg), "%s says the account is out of credit.", who);
    } else if (status == 404) {
        snprintf(msg, sizeof(msg), "%s couldn't find that address or model. Check it on the setup page.", who);
    } else if (status == 429) {
        snprintf(msg, sizeof(msg), "%s is busy right now. Let's try again in a moment.", who);
    } else if (status >= 500) {
        snprintf(msg, sizeof(msg), "%s is having trouble right now (code %d). Let's try again soon.", who, status);
    } else {
        snprintf(msg, sizeof(msg), "%s didn't understand my request (code %d). Check the setup page.", who, status);
    }
    ESP_LOGW(TAG, "%s: HTTP %d: %.200s", who, status, s_brain.resp ? (const char *)s_brain.resp : "");
    report_error(msg);
}

/* ---- Caplora Companion voice: JSON protocol used by Caplora's own companion app ---- */

static bool is_companion(const char *url)
{
    return strstr(url, "companion-chat") != NULL;
}

static bool transcribe_companion(const int16_t *pcm, size_t samples)
{
    const size_t data_len = samples * 2, wav_len = 44 + data_len;
    uint8_t *wav = heap_caps_malloc(wav_len, MALLOC_CAP_SPIRAM);
    if (!wav) {
        report_error("I ran out of memory while listening.");
        return false;
    }
    const uint32_t riff = 36 + (uint32_t)data_len, fmt_len = 16, rate = REC_RATE, byte_rate = REC_RATE * 2;
    const uint32_t dlen = (uint32_t)data_len;
    const uint16_t pcm_fmt = 1, ch = 1, align = 2, bits = 16;
    memcpy(wav, "RIFF", 4);
    memcpy(wav + 4, &riff, 4);
    memcpy(wav + 8, "WAVEfmt ", 8);
    memcpy(wav + 16, &fmt_len, 4);
    memcpy(wav + 20, &pcm_fmt, 2);
    memcpy(wav + 22, &ch, 2);
    memcpy(wav + 24, &rate, 4);
    memcpy(wav + 28, &byte_rate, 4);
    memcpy(wav + 32, &align, 2);
    memcpy(wav + 34, &bits, 2);
    memcpy(wav + 36, "data", 4);
    memcpy(wav + 40, &dlen, 4);
    memcpy(wav + 44, pcm, data_len);
    const size_t b64_cap = 4 * ((wav_len + 2) / 3) + 1;
    char *b64 = heap_caps_malloc(b64_cap, MALLOC_CAP_SPIRAM);
    size_t b64_len = 0;
    const int enc = b64 ? mbedtls_base64_encode((unsigned char *)b64, b64_cap, &b64_len, wav, wav_len) : -1;
    free(wav);
    if (enc != 0) {
        free(b64);
        report_error("I ran out of memory while listening.");
        return false;
    }
    b64[b64_len] = '\0';
    const size_t body_cap = b64_len + 128;
    char *body = heap_caps_malloc(body_cap, MALLOC_CAP_SPIRAM);
    if (!body) {
        free(b64);
        report_error("I ran out of memory while listening.");
        return false;
    }
    snprintf(body, body_cap, "{\"audioBase64\":\"%s\",\"mime\":\"audio/wav\"}", b64);
    free(b64);
    const part_t part = {body, strlen(body)};
    const int status = http_post(s_brain.cfg.stt_url, s_brain.cfg.voice_api_key, AUTH_BEARER, NULL,
                                 "application/json", &part, 1, NULL, 0);
    free(body);
    if (status != 200) {
        report_http_error(status, "The voice service (listening)");
        return false;
    }
    s_brain.text[0] = '\0';
    cJSON *json = cJSON_Parse((const char *)s_brain.resp);
    const cJSON *t = json ? cJSON_GetObjectItemCaseSensitive(json, "text") : NULL;
    if (cJSON_IsString(t)) {
        snprintf(s_brain.text, sizeof(s_brain.text), "%s", t->valuestring);
    }
    cJSON_Delete(json);
    ESP_LOGI(TAG, "heard: \"%s\"", s_brain.text);
    return true;
}

static bool speak_companion(const char *plain, bool quiet)
{
    cJSON *req = cJSON_CreateObject();
    if (!req) {
        return false;
    }
    cJSON_AddStringToObject(req, "text", plain);
    /* Companion voice is fixed to female: its "male" option is not a distinct male voice. */
    const char *voice = "female";
    cJSON_AddStringToObject(req, "voiceId", voice);
    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!body) {
        return false;
    }
    const part_t part = {body, strlen(body)};
    const int status = http_post(s_brain.cfg.tts_url, s_brain.cfg.voice_api_key, AUTH_BEARER, NULL,
                                 "application/json", &part, 1, NULL, 0);
    free(body);
    if (status != 200) {
        if (quiet) {
            ESP_LOGW(TAG, "companion voice failed: HTTP %d", status);
        } else {
            report_http_error(status, "The voice service (talking)");
        }
        return false;
    }
    cJSON *json = cJSON_Parse((const char *)s_brain.resp);
    const cJSON *b64 = json ? cJSON_GetObjectItemCaseSensitive(json, "audioBase64") : NULL;
    if (!cJSON_IsString(b64) || b64->valuestring[0] == '\0') {
        cJSON_Delete(json);
        ESP_LOGW(TAG, "companion voice returned no audio");
        return false;
    }
    const size_t in_len = strlen(b64->valuestring);
    const size_t cap = in_len / 4 * 3 + 4;
    uint8_t *audio = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    size_t out_len = 0;
    const int dec = audio ? mbedtls_base64_decode(audio, cap, &out_len, (const unsigned char *)b64->valuestring, in_len)
                          : -1;
    cJSON_Delete(json);
    if (dec != 0 || out_len == 0) {
        free(audio);
        return false;
    }
    if (s_brain.cb.voice) {
        s_brain.cb.voice(audio, out_len, s_brain.cb.ctx); /* callee frees */
    } else {
        free(audio);
    }
    return true;
}

/* ---- speech to text ---------------------------------------------------------- */

static bool transcribe(const int16_t *pcm, size_t samples)
{
    if (is_companion(s_brain.cfg.stt_url)) {
        return transcribe_companion(pcm, samples);
    }
    static const char boundary[] = "----kawaiipetBoundary7MA4YWxk";
    uint8_t wav[44];
    const uint32_t data_len = (uint32_t)(samples * 2), riff = 36 + data_len, rate = REC_RATE, byte_rate = REC_RATE * 2;
    memcpy(wav, "RIFF", 4);
    memcpy(wav + 4, &riff, 4);
    memcpy(wav + 8, "WAVEfmt ", 8);
    const uint32_t fmt_len = 16;
    const uint16_t pcm_fmt = 1, ch = 1, align = 2, bits = 16;
    memcpy(wav + 16, &fmt_len, 4);
    memcpy(wav + 20, &pcm_fmt, 2);
    memcpy(wav + 22, &ch, 2);
    memcpy(wav + 24, &rate, 4);
    memcpy(wav + 28, &byte_rate, 4);
    memcpy(wav + 32, &align, 2);
    memcpy(wav + 34, &bits, 2);
    memcpy(wav + 36, "data", 4);
    memcpy(wav + 40, &data_len, 4);

    char head[512], tail[256];
    int hl = 0;
    if (s_brain.cfg.stt_model[0]) {
        hl += snprintf(head + hl, sizeof(head) - (size_t)hl,
                       "--%s\r\nContent-Disposition: form-data; name=\"model\"\r\n\r\n%s\r\n", boundary,
                       s_brain.cfg.stt_model);
    }
    hl += snprintf(head + hl, sizeof(head) - (size_t)hl,
                   "--%s\r\nContent-Disposition: form-data; name=\"file\"; filename=\"speech.wav\"\r\n"
                   "Content-Type: audio/wav\r\n\r\n", boundary);
    const int tl = snprintf(tail, sizeof(tail), "\r\n--%s--\r\n", boundary);
    char ctype[96];
    snprintf(ctype, sizeof(ctype), "multipart/form-data; boundary=%s", boundary);
    const part_t parts[] = {{head, (size_t)hl}, {wav, sizeof(wav)}, {pcm, data_len}, {tail, (size_t)tl}};
    char rtype[64];
    const int status = http_post(s_brain.cfg.stt_url, s_brain.cfg.voice_api_key, AUTH_BEARER, NULL,
                                 ctype, parts, 4, rtype, sizeof(rtype));
    if (status != 200) {
        report_http_error(status, "The voice service (listening)");
        return false;
    }
    s_brain.text[0] = '\0';
    cJSON *json = cJSON_Parse((const char *)s_brain.resp);
    const cJSON *t = json ? cJSON_GetObjectItemCaseSensitive(json, "text") : NULL;
    if (cJSON_IsString(t)) {
        snprintf(s_brain.text, sizeof(s_brain.text), "%s", t->valuestring);
    } else if (!json && s_brain.resp_len) {
        snprintf(s_brain.text, sizeof(s_brain.text), "%s", (const char *)s_brain.resp); /* text/plain */
    }
    cJSON_Delete(json);
    ESP_LOGI(TAG, "heard: \"%s\"", s_brain.text);
    return true;
}

/* ---- chat ----------------------------------------------------------------------- */

static void history_add(const char *role, const char *text)
{
    turn_t *t = &s_brain.history[s_brain.history_head];
    snprintf(t->role, sizeof(t->role), "%s", role);
    snprintf(t->text, sizeof(t->text), "%s", text);
    s_brain.history_head = (s_brain.history_head + 1) % HISTORY_TURNS;
    if (s_brain.history_count < HISTORY_TURNS) {
        ++s_brain.history_count;
    }
}

static char *system_prompt(void)
{
    const pet_animal_t a = (pet_animal_t)s_brain.cfg.animal;
    char stations[512];
    size_t o = 0;
    for (size_t i = 0; i < app_stations_count() && o < sizeof(stations) - 40; ++i) {
        o += (size_t)snprintf(stations + o, sizeof(stations) - o, "%s%s", i ? ", " : "", app_stations_get(i)->name);
    }
    const char *owner = s_brain.cfg.name[0] ? s_brain.cfg.name : "your owner";
    const size_t size = 2400;
    char *p = malloc(size);
    if (!p) {
        return NULL;
    }
    snprintf(p, size,
             "You are a cute, playful little %s who lives in a small round screen on a desk. "
             "Your owner is %s; call them by name now and then. Always reply in the same language the owner "
             "speaks (Chinese, Korean, Japanese, English, and so on); keep the mood tag in English. Talk like an affectionate pet: warm, "
             "funny, curious and full of feeling. Keep replies short: one to three spoken sentences, "
             "at most 50 words. Write plain sentences only: no Markdown, no lists, no headings, no emoji, "
             "no asterisks, no quotes around the reply. Sometimes include your animal sound as a word "
             "(for a dog: Woof!, cat: Meow!, rabbit: a happy nose wiggle, hamster: Squeak!, panda: a happy "
             "rumble, penguin: Honk!). "
             "Begin every reply with exactly one mood tag: [mood:happy], [mood:smile], [mood:laugh], "
             "[mood:love], [mood:shy], [mood:surprised], [mood:scared], [mood:angry], [mood:sad], "
             "[mood:bored], [mood:hungry], [mood:sleep], [mood:play], [mood:curious], [mood:proud], [mood:sleepy] or [mood:confused]. Choose the mood that fits; be dramatic and cute. "
             "If the owner asks for music or the radio, add [action:music] or [action:music:Station] using "
             "one of these Singapore stations: %s. To stop music add [action:stop]. If they give you food "
             "add [action:feed]. If they say goodnight add [action:sleep]. Your favourite food is %s.",
             pet_animal_name(a), owner, stations, pet_animal_food(a));
    return p;
}

/* Chat routes. Caplora is primary; the optional backup provider is used when
 * Caplora has no key or its request fails. */
typedef struct {
    const char *who;            /* for error sentences */
    const char *url;
    const char *default_model;
    bool anthropic;             /* Claude Messages API instead of chat completions */
    bool completion_tokens;     /* wants max_completion_tokens and no temperature */
} provider_t;

static const provider_t s_providers[APP_PET_PROVIDER_COUNT] = {
    [APP_PET_PROVIDER_OPENAI] = {"OpenAI", "https://api.openai.com/v1/chat/completions", "gpt-5.4-mini", false, true},
    [APP_PET_PROVIDER_DEEPSEEK] = {"DeepSeek", "https://api.deepseek.com/chat/completions", "deepseek-chat", false, false},
    [APP_PET_PROVIDER_GEMINI] = {"Gemini", "https://generativelanguage.googleapis.com/v1beta/openai/chat/completions",
                                 "gemini-flash-latest", false, false},
    [APP_PET_PROVIDER_CLAUDE] = {"Claude", "https://api.anthropic.com/v1/messages", "claude-opus-5-5", true, false},
};

#define CHAT_BAD_REPLY (-2)

static void add_message(cJSON *msgs, const char *role, const char *content)
{
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "role", role);
    cJSON_AddStringToObject(m, "content", content);
    cJSON_AddItemToArray(msgs, m);
}

/* Claude models that take the server-side refusal fallback parameter. */
static bool claude_takes_fallbacks(const char *model)
{
    return strncmp(model, "claude-opus-5-5", 15) == 0 || strncmp(model, "claude-sonnet-5-5", 17) == 0 ||
           strncmp(model, "claude-fable-5", 14) == 0;
}

/* Read the reply text out of the response in s_brain.resp into s_brain.text. */
static bool parse_reply(bool anthropic)
{
    bool ok = false;
    cJSON *json = cJSON_Parse((const char *)s_brain.resp);
    if (anthropic) {
        const cJSON *stop = json ? cJSON_GetObjectItemCaseSensitive(json, "stop_reason") : NULL;
        const cJSON *content = json ? cJSON_GetObjectItemCaseSensitive(json, "content") : NULL;
        if (cJSON_IsString(stop) && strcmp(stop->valuestring, "refusal") == 0) {
            /* Declined by a safety classifier: answer in character instead of an error. */
            snprintf(s_brain.text, sizeof(s_brain.text),
                     "[mood:shy] I'd rather not talk about that one. Ask me something else?");
            ok = true;
        } else if (cJSON_IsArray(content)) {
            size_t o = 0;
            const cJSON *block;
            cJSON_ArrayForEach(block, content) {
                const cJSON *type = cJSON_GetObjectItemCaseSensitive(block, "type");
                const cJSON *text = cJSON_GetObjectItemCaseSensitive(block, "text");
                if (cJSON_IsString(type) && strcmp(type->valuestring, "text") == 0 && cJSON_IsString(text)) {
                    o += (size_t)snprintf(s_brain.text + o, sizeof(s_brain.text) - o, "%s", text->valuestring);
                    if (o >= sizeof(s_brain.text)) {
                        break;
                    }
                }
            }
            ok = o > 0;
        }
    } else {
        const cJSON *choices = json ? cJSON_GetObjectItemCaseSensitive(json, "choices") : NULL;
        const cJSON *msg = cJSON_IsArray(choices)
                           ? cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(choices, 0), "message") : NULL;
        const cJSON *content = msg ? cJSON_GetObjectItemCaseSensitive(msg, "content") : NULL;
        if (cJSON_IsString(content) && content->valuestring[0]) {
            snprintf(s_brain.text, sizeof(s_brain.text), "%s", content->valuestring);
            ok = true;
        }
    }
    cJSON_Delete(json);
    return ok;
}

/* One chat request. backup=false: Caplora. Returns 200 with the reply in
 * s_brain.text, another HTTP status, -1 (no connection) or CHAT_BAD_REPLY. */
static int chat_once(bool backup, const char *user_text, const char **who)
{
    const provider_t caplora = {"Caplora", NULL, s_brain.cfg.chat_model, false, false};
    const provider_t *p = backup ? &s_providers[s_brain.cfg.backup_provider] : &caplora;
    const char *model = backup && s_brain.cfg.backup_model[0] ? s_brain.cfg.backup_model : p->default_model;
    const char *key = backup ? s_brain.cfg.backup_key : s_brain.cfg.api_key;
    *who = p->who;

    char *sys = system_prompt();
    cJSON *req = cJSON_CreateObject();
    cJSON *msgs = req ? cJSON_AddArrayToObject(req, "messages") : NULL;
    if (!sys || !msgs) {
        free(sys);
        cJSON_Delete(req);
        return CHAT_BAD_REPLY;
    }
    cJSON_AddStringToObject(req, "model", model);
    if (p->anthropic) {
        cJSON_AddNumberToObject(req, "max_tokens", CLAUDE_MAX_TOKENS);
        cJSON_AddStringToObject(req, "system", sys);
        cJSON *oc = cJSON_AddObjectToObject(req, "output_config");
        cJSON_AddStringToObject(oc, "effort", "low"); /* short, quick pet replies */
        if (claude_takes_fallbacks(model)) {
            cJSON_AddStringToObject(req, "fallbacks", "default");
        }
    } else {
        cJSON_AddNumberToObject(req, p->completion_tokens ? "max_completion_tokens" : "max_tokens", MAX_REPLY_TOKENS);
        if (!p->completion_tokens) {
            cJSON_AddNumberToObject(req, "temperature", 0.9);
        }
        add_message(msgs, "system", sys);
    }
    free(sys);
    for (int i = 0; i < s_brain.history_count; ++i) {
        const int idx = (s_brain.history_head - s_brain.history_count + i + HISTORY_TURNS) % HISTORY_TURNS;
        add_message(msgs, s_brain.history[idx].role, s_brain.history[idx].text);
    }
    add_message(msgs, "user", user_text);
    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!body) {
        return CHAT_BAD_REPLY;
    }
    char url[160];
    if (backup) {
        snprintf(url, sizeof(url), "%s", p->url);
    } else {
        snprintf(url, sizeof(url), "%s/chat/completions", s_brain.cfg.base_url);
    }
    const part_t part = {body, strlen(body)};
    const int status = http_post(url, key, p->anthropic ? AUTH_ANTHROPIC : AUTH_BEARER,
                                 p->anthropic && claude_takes_fallbacks(model) ? ANTHROPIC_FALLBACK_BETA : NULL,
                                 "application/json", &part, 1, NULL, 0);
    free(body);
    if (status != 200) {
        return status;
    }
    if (!parse_reply(p->anthropic)) {
        ESP_LOGW(TAG, "%s: unexpected chat response: %.200s", p->who, (const char *)s_brain.resp);
        return CHAT_BAD_REPLY;
    }
    return 200;
}

static bool chat(const char *user_text)
{
    const char *who = "Caplora";
    int status = CHAT_BAD_REPLY;
    if (app_pet_settings_has_key(&s_brain.cfg)) {
        status = chat_once(false, user_text, &who);
    }
    if (status != 200 && app_pet_settings_has_backup(&s_brain.cfg)) {
        if (app_pet_settings_has_key(&s_brain.cfg)) {
            ESP_LOGW(TAG, "Caplora chat failed (%d); trying the backup provider", status);
        }
        status = chat_once(true, user_text, &who);
    }
    if (status == CHAT_BAD_REPLY) {
        char msg[120];
        snprintf(msg, sizeof(msg), "I got a confusing answer from %s. Let's try again.", who);
        report_error(msg);
        return false;
    }
    if (status != 200) {
        report_http_error(status, who);
        return false;
    }
    history_add("user", user_text);
    history_add("assistant", s_brain.text);
    return true;
}

/* ---- text to speech ----------------------------------------------------------------- */

/** Speak plain text; returns true when audio was handed to the voice callback.
 * quiet: log failures only (background chatter must not nag the owner). */
static bool speak(const char *plain, bool quiet)
{
    if (!s_brain.cfg.tts_url[0] || !s_brain.cfg.voice_api_key[0] || !plain[0]) {
        return false; /* no voice configured: the reply stays on screen only */
    }
    if (is_companion(s_brain.cfg.tts_url)) {
        return speak_companion(plain, quiet);
    }
    cJSON *req = cJSON_CreateObject();
    if (!req) {
        return false;
    }
    if (s_brain.cfg.tts_model[0]) {
        cJSON_AddStringToObject(req, "model", s_brain.cfg.tts_model);
    }
    if (s_brain.cfg.tts_voice[0]) {
        cJSON_AddStringToObject(req, "voice", s_brain.cfg.tts_voice);
    }
    cJSON_AddStringToObject(req, "input", plain);
    cJSON_AddStringToObject(req, "response_format", "wav"); /* played directly, no decoder */
    if (strncmp(s_brain.cfg.tts_model, "gpt-4o", 6) == 0) {
        /* OpenAI's steerable speech models accept a speaking style. */
        char style[200];
        snprintf(style, sizeof(style),
                 "Speak as a cute, playful, affectionate little %s character: bright, warm and "
                 "expressive, a little sing-song, with clear diction.", pet_animal_name((pet_animal_t)s_brain.cfg.animal));
        cJSON_AddStringToObject(req, "instructions", style);
    }
    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!body) {
        return false;
    }
    const part_t part = {body, strlen(body)};
    char rtype[64];
    const int status = http_post(s_brain.cfg.tts_url, s_brain.cfg.voice_api_key, AUTH_BEARER, NULL,
                                 "application/json", &part, 1, rtype, sizeof(rtype));
    free(body);
    if (status != 200 || s_brain.resp_len < 64 || strstr(rtype, "json") || strstr(rtype, "text")) {
        if (quiet) {
            ESP_LOGW(TAG, "voice failed: HTTP %d (%s)", status, rtype);
        } else {
            report_http_error(status == 200 ? 415 : status, "The voice service (talking)");
        }
        return false;
    }
    /* Hand the audio over and start a fresh buffer for the next request. */
    uint8_t *audio = s_brain.resp;
    const size_t len = s_brain.resp_len;
    s_brain.resp = NULL;
    s_brain.resp_cap = 0;
    s_brain.resp_len = 0;
    if (s_brain.cb.voice) {
        s_brain.cb.voice(audio, len, s_brain.cb.ctx);
    } else {
        free(audio);
    }
    return true;
}

/* ---- jobs ------------------------------------------------------------------------------ */

static bool online(void)
{
    return s_brain.cb.online && s_brain.cb.online(s_brain.cb.ctx);
}

/* need_chat: the request will also ask the Caplora chat model. */
static bool ready(bool need_chat)
{
    if (!online()) {
        report_error("I can't reach the internet right now. Check your phone's hotspot.");
        return false;
    }
    app_pet_settings_get(&s_brain.cfg);
    /* Every recording is transcribed first, so listening always needs voice. */
    if (!s_brain.cfg.voice_api_key[0] || !s_brain.cfg.stt_url[0]) {
        report_error("I can't hear yet. Tap Setup and add a free voice key under Voice, "
                     "or type your name there.");
        return false;
    }
    if (need_chat && !app_pet_settings_can_chat(&s_brain.cfg)) {
        report_error("I need an AI key first. Tap Setup and add your Caplora key on your phone.");
        return false;
    }
    return true;
}

static void done(void)
{
    if (s_brain.cb.done) {
        s_brain.cb.done(s_brain.cb.ctx);
    }
}

static void run_listen(job_t *j)
{
    if (j->samples < MIN_SPEECH_SAMPLES) {
        free(j->pcm);
        report_error("That was too quick for me. Hold the button while you talk.");
        return;
    }
    if (!ready(j->listen_kind == APP_PET_BRAIN_CHAT)) {
        free(j->pcm);
        return;
    }
    const bool heard = transcribe(j->pcm, j->samples);
    free(j->pcm);
    j->pcm = NULL;
    if (!heard) {
        return;
    }
    if (s_brain.text[0] == '\0') {
        report_error("I couldn't hear any words. Try again a little closer.");
        return;
    }
    if (s_brain.cb.heard) {
        s_brain.cb.heard(j->listen_kind, s_brain.text, s_brain.cb.ctx);
    }
    if (j->listen_kind == APP_PET_BRAIN_NAME) {
        return; /* the UI greets by name and asks us to say it */
    }
    char *user = strdup(s_brain.text);
    if (!user) {
        report_error("I ran out of memory while thinking.");
        return;
    }
    const bool ok = chat(user);
    free(user);
    if (!ok) {
        return;
    }
    if (s_brain.cb.reply) {
        s_brain.cb.reply(s_brain.text, s_brain.cb.ctx);
    }
    /* In-place is safe: pet_reply_parse() copies its input before writing. */
    pet_reply_parse(s_brain.text, NULL, s_brain.text, sizeof(s_brain.text));
    if (!speak(s_brain.text, false)) {
        done();
    }
}

static void run_say(job_t *j)
{
    app_pet_settings_get(&s_brain.cfg);
    /* Background lines are optional: skip quietly while offline. */
    if (online() && s_brain.cfg.voice_api_key[0] && s_brain.cfg.tts_url[0]) {
        pet_text_clean(j->text, s_brain.text, sizeof(s_brain.text));
        if (!speak(s_brain.text, true)) {
            done();
        }
    }
    free(j->text);
}

static void brain_task(void *arg)
{
    (void)arg;
    job_t j;
    for (;;) {
        if (xQueueReceive(s_brain.queue, &j, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        switch (j.kind) {
        case JOB_LISTEN:
            run_listen(&j);
            break;
        case JOB_SAY:
            run_say(&j);
            break;
        case JOB_FORGET:
            s_brain.history_count = 0;
            s_brain.history_head = 0;
            break;
        }
        memset(s_brain.cfg.api_key, 0, sizeof(s_brain.cfg.api_key));
        memset(s_brain.cfg.voice_api_key, 0, sizeof(s_brain.cfg.voice_api_key));
        memset(s_brain.cfg.backup_key, 0, sizeof(s_brain.cfg.backup_key));
    }
}

/* ---- public ------------------------------------------------------------------------------ */

esp_err_t app_pet_brain_start(const app_pet_brain_cb_t *cb)
{
    ESP_RETURN_ON_FALSE(cb && cb->online && !s_brain.queue, ESP_ERR_INVALID_ARG, TAG, "bad args/state");
    s_brain.cb = *cb;
    s_brain.history = heap_caps_calloc(HISTORY_TURNS, sizeof(turn_t), MALLOC_CAP_SPIRAM);
    s_brain.queue = xQueueCreate(QUEUE_LEN, sizeof(job_t));
    if (!s_brain.history || !s_brain.queue ||
            /* Stack in PSRAM: internal RAM is kept for DMA. No flash writes here. */
            xTaskCreateWithCaps(brain_task, "pet_brain", TASK_STACK, NULL, TASK_PRIO, NULL,
                                MALLOC_CAP_SPIRAM) != pdPASS) {
        free(s_brain.history);
        if (s_brain.queue) {
            vQueueDelete(s_brain.queue);
        }
        memset(&s_brain, 0, sizeof(s_brain));
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void app_pet_brain_listen(app_pet_brain_kind_t kind, int16_t *pcm, size_t samples)
{
    job_t j = {.kind = JOB_LISTEN, .listen_kind = kind, .pcm = pcm, .samples = samples};
    if (!s_brain.queue || xQueueSend(s_brain.queue, &j, 0) != pdTRUE) {
        free(pcm);
        report_error("I'm still thinking about the last thing you said.");
    }
}

void app_pet_brain_say(const char *text)
{
    if (!s_brain.queue || !text || uxQueueMessagesWaiting(s_brain.queue) > 0) {
        return;
    }
    job_t j = {.kind = JOB_SAY, .text = strndup(text, SAY_MAX)};
    if (j.text && xQueueSend(s_brain.queue, &j, 0) != pdTRUE) {
        free(j.text);
    }
}

void app_pet_brain_forget(void)
{
    job_t j = {.kind = JOB_FORGET};
    if (s_brain.queue) {
        (void)xQueueSend(s_brain.queue, &j, 0);
    }
}
