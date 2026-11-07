// SPDX-License-Identifier: Apache-2.0
#include "app_radio_player.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "app_stations.h"
#include "bsp/esp_mosaico.h"
#include "esp_audio_simple_player.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

static const char *TAG = "radio_player";

#define CONTROL_TASK_STACK 4096
#define CONTROL_TASK_PRIO 4
#define DECODER_TASK_STACK 6144
#define DECODER_TASK_PRIO 5
#define RETRY_MIN_MS 2000U
#define RETRY_MAX_MS 30000U
#define IDLE_WAIT_MS 1000U

#define PREFS_NAMESPACE "sg_radio"
#define PREFS_FREQ_KEY "freq"
#define PREFS_VOLUME_KEY "volume"

/* Event bits set by producers and the decoder; consumed by the control task. */
#define EV_REQUEST (1u << 0)
#define EV_MUSIC_INFO (1u << 1)
#define EV_STREAM_ENDED (1u << 2)

/* Ownership: `want_*` and `events` are written by any task (atomics) and read
 * by the control task. Everything else is owned by the control task, except
 * `speaker`, which the decoder task also uses for writes/format changes; the
 * control task touches the codec only for volume and never while reopening. */
struct app_radio_player {
    _Atomic int want_station;
    _Atomic bool want_paused;
    _Atomic uint8_t want_volume;
    _Atomic bool online;
    _Atomic uint32_t events;
    _Atomic bool expect_stop;
    esp_asp_music_info_t music_info; /* written by decoder before EV_MUSIC_INFO */

    TaskHandle_t task;
    esp_asp_handle_t asp;
    esp_codec_dev_handle_t speaker;
    app_radio_player_status_cb_t status_cb;
    void *ctx;

    int playing_station;   /* station of the running stream, or NONE */
    bool running;
    uint8_t applied_volume;
    uint16_t saved_freq;
    uint8_t saved_volume;
    uint32_t retry_ms;
    TickType_t retry_at;
    bool retry_pending;
};

static void report(struct app_radio_player *p, app_radio_player_state_t state, int station)
{
    if (p->status_cb) {
        p->status_cb(state, station, p->ctx);
    }
}

static void notify_task(struct app_radio_player *p, uint32_t bits)
{
    atomic_fetch_or(&p->events, bits);
    xTaskNotifyGive(p->task);
}

/* ---- persistence ---------------------------------------------------------- */

esp_err_t app_radio_player_load_saved(uint16_t *out_freq_tenths, uint8_t *out_volume)
{
    ESP_RETURN_ON_FALSE(out_freq_tenths && out_volume, ESP_ERR_INVALID_ARG, TAG, "null output");
    nvs_handle_t h;
    esp_err_t err = nvs_open(PREFS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err == ESP_ERR_NVS_NOT_FOUND ? ESP_ERR_NOT_FOUND : err;
    }
    uint16_t freq = 0;
    uint8_t volume = 0;
    err = nvs_get_u16(h, PREFS_FREQ_KEY, &freq);
    if (err == ESP_OK) {
        err = nvs_get_u8(h, PREFS_VOLUME_KEY, &volume);
    }
    nvs_close(h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_ERR_NOT_FOUND;
    }
    if (err == ESP_OK) {
        *out_freq_tenths = freq;
        *out_volume = volume;
    }
    return err;
}

/* Called on the control task only, after a request settles, so sweeping the
 * dial does not wear the flash. */
static void save_prefs(struct app_radio_player *p, int station, uint8_t volume)
{
    const app_station_t *s = station < 0 ? NULL : app_stations_get((size_t)station);
    const uint16_t freq = s ? s->freq_tenths : p->saved_freq;
    if (freq == p->saved_freq && volume == p->saved_volume) {
        return;
    }
    nvs_handle_t h;
    if (nvs_open(PREFS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    esp_err_t err = nvs_set_u16(h, PREFS_FREQ_KEY, freq);
    if (err == ESP_OK) {
        err = nvs_set_u8(h, PREFS_VOLUME_KEY, volume);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err == ESP_OK) {
        p->saved_freq = freq;
        p->saved_volume = volume;
    } else {
        ESP_LOGW(TAG, "saving preferences failed: %s", esp_err_to_name(err));
    }
}

/* ---- decoder callbacks (simple player task) ------------------------------ */

static int on_pcm(uint8_t *data, int size, void *ctx)
{
    struct app_radio_player *p = ctx;
    if (p->speaker && data && size > 0) {
        (void)esp_codec_dev_write(p->speaker, data, size);
    }
    return 0;
}

static int on_player_event(esp_asp_event_pkt_t *event, void *ctx)
{
    struct app_radio_player *p = ctx;
    if (event->type == ESP_ASP_EVENT_TYPE_MUSIC_INFO &&
            event->payload_size >= (int)sizeof(esp_asp_music_info_t)) {
        memcpy(&p->music_info, event->payload, sizeof(p->music_info));
        /* Match the codec to the stream before the first PCM write. */
        esp_codec_dev_sample_info_t fmt = {
            .sample_rate = (uint32_t)p->music_info.sample_rate,
            .bits_per_sample = (uint8_t)p->music_info.bits,
            .channel = (uint8_t)p->music_info.channels,
        };
        (void)esp_codec_dev_close(p->speaker);
        if (esp_codec_dev_open(p->speaker, &fmt) == ESP_CODEC_DEV_OK) {
            (void)esp_codec_dev_set_out_vol(p->speaker, atomic_load(&p->want_volume));
        } else {
            ESP_LOGE(TAG, "speaker reopen failed for %d Hz", p->music_info.sample_rate);
        }
        notify_task(p, EV_MUSIC_INFO);
    } else if (event->type == ESP_ASP_EVENT_TYPE_STATE &&
               event->payload_size >= (int)sizeof(esp_asp_state_t)) {
        esp_asp_state_t st;
        memcpy(&st, event->payload, sizeof(st));
        if ((st == ESP_ASP_STATE_ERROR || st == ESP_ASP_STATE_FINISHED ||
                st == ESP_ASP_STATE_STOPPED) && !atomic_load(&p->expect_stop)) {
            ESP_LOGW(TAG, "stream ended: %s", esp_audio_simple_player_state_to_str(st));
            notify_task(p, EV_STREAM_ENDED);
        }
    }
    return 0;
}

/* ---- control task --------------------------------------------------------- */

static void stop_stream(struct app_radio_player *p)
{
    if (!p->running) {
        return;
    }
    atomic_store(&p->expect_stop, true);
    (void)esp_audio_simple_player_stop(p->asp);
    atomic_store(&p->expect_stop, false);
    p->running = false;
    p->playing_station = APP_STATIONS_NONE;
}

static void start_stream(struct app_radio_player *p, int station)
{
    const app_station_t *s = app_stations_get((size_t)station);
    ESP_LOGI(TAG, "tuning %s (%s)", s->name, s->stream_url);
    report(p, APP_RADIO_PLAYER_BUFFERING, station);
    const esp_gmf_err_t err = esp_audio_simple_player_run(p->asp, s->stream_url, NULL);
    if (err != ESP_GMF_ERR_OK) {
        ESP_LOGE(TAG, "run failed (%d)", err);
        notify_task(p, EV_STREAM_ENDED);
        p->playing_station = station;
        return;
    }
    p->running = true;
    p->playing_station = station;
}

static void schedule_retry(struct app_radio_player *p)
{
    p->retry_pending = true;
    p->retry_at = xTaskGetTickCount() + pdMS_TO_TICKS(p->retry_ms);
    p->retry_ms = p->retry_ms * 2 > RETRY_MAX_MS ? RETRY_MAX_MS : p->retry_ms * 2;
}

static void reconcile(struct app_radio_player *p, uint32_t events)
{
    const int station = atomic_load(&p->want_station);
    const bool paused = atomic_load(&p->want_paused);
    const bool online = atomic_load(&p->online);
    const uint8_t volume = atomic_load(&p->want_volume);

    if (volume != p->applied_volume) {
        p->applied_volume = volume;
        (void)esp_codec_dev_set_out_vol(p->speaker, volume);
    }

    if (events & EV_MUSIC_INFO) {
        if (p->running && p->playing_station == station) {
            p->retry_ms = RETRY_MIN_MS;
            report(p, APP_RADIO_PLAYER_PLAYING, station);
        }
    }
    if (events & EV_STREAM_ENDED) {
        p->running = false;
        if (station >= 0 && !paused && online) {
            report(p, APP_RADIO_PLAYER_ERROR, station);
            schedule_retry(p);
        }
    }

    const bool want_play = station >= 0 && !paused && online;
    if (!want_play) {
        stop_stream(p);
        p->retry_pending = false;
        p->retry_ms = RETRY_MIN_MS;
        report(p, APP_RADIO_PLAYER_IDLE, station);
    } else if (station != p->playing_station || !p->running) {
        const bool retry_due = !p->retry_pending ||
                               (int32_t)(xTaskGetTickCount() - p->retry_at) >= 0;
        if (station != p->playing_station) {
            /* A new station is a fresh attempt, not a retry. */
            p->retry_pending = false;
            p->retry_ms = RETRY_MIN_MS;
        }
        if (station != p->playing_station || retry_due) {
            stop_stream(p);
            p->retry_pending = false;
            start_stream(p, station);
        }
    }
    save_prefs(p, station, volume);
}

static void control_task(void *arg)
{
    struct app_radio_player *p = arg;
    for (;;) {
        TickType_t wait = pdMS_TO_TICKS(IDLE_WAIT_MS);
        if (p->retry_pending) {
            const int32_t left = (int32_t)(p->retry_at - xTaskGetTickCount());
            wait = left > 0 ? (TickType_t)left : 0;
        }
        (void)ulTaskNotifyTake(pdTRUE, wait);
        reconcile(p, atomic_exchange(&p->events, 0));
    }
}

/* ---- public API ----------------------------------------------------------- */

esp_err_t app_radio_player_start(const app_radio_player_config_t *config,
                                 app_radio_player_handle_t *out_handle)
{
    ESP_RETURN_ON_FALSE(config && out_handle, ESP_ERR_INVALID_ARG, TAG, "null argument");
    *out_handle = NULL;
    struct app_radio_player *p = calloc(1, sizeof(*p));
    ESP_RETURN_ON_FALSE(p, ESP_ERR_NO_MEM, TAG, "no memory");
    p->status_cb = config->status_cb;
    p->ctx = config->ctx;
    p->playing_station = APP_STATIONS_NONE;
    p->retry_ms = RETRY_MIN_MS;
    atomic_init(&p->want_station, APP_STATIONS_NONE);
    atomic_init(&p->want_paused, false);
    atomic_init(&p->want_volume, config->initial_volume > 100 ? 100 : config->initial_volume);
    atomic_init(&p->online, false);
    atomic_init(&p->events, 0);
    atomic_init(&p->expect_stop, false);
    (void)app_radio_player_load_saved(&p->saved_freq, &p->saved_volume);

    esp_err_t err = ESP_OK;
    p->speaker = bsp_audio_codec_speaker_init();
    if (!p->speaker) {
        err = ESP_FAIL;
        ESP_LOGE(TAG, "speaker init failed");
    }
    if (err == ESP_OK) {
        esp_codec_dev_sample_info_t fmt = {
            .sample_rate = BSP_AUDIO_DEFAULT_SAMPLE_RATE,
            .bits_per_sample = BSP_AUDIO_DEFAULT_BITS_PER_SAMPLE,
            .channel = BSP_AUDIO_DEFAULT_CHANNELS,
        };
        err = esp_codec_dev_open(p->speaker, &fmt) == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL;
    }
    if (err == ESP_OK) {
        p->applied_volume = atomic_load(&p->want_volume);
        (void)esp_codec_dev_set_out_vol(p->speaker, p->applied_volume);
        esp_asp_cfg_t cfg = {
            .out = {.cb = on_pcm, .user_ctx = p},
            .task_prio = DECODER_TASK_PRIO,
            .task_stack = DECODER_TASK_STACK,
            .task_core = 0,
            .task_stack_in_ext = true,
        };
        err = esp_audio_simple_player_new(&cfg, &p->asp) == ESP_GMF_ERR_OK ? ESP_OK : ESP_ERR_NO_MEM;
    }
    if (err == ESP_OK) {
        err = esp_audio_simple_player_set_event(p->asp, on_player_event, p) == ESP_GMF_ERR_OK
              ? ESP_OK : ESP_FAIL;
    }
    if (err == ESP_OK &&
            xTaskCreate(control_task, "radio_ctl", CONTROL_TASK_STACK, p, CONTROL_TASK_PRIO, &p->task) != pdPASS) {
        err = ESP_ERR_NO_MEM;
    }
    if (err != ESP_OK) {
        if (p->asp) {
            (void)esp_audio_simple_player_destroy(p->asp);
        }
        if (p->speaker) {
            (void)esp_codec_dev_close(p->speaker);
        }
        free(p);
        return err;
    }
    *out_handle = p;
    return ESP_OK;
}

void app_radio_player_tune(app_radio_player_handle_t h, int station)
{
    if (h) {
        atomic_store(&h->want_station, station);
        notify_task(h, EV_REQUEST);
    }
}

void app_radio_player_set_paused(app_radio_player_handle_t h, bool paused)
{
    if (h) {
        atomic_store(&h->want_paused, paused);
        notify_task(h, EV_REQUEST);
    }
}

void app_radio_player_set_volume(app_radio_player_handle_t h, uint8_t volume_percent)
{
    if (h) {
        atomic_store(&h->want_volume, volume_percent > 100 ? 100 : volume_percent);
        notify_task(h, EV_REQUEST);
    }
}

void app_radio_player_set_online(app_radio_player_handle_t h, bool online)
{
    if (h) {
        atomic_store(&h->online, online);
        notify_task(h, EV_REQUEST);
    }
}
