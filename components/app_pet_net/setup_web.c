// SPDX-License-Identifier: Apache-2.0
/* Phone setup page: GET / shows a form, POST /save stores it. The API key and
 * passwords are never sent back to the browser; leaving them blank keeps the
 * saved values. Served on both the setup network and the hotspot. */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "app_pet_net.h"
#include "app_pet_settings.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "net_private.h"

static const char *TAG = "pet_web";

#define BODY_MAX 4096

/* Only the httpd task touches these (one request at a time). */
static app_pet_settings_t s_form;
static char s_esc[512];

static const char PAGE_HEAD[] =
    "<!doctype html><html><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Kawaii Pet setup</title><style>"
    "body{font-family:-apple-system,system-ui,sans-serif;background:#1B1A2E;color:#F1EFE8;margin:0;padding:18px}"
    "h1{font-size:22px;margin:4px 0 2px}p.s{color:#B4B2A9;margin:0 0 14px;font-size:14px}"
    "fieldset{border:1px solid #3C3489;border-radius:12px;margin:0 0 14px;padding:10px 14px}"
    "legend{color:#CECBF6;padding:0 6px;font-weight:600}"
    "label{display:block;font-size:13px;color:#B4B2A9;margin:10px 0 4px}"
    "input,select{width:100%;box-sizing:border-box;font-size:16px;padding:10px;border-radius:8px;"
    "border:1px solid #534AB7;background:#26244a;color:#fff}"
    "input[type=checkbox]{width:auto}input[type=range]{padding:0}"
    ".row{display:flex;gap:8px;align-items:center}.hint{font-size:12px;color:#888780;margin-top:4px}"
    "button{width:100%;font-size:18px;padding:14px;border:0;border-radius:24px;background:#EF9F27;"
    "color:#412402;font-weight:700;margin-top:6px}.ok{background:#173404;color:#C0DD97;padding:10px;"
    "border-radius:8px;margin-bottom:12px}</style></head><body>"
    "<h1>Kawaii Pet setup</h1><p class=s>Settings are saved on your pet, not online.</p>";

static const char *const s_models[] = {
    "claude-haiku-5-5", "gpt-6-luna", "deepseek-v4.1-flash", "gpt-5.4-mini", "claude-sonnet-5-5",
    "gpt-6-sol", "gpt-5.4", "gpt-5.5", "deepseek-v4-pro", "grok-4.5", "kimi-k3", "claude-opus-5-5",
};

static const char *esc(const char *in)
{
    size_t o = 0;
    for (const char *p = in; *p && o + 7 < sizeof(s_esc); ++p) {
        const char *r = NULL;
        switch (*p) {
        case '&': r = "&amp;"; break;
        case '<': r = "&lt;"; break;
        case '>': r = "&gt;"; break;
        case '"': r = "&quot;"; break;
        case '\'': r = "&#39;"; break;
        default: break;
        }
        if (r) {
            o += (size_t)snprintf(s_esc + o, sizeof(s_esc) - o, "%s", r);
        } else {
            s_esc[o++] = *p;
        }
    }
    s_esc[o] = '\0';
    return s_esc;
}

static esp_err_t chunk(httpd_req_t *req, const char *text)
{
    /* A zero-length chunk ends a chunked response; never send one mid-page. */
    if (!text || text[0] == '\0') {
        return ESP_OK;
    }
    return httpd_resp_sendstr_chunk(req, text);
}

static esp_err_t field(httpd_req_t *req, const char *label, const char *name, const char *type,
                       const char *value, const char *placeholder, const char *hint)
{
    char buf[256];
    chunk(req, "<label>");
    chunk(req, label);
    snprintf(buf, sizeof(buf), "</label><input name=%s type=%s autocomplete=off autocapitalize=off placeholder='",
             name, type);
    chunk(req, buf);
    chunk(req, esc(placeholder ? placeholder : ""));
    chunk(req, "' value='");
    chunk(req, esc(value ? value : ""));
    chunk(req, "'>");
    if (hint) {
        chunk(req, "<div class=hint>");
        chunk(req, hint);
        chunk(req, "</div>");
    }
    return ESP_OK;
}

static esp_err_t page_get(httpd_req_t *req)
{
    app_pet_settings_get(&s_form);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    chunk(req, PAGE_HEAD);
    if (strstr(req->uri, "saved=1")) {
        chunk(req, "<div class=ok>Saved. Your pet is updating now.</div>");
    }
    chunk(req, "<form method=post action=/save>");

    chunk(req, "<fieldset><legend>Your pet</legend><label>Animal</label><select name=animal>");
    for (int i = 0; i < PET_ANIMAL_COUNT; ++i) {
        char opt[96];
        snprintf(opt, sizeof(opt), "<option value=%s%s>%s</option>", pet_animal_id((pet_animal_t)i),
                 i == s_form.animal ? " selected" : "", pet_animal_name((pet_animal_t)i));
        chunk(req, opt);
    }
    chunk(req, "</select>");
    field(req, "Your name", "name", "text", s_form.name, "Patrick", "The pet uses it to greet you.");
    char vol[160];
    snprintf(vol, sizeof(vol), "<label>Volume</label><input name=volume type=range min=0 max=100 value=%u>",
             (unsigned)s_form.volume);
    chunk(req, vol);
    chunk(req, s_form.talk_on_its_own ? "<label class=row><input type=checkbox name=talk_own value=1 checked>"
                                      : "<label class=row><input type=checkbox name=talk_own value=1>");
    chunk(req, " Talk out loud on its own (uses a little more credit)</label></fieldset>");

    chunk(req, "<fieldset><legend>Phone hotspot</legend>");
    field(req, "Hotspot name", "hotspot_ssid", "text", s_form.hotspot_ssid, "Patrick's iPhone", NULL);
    field(req, "Hotspot password", "hotspot_password", "password", "",
          s_form.hotspot_password[0] ? "saved - leave blank to keep" : "", NULL);
    chunk(req, "</fieldset>");

    chunk(req, "<fieldset><legend>Caplora Lab (main AI)</legend>");
    field(req, "API key", "api_key", "password", "",
          s_form.api_key[0] ? "saved - leave blank to keep" : "cap_...", "From caploralab.online. Never shown again.");
    chunk(req, "<label>Chat model</label><input name=chat_model list=models value='");
    chunk(req, esc(s_form.chat_model));
    chunk(req, "'><datalist id=models>");
    for (size_t i = 0; i < sizeof(s_models) / sizeof(s_models[0]); ++i) {
        char opt[80];
        snprintf(opt, sizeof(opt), "<option value='%s'>", s_models[i]);
        chunk(req, opt);
    }
    chunk(req, "</datalist>");
    field(req, "API base URL", "base_url", "url", s_form.base_url, "https://caploralab.online/api/v1", NULL);
    /* Optional second provider, used when Caplora has no key or is down. */
    static const char *const providers[APP_PET_PROVIDER_COUNT] = {
        "None", "ChatGPT (OpenAI)", "DeepSeek", "Gemini (Google)", "Claude (Anthropic)",
    };
    chunk(req, "</fieldset><fieldset><legend>Other AI key (optional backup)</legend>"
               "<div class=hint>Used when Caplora has no key or is not answering.</div>"
               "<label>Provider</label><select name=backup_provider>");
    for (int i = 0; i < APP_PET_PROVIDER_COUNT; ++i) {
        char opt[96];
        snprintf(opt, sizeof(opt), "<option value=%d%s>%s</option>", i,
                 i == s_form.backup_provider ? " selected" : "", providers[i]);
        chunk(req, opt);
    }
    chunk(req, "</select>");
    field(req, "API key for that provider", "backup_key", "password", "",
          s_form.backup_key[0] ? "saved - leave blank to keep" : "", "Never shown again.");
    field(req, "Model (optional)", "backup_model", "text", s_form.backup_model, "",
          "Blank uses gpt-5.4-mini, deepseek-chat, gemini-flash-latest or claude-opus-5-5.");

    chunk(req, "</fieldset><fieldset><legend>Voice (listening and talking)</legend>"
               "<div class=hint>"
               "Default: your Caplora Companion Chat key (caploralab.online/apps). Leave blank for text-only.</div>"
               "<div class=row style='margin-top:8px'>"
               "<button type=button style='font-size:14px;padding:8px;margin:0' onclick=\"v('groq')\">Use Groq (free)</button>"
               "<button type=button style='font-size:14px;padding:8px;margin:0' onclick=\"v('openai')\">Use OpenAI</button></div>");
    field(req, "Voice API key", "voice_api_key", "password", "",
          s_form.voice_api_key[0] ? "saved - leave blank to keep" : "cap_... (Companion key)", "Never shown again.");
    field(req, "Speech-to-text URL", "stt_url", "url", s_form.stt_url, "", NULL);
    field(req, "Speech-to-text model", "stt_model", "text", s_form.stt_model, "", NULL);
    field(req, "Text-to-speech URL", "tts_url", "url", s_form.tts_url, "", NULL);
    field(req, "Text-to-speech model", "tts_model", "text", s_form.tts_model, "", NULL);
    chunk(req, "<input type=hidden name=tts_voice value=female>");
    chunk(req, "<script>function v(k){var p={groq:['https://api.groq.com/openai/v1/audio/transcriptions',"
               "'whisper-large-v3-turbo','https://api.groq.com/openai/v1/audio/speech',"
               "'canopylabs/orpheus-v1-english','hannah'],openai:['https://api.openai.com/v1/audio/transcriptions',"
               "'gpt-4o-mini-transcribe','https://api.openai.com/v1/audio/speech','gpt-4o-mini-tts','coral']}[k],"
               "n=['stt_url','stt_model','tts_url','tts_model'];"
               "for(var i=0;i<4;i++)document.getElementsByName(n[i])[0].value=p[i]}</script>");
    chunk(req, "</fieldset><button type=submit>Save</button></form></body></html>");
    memset(&s_form, 0, sizeof(s_form));
    return httpd_resp_sendstr_chunk(req, NULL);
}

/* ---- form parsing ----------------------------------------------------------- */

static void url_decode(char *s)
{
    char *o = s;
    for (char *p = s; *p; ++p) {
        if (*p == '+') {
            *o++ = ' ';
        } else if (*p == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
            const char hex[3] = {p[1], p[2], 0};
            *o++ = (char)strtol(hex, NULL, 16);
            p += 2;
        } else {
            *o++ = *p;
        }
    }
    *o = '\0';
}

/* Find key in an urlencoded body (decoded in place per pair) and copy it. */
static bool form_get(char *body, size_t body_len, const char *key, char *out, size_t out_size)
{
    const size_t kl = strlen(key);
    for (size_t i = 0; i < body_len;) {
        char *pair = body + i;
        size_t len = strcspn(pair, "&");
        if (len > kl && strncmp(pair, key, kl) == 0 && pair[kl] == '=') {
            char tmp[256];
            size_t vl = len - kl - 1;
            vl = vl >= sizeof(tmp) ? sizeof(tmp) - 1 : vl;
            memcpy(tmp, pair + kl + 1, vl);
            tmp[vl] = '\0';
            url_decode(tmp);
            /* Trim surrounding whitespace (pasted keys often carry some). */
            char *start = tmp;
            while (*start == ' ') {
                ++start;
            }
            size_t n = strlen(start);
            while (n && (start[n - 1] == ' ' || start[n - 1] == '\r' || start[n - 1] == '\n')) {
                start[--n] = '\0';
            }
            snprintf(out, out_size, "%s", start);
            return true;
        }
        i += len + 1;
    }
    return false;
}

static esp_err_t save_post(httpd_req_t *req)
{
    if (req->content_len == 0 || req->content_len >= BODY_MAX) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Form too large");
    }
    char *body = calloc(1, BODY_MAX);
    if (!body) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
    }
    size_t got = 0;
    while (got < req->content_len) {
        const int r = httpd_req_recv(req, body + got, req->content_len - got);
        if (r <= 0) {
            free(body);
            return ESP_FAIL;
        }
        got += (size_t)r;
    }
    app_pet_settings_get(&s_form);
    /* Remember only what decides a Wi-Fi rejoin; keeps this task's stack small. */
    char old_ssid[sizeof(s_form.hotspot_ssid)], old_pass[sizeof(s_form.hotspot_password)];
    memcpy(old_ssid, s_form.hotspot_ssid, sizeof(old_ssid));
    memcpy(old_pass, s_form.hotspot_password, sizeof(old_pass));
    char tmp[200];

#define TEXT(f) form_get(body, got, #f, s_form.f, sizeof(s_form.f))
    TEXT(name);
    TEXT(hotspot_ssid);
    TEXT(chat_model);
    TEXT(base_url);
    TEXT(stt_url);
    TEXT(stt_model);
    TEXT(tts_url);
    TEXT(tts_model);
    TEXT(tts_voice);
    TEXT(backup_model);
#undef TEXT
    /* Secrets: blank keeps the saved value. */
    if (form_get(body, got, "api_key", tmp, sizeof(tmp)) && tmp[0]) {
        strlcpy(s_form.api_key, tmp, sizeof(s_form.api_key));
    }
    if (form_get(body, got, "backup_key", tmp, sizeof(tmp)) && tmp[0]) {
        strlcpy(s_form.backup_key, tmp, sizeof(s_form.backup_key));
    }
    if (form_get(body, got, "backup_provider", tmp, sizeof(tmp)) && tmp[0]) {
        const long v = strtol(tmp, NULL, 10);
        s_form.backup_provider = (uint8_t)(v < 0 || v >= APP_PET_PROVIDER_COUNT ? APP_PET_PROVIDER_NONE : v);
    }
    if (form_get(body, got, "voice_api_key", tmp, sizeof(tmp)) && tmp[0]) {
        strlcpy(s_form.voice_api_key, tmp, sizeof(s_form.voice_api_key));
    }
    if (form_get(body, got, "hotspot_password", tmp, sizeof(tmp)) && tmp[0]) {
        strlcpy(s_form.hotspot_password, tmp, sizeof(s_form.hotspot_password));
    }
    if (form_get(body, got, "animal", tmp, sizeof(tmp))) {
        const pet_animal_t a = pet_animal_parse(tmp);
        if (a < PET_ANIMAL_COUNT) {
            s_form.animal = (uint8_t)a;
        }
    }
    if (form_get(body, got, "volume", tmp, sizeof(tmp)) && tmp[0]) { /* empty keeps the saved volume */
        const long v = strtol(tmp, NULL, 10);
        s_form.volume = (uint8_t)(v < 0 ? 0 : v > 100 ? 100 : v);
    }
    s_form.talk_on_its_own = form_get(body, got, "talk_own", tmp, sizeof(tmp));
    memset(body, 0, BODY_MAX);
    free(body);
    memset(tmp, 0, sizeof(tmp));

    const bool wifi_changed = strcmp(old_ssid, s_form.hotspot_ssid) != 0 ||
                              strcmp(old_pass, s_form.hotspot_password) != 0;
    memset(old_pass, 0, sizeof(old_pass));
    const esp_err_t err = app_pet_settings_set(&s_form);
    memset(&s_form, 0, sizeof(s_form));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save failed: %s", esp_err_to_name(err));
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not save");
    }
    ESP_LOGI(TAG, "settings saved%s", wifi_changed ? "; rejoining hotspot" : "");
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/?saved=1");
    httpd_resp_send(req, NULL, 0);
    if (wifi_changed) {
        app_pet_net_reconnect();
    }
    return ESP_OK;
}

esp_err_t pet_setup_web_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 6144;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.max_open_sockets = 3;
    httpd_handle_t server = NULL;
    ESP_RETURN_ON_ERROR(httpd_start(&server, &cfg), TAG, "httpd");
    const httpd_uri_t save = {.uri = "/save", .method = HTTP_POST, .handler = save_post};
    const httpd_uri_t page = {.uri = "/*", .method = HTTP_GET, .handler = page_get};
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &save), TAG, "save");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &page), TAG, "page");
    return ESP_OK;
}
