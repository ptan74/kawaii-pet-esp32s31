// SPDX-License-Identifier: Apache-2.0
#include "app_pet_audio.h"

#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "bsp/esp_mosaico.h"
#include "esp_audio_simple_player.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_heap_caps.h"
#include "freertos/idf_additions.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "pet_synth.h"

static const char *TAG = "pet_audio";

#define QUEUE_LEN 12
#define TASK_STACK 6144
#define TASK_PRIO 6
#define PLAYER_STACK 6144
#define PLAYER_PRIO 5
#define MIC_CHUNK 512                 /* samples per read, 32 ms */
#define MIC_GAIN_DB 30.0f
#define LEVEL_PERIOD_MS 50
#define CLIP_TIMEOUT_MS 90000
#define URL_MAX 192

typedef enum {
    CMD_EFFECT,
    CMD_CLIP,
    CMD_MUSIC,
    CMD_REC_START,
    CMD_REC_STOP,
    CMD_VOLUME,
    CMD_MUSIC_RETRY,
} cmd_kind_t;

typedef struct {
    cmd_kind_t kind;
    uint8_t animal;
    uint8_t sound;
    uint8_t volume;
    uint8_t *data;                    /* CLIP: owned buffer */
    size_t len;
    char *url;                        /* MUSIC: owned copy, NULL = stop */
} cmd_t;

typedef struct {
    uint32_t rate;
    uint8_t channels;
} fmt_t;

/* Module singleton (one speaker/mic per board). The audio task owns all
 * fields except: `clip_*` read by the clip player's input callback while that
 * player runs, the speaker which player output callbacks also write (only one
 * player runs at a time and the task does not write meanwhile), and the
 * atomics. */
static struct {
    app_pet_audio_config_t cfg;
    QueueHandle_t queue;
    esp_codec_dev_handle_t speaker;
    esp_codec_dev_handle_t mic;
    fmt_t spk_fmt;                    /* current speaker format, rate 0 = closed */
    esp_asp_handle_t music_player;
    esp_asp_handle_t clip_player;
    SemaphoreHandle_t clip_done;
    const uint8_t *clip_data;
    size_t clip_len;
    size_t clip_pos;
    bool music_on;
    bool music_retried;              /* one automatic retry per request */
    char *music_url;
    bool recording;
    int16_t *rec;
    size_t rec_len;
    int16_t *fx;                      /* synth scratch, PSRAM */
    _Atomic uint8_t volume;
    _Atomic bool clip_playing;
    TickType_t last_level;
} s_audio;

static void emit(app_pet_audio_event_t ev, float level)
{
    if (s_audio.cfg.event_cb) {
        s_audio.cfg.event_cb(ev, level, s_audio.cfg.ctx);
    }
}

/* ---- speaker / mic format ------------------------------------------------- */

static bool speaker_open(uint32_t rate, uint8_t channels)
{
    if (s_audio.spk_fmt.rate == rate && s_audio.spk_fmt.channels == channels) {
        return true;
    }
    if (s_audio.spk_fmt.rate) {
        (void)esp_codec_dev_close(s_audio.speaker);
        s_audio.spk_fmt.rate = 0;
    }
    esp_codec_dev_sample_info_t fmt = {.sample_rate = rate, .bits_per_sample = 16, .channel = channels};
    if (esp_codec_dev_open(s_audio.speaker, &fmt) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "speaker open %u Hz x%u failed", (unsigned)rate, channels);
        return false;
    }
    (void)esp_codec_dev_set_out_vol(s_audio.speaker, atomic_load(&s_audio.volume));
    s_audio.spk_fmt = (fmt_t){rate, channels};
    return true;
}

static void speaker_close(void)
{
    if (s_audio.spk_fmt.rate) {
        (void)esp_codec_dev_close(s_audio.speaker);
        s_audio.spk_fmt.rate = 0;
    }
}

/* ---- simple player callbacks (player tasks) ------------------------------ */

static void report_level(const int16_t *pcm, size_t samples)
{
    const TickType_t now = xTaskGetTickCount();
    if (now - s_audio.last_level < pdMS_TO_TICKS(LEVEL_PERIOD_MS) || samples == 0) {
        return;
    }
    s_audio.last_level = now;
    double acc = 0;
    for (size_t i = 0; i < samples; i += 4) {
        acc += (double)pcm[i] * pcm[i];
    }
    const float rms = (float)sqrt(acc / (double)((samples + 3) / 4)) / 32768.0f;
    const float level = rms * 4.0f > 1.0f ? 1.0f : rms * 4.0f;
    emit(APP_PET_AUDIO_CLIP_LEVEL, level);
}

static int on_pcm(uint8_t *data, int size, void *ctx)
{
    const bool clip = ctx == (void *)1;
    if (data && size > 0 && s_audio.spk_fmt.rate) {
        (void)esp_codec_dev_write(s_audio.speaker, data, size);
        if (clip) {
            report_level((const int16_t *)data, (size_t)size / 2);
        }
    }
    return 0;
}

static int clip_read(uint8_t *data, int size, void *ctx)
{
    (void)ctx;
    const size_t left = s_audio.clip_len - s_audio.clip_pos;
    const size_t n = (size_t)size < left ? (size_t)size : left;
    memcpy(data, s_audio.clip_data + s_audio.clip_pos, n);
    s_audio.clip_pos += n;
    return (int)n;
}

static int on_player_event(esp_asp_event_pkt_t *event, void *ctx)
{
    const bool clip = ctx == (void *)1;
    if (event->type == ESP_ASP_EVENT_TYPE_MUSIC_INFO && event->payload_size >= (int)sizeof(esp_asp_music_info_t)) {
        esp_asp_music_info_t info;
        memcpy(&info, event->payload, sizeof(info));
        (void)speaker_open((uint32_t)info.sample_rate, (uint8_t)info.channels);
        if (!clip) {
            emit(APP_PET_AUDIO_MUSIC_ON, 0);
        }
    } else if (event->type == ESP_ASP_EVENT_TYPE_STATE && event->payload_size >= (int)sizeof(esp_asp_state_t)) {
        esp_asp_state_t st;
        memcpy(&st, event->payload, sizeof(st));
        if (st == ESP_ASP_STATE_FINISHED || st == ESP_ASP_STATE_STOPPED || st == ESP_ASP_STATE_ERROR) {
            if (clip) {
                xSemaphoreGive(s_audio.clip_done);
            } else if (st != ESP_ASP_STATE_STOPPED) {
                ESP_LOGW(TAG, "music stream ended: %s", esp_audio_simple_player_state_to_str(st));
                /* Radio edges sometimes answer 503; retry once from the task. */
                cmd_t retry = {.kind = CMD_MUSIC_RETRY};
                if (xQueueSend(s_audio.queue, &retry, 0) != pdTRUE) {
                    emit(APP_PET_AUDIO_MUSIC_OFF, 0);
                }
            }
        }
    }
    return 0;
}

/* ---- command handlers (audio task) ---------------------------------------- */

static void music_stop(void)
{
    if (s_audio.music_on) {
        (void)esp_audio_simple_player_stop(s_audio.music_player);
        s_audio.music_on = false;
        emit(APP_PET_AUDIO_MUSIC_OFF, 0);
    }
}

static void music_start(void)
{
    if (!s_audio.music_url) {
        return;
    }
    music_stop();
    ESP_LOGI(TAG, "music %s", s_audio.music_url);
    if (esp_audio_simple_player_run(s_audio.music_player, s_audio.music_url, NULL) == ESP_GMF_ERR_OK) {
        s_audio.music_on = true;
    } else {
        emit(APP_PET_AUDIO_MUSIC_OFF, 0);
    }
}

static void play_effect(const cmd_t *c)
{
    if (s_audio.music_on || !s_audio.fx) {
        return; /* don't cut the radio for a bark */
    }
    const size_t n = pet_synth((pet_animal_t)c->animal, (pet_sound_t)c->sound, (uint32_t)xTaskGetTickCount(),
                               s_audio.fx, PET_SYNTH_MAX_SAMPLES);
    if (n && speaker_open(PET_SYNTH_RATE, 1)) {
        (void)esp_codec_dev_write(s_audio.speaker, s_audio.fx, (int)(n * sizeof(int16_t)));
    }
}

/* WAV: find "fmt " and "data"; play PCM16 directly. */
static bool play_wav(const uint8_t *d, size_t len)
{
    if (len < 44 || memcmp(d, "RIFF", 4) != 0 || memcmp(d + 8, "WAVE", 4) != 0) {
        return false;
    }
    uint32_t rate = 0;
    uint16_t channels = 0, bits = 0;
    size_t pos = 12;
    while (pos + 8 <= len) {
        const uint32_t sz = d[pos + 4] | d[pos + 5] << 8 | d[pos + 6] << 16 | (uint32_t)d[pos + 7] << 24;
        if (memcmp(d + pos, "fmt ", 4) == 0 && pos + 24 <= len) {
            channels = (uint16_t)(d[pos + 10] | d[pos + 11] << 8);
            rate = d[pos + 12] | d[pos + 13] << 8 | d[pos + 14] << 16 | (uint32_t)d[pos + 15] << 24;
            bits = (uint16_t)(d[pos + 22] | d[pos + 23] << 8);
        } else if (memcmp(d + pos, "data", 4) == 0) {
            if (bits != 16 || !rate || !channels || !speaker_open(rate, (uint8_t)channels)) {
                return true; /* recognised but unplayable: done */
            }
            size_t data_len = sz < len - pos - 8 ? sz : len - pos - 8;
            const uint8_t *pcm = d + pos + 8;
            const size_t step = 2048;
            for (size_t off = 0; off < data_len; off += step) {
                const size_t n = data_len - off < step ? data_len - off : step;
                (void)esp_codec_dev_write(s_audio.speaker, (void *)(pcm + off), (int)n);
                report_level((const int16_t *)(pcm + off), n / 2);
            }
            return true;
        }
        pos += 8 + sz + (sz & 1);
    }
    return true;
}

static void play_clip(cmd_t *c)
{
    music_stop();
    atomic_store(&s_audio.clip_playing, true);
    emit(APP_PET_AUDIO_CLIP_STARTED, 0);
    if (!play_wav(c->data, c->len)) {
        /* Treat everything else as MP3 through the clip player. */
        s_audio.clip_data = c->data;
        s_audio.clip_len = c->len;
        s_audio.clip_pos = 0;
        xSemaphoreTake(s_audio.clip_done, 0);
        if (esp_audio_simple_player_run(s_audio.clip_player, "raw://reply/voice.mp3", NULL) == ESP_GMF_ERR_OK) {
            if (xSemaphoreTake(s_audio.clip_done, pdMS_TO_TICKS(CLIP_TIMEOUT_MS)) != pdTRUE) {
                ESP_LOGW(TAG, "clip timed out");
            }
            (void)esp_audio_simple_player_stop(s_audio.clip_player);
        } else {
            ESP_LOGE(TAG, "clip decode failed to start");
        }
        s_audio.clip_data = NULL;
    }
    free(c->data);
    c->data = NULL;
    atomic_store(&s_audio.clip_playing, false);
    emit(APP_PET_AUDIO_CLIP_LEVEL, 0);
    emit(APP_PET_AUDIO_CLIP_DONE, 0);
}

static void rec_start(void)
{
    if (s_audio.recording || !s_audio.mic) {
        return;
    }
    music_stop();
    speaker_close();
    s_audio.rec_len = 0;
    s_audio.rec = heap_caps_malloc(APP_PET_AUDIO_REC_RATE * APP_PET_AUDIO_REC_MAX_S * sizeof(int16_t),
                                   MALLOC_CAP_SPIRAM);
    if (!s_audio.rec) {
        ESP_LOGE(TAG, "no memory for recording");
        return;
    }
    esp_codec_dev_sample_info_t fmt = {.sample_rate = APP_PET_AUDIO_REC_RATE, .bits_per_sample = 16, .channel = 1};
    if (esp_codec_dev_open(s_audio.mic, &fmt) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "mic open failed");
        free(s_audio.rec);
        s_audio.rec = NULL;
        return;
    }
    (void)esp_codec_dev_set_in_gain(s_audio.mic, MIC_GAIN_DB);
    s_audio.recording = true;
}

static void rec_stop(void)
{
    if (!s_audio.recording) {
        if (s_audio.cfg.rec_cb) {
            s_audio.cfg.rec_cb(NULL, 0, s_audio.cfg.ctx); /* nothing captured */
        }
        return;
    }
    s_audio.recording = false;
    (void)esp_codec_dev_close(s_audio.mic);
    int16_t *pcm = s_audio.rec;
    const size_t n = s_audio.rec_len;
    s_audio.rec = NULL;
    ESP_LOGI(TAG, "recorded %.1f s", (double)n / APP_PET_AUDIO_REC_RATE);
    if (s_audio.cfg.rec_cb) {
        s_audio.cfg.rec_cb(pcm, n, s_audio.cfg.ctx);
    } else {
        free(pcm);
    }
}

static void rec_capture(void)
{
    const size_t cap = APP_PET_AUDIO_REC_RATE * APP_PET_AUDIO_REC_MAX_S;
    if (s_audio.rec_len + MIC_CHUNK > cap) {
        rec_stop(); /* hit the limit */
        return;
    }
    if (esp_codec_dev_read(s_audio.mic, s_audio.rec + s_audio.rec_len, MIC_CHUNK * sizeof(int16_t)) ==
            ESP_CODEC_DEV_OK) {
        s_audio.rec_len += MIC_CHUNK;
    }
}

static void handle(cmd_t *c)
{
    switch (c->kind) {
    case CMD_EFFECT:
        if (!s_audio.recording) {
            play_effect(c);
        }
        break;
    case CMD_CLIP:
        if (s_audio.recording) {
            free(c->data);
        } else {
            play_clip(c);
        }
        break;
    case CMD_MUSIC:
        free(s_audio.music_url);
        s_audio.music_url = c->url;
        c->url = NULL;
        if (s_audio.music_url) {
            s_audio.music_retried = false;
            speaker_close();
            music_start();
        } else {
            music_stop();
        }
        break;
    case CMD_REC_START:
        rec_start();
        break;
    case CMD_REC_STOP:
        rec_stop();
        break;
    case CMD_MUSIC_RETRY:
        s_audio.music_on = false;
        if (s_audio.music_url && !s_audio.music_retried) {
            s_audio.music_retried = true;
            vTaskDelay(pdMS_TO_TICKS(1500));
            music_start();
        } else {
            emit(APP_PET_AUDIO_MUSIC_OFF, 0);
        }
        break;
    case CMD_VOLUME:
        atomic_store(&s_audio.volume, c->volume);
        if (s_audio.spk_fmt.rate) {
            (void)esp_codec_dev_set_out_vol(s_audio.speaker, c->volume);
        }
        break;
    }
}

static void audio_task(void *arg)
{
    (void)arg;
    cmd_t c;
    for (;;) {
        if (s_audio.recording) {
            rec_capture();
            while (xQueueReceive(s_audio.queue, &c, 0) == pdTRUE) {
                handle(&c);
            }
        } else if (xQueueReceive(s_audio.queue, &c, portMAX_DELAY) == pdTRUE) {
            handle(&c);
        }
    }
}

/* ---- public ------------------------------------------------------------------ */

static void post(cmd_t *c)
{
    if (!s_audio.queue || xQueueSend(s_audio.queue, c, 0) != pdTRUE) {
        ESP_LOGW(TAG, "audio queue full; dropped request %d", c->kind);
        free(c->data);
        free(c->url);
    }
}

void app_pet_audio_effect(pet_animal_t animal, pet_sound_t sound)
{
    cmd_t c = {.kind = CMD_EFFECT, .animal = (uint8_t)animal, .sound = (uint8_t)sound};
    post(&c);
}

void app_pet_audio_clip(uint8_t *data, size_t len)
{
    cmd_t c = {.kind = CMD_CLIP, .data = data, .len = len};
    post(&c);
}

void app_pet_audio_music(const char *url)
{
    cmd_t c = {.kind = CMD_MUSIC, .url = url ? strndup(url, URL_MAX) : NULL};
    if (url && !c.url) {
        return;
    }
    post(&c);
}

void app_pet_audio_record_start(void)
{
    cmd_t c = {.kind = CMD_REC_START};
    post(&c);
}

void app_pet_audio_record_stop(void)
{
    cmd_t c = {.kind = CMD_REC_STOP};
    post(&c);
}

void app_pet_audio_set_volume(uint8_t volume)
{
    cmd_t c = {.kind = CMD_VOLUME, .volume = volume > 100 ? 100 : volume};
    post(&c);
}

static esp_err_t new_player(esp_asp_handle_t *out, bool clip)
{
    esp_asp_cfg_t cfg = {
        .in = {.cb = clip ? clip_read : NULL, .user_ctx = NULL},
        .out = {.cb = on_pcm, .user_ctx = clip ? (void *)1 : NULL},
        .task_prio = PLAYER_PRIO,
        .task_stack = PLAYER_STACK,
        .task_core = 0,
        .task_stack_in_ext = true,
    };
    ESP_RETURN_ON_FALSE(esp_audio_simple_player_new(&cfg, out) == ESP_GMF_ERR_OK, ESP_ERR_NO_MEM, TAG, "player");
    ESP_RETURN_ON_FALSE(esp_audio_simple_player_set_event(*out, on_player_event, clip ? (void *)1 : NULL) ==
                        ESP_GMF_ERR_OK, ESP_FAIL, TAG, "player events");
    return ESP_OK;
}

static void cleanup(void)
{
    if (s_audio.clip_player) {
        (void)esp_audio_simple_player_destroy(s_audio.clip_player);
    }
    if (s_audio.music_player) {
        (void)esp_audio_simple_player_destroy(s_audio.music_player);
    }
    if (s_audio.queue) {
        vQueueDelete(s_audio.queue);
    }
    if (s_audio.clip_done) {
        vSemaphoreDelete(s_audio.clip_done);
    }
    free(s_audio.fx);
    memset(&s_audio, 0, sizeof(s_audio));
}

esp_err_t app_pet_audio_start(const app_pet_audio_config_t *config)
{
    ESP_RETURN_ON_FALSE(config && !s_audio.queue, ESP_ERR_INVALID_STATE, TAG, "bad state");
    s_audio.cfg = *config;
    atomic_init(&s_audio.volume, config->volume > 100 ? 100 : config->volume);
    s_audio.speaker = bsp_audio_codec_speaker_init();
    ESP_RETURN_ON_FALSE(s_audio.speaker, ESP_FAIL, TAG, "speaker init");
    s_audio.mic = bsp_audio_codec_microphone_init();
    if (!s_audio.mic) {
        ESP_LOGE(TAG, "microphone init failed; talking will be unavailable");
    }
    esp_err_t err = ESP_OK;
    s_audio.fx = heap_caps_malloc(PET_SYNTH_MAX_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    s_audio.clip_done = xSemaphoreCreateBinary();
    s_audio.queue = xQueueCreate(QUEUE_LEN, sizeof(cmd_t));
    if (!s_audio.fx || !s_audio.clip_done || !s_audio.queue) {
        err = ESP_ERR_NO_MEM;
    }
    if (err == ESP_OK) {
        err = new_player(&s_audio.music_player, false);
    }
    if (err == ESP_OK) {
        err = new_player(&s_audio.clip_player, true);
    }
    /* Stack in PSRAM: internal RAM is kept for display and audio DMA. This
     * task never writes flash. */
    if (err == ESP_OK && xTaskCreateWithCaps(audio_task, "pet_audio", TASK_STACK, NULL, TASK_PRIO, NULL,
                                             MALLOC_CAP_SPIRAM) != pdPASS) {
        err = ESP_ERR_NO_MEM;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "audio start failed: %s", esp_err_to_name(err));
        cleanup();
    }
    return err;
}
