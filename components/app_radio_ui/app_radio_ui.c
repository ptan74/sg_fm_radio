// SPDX-License-Identifier: Apache-2.0
#include "app_radio_ui.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#include "app_stations.h"
#include "bundle_gsp.h"
#include "radio_tuner.h"

#define CHIP_COUNT 5
#define CHIP_CENTER 2
#define STATUS_REFRESH_MS 150

#define COLOR_PILL_NEUTRAL 0x3A3935u
#define COLOR_PILL_PLAYING 0x3B6D11u
#define COLOR_PILL_BUSY 0x854F0Bu
#define COLOR_PILL_ERROR 0xA32D2Du

/* Packed {link, audio, station + 1} so the report crosses tasks atomically. */
#define REPORT_PACK(link, audio, station) \
    ((uint32_t)(link) | ((uint32_t)(audio) << 8) | ((uint32_t)((station) + 1) << 16))
#define REPORT_LINK(r) ((app_radio_link_t)((r) & 0xFFu))
#define REPORT_AUDIO(r) ((app_radio_audio_t)(((r) >> 8) & 0xFFu))
#define REPORT_STATION(r) ((int)((r) >> 16) - 1)

/* Single owner: every field except `report` is touched only on the GSP render
 * task (event and timer callbacks, plus create/destroy before and
 * after dispatch). `report` is the only cross-task input. */
struct app_radio_ui {
    esp_gsp_handle_t ui;
    app_radio_ui_sink_t sink;
    radio_tuner_t tuner;
    void *status_timer;
    _Atomic uint32_t report;
    /* Inputs of the status line as last drawn; redraw only on change. */
    struct {
        uint32_t report;
        int locked;
        int committed;
        bool paused;
        bool valid;
    } shown_status;
    uint16_t shown_freq;
    int shown_locked;
    esp_gsp_err_t last_error;
};

static void remember(struct app_radio_ui *s, esp_gsp_err_t err)
{
    if (err != ESP_GSP_OK) {
        s->last_error = err;
    }
}

static void format_freq(char *buf, size_t size, uint16_t tenths)
{
    snprintf(buf, size, "%u.%u", (unsigned)(tenths / 10), (unsigned)(tenths % 10));
}

/* ---- status line ------------------------------------------------------ */

typedef struct {
    const char *text;
    uint32_t color;
} status_view_t;

static status_view_t status_view(const struct app_radio_ui *s, uint32_t report)
{
    const radio_tuner_t *t = &s->tuner;
    if (t->locked == APP_STATIONS_NONE) {
        return (status_view_t) {"Searching", COLOR_PILL_NEUTRAL};
    }
    if (t->paused) {
        return (status_view_t) {"Paused", COLOR_PILL_NEUTRAL};
    }
    switch (REPORT_LINK(report)) {
    case APP_RADIO_LINK_OFFLINE:
        return (status_view_t) {"No Wi-Fi", COLOR_PILL_ERROR};
    case APP_RADIO_LINK_CONNECTING:
        return (status_view_t) {"Connecting", COLOR_PILL_BUSY};
    default:
        break;
    }
    /* Audio reports for an older tune request are stale. */
    const app_radio_audio_t audio = REPORT_STATION(report) == t->committed
                                    ? REPORT_AUDIO(report) : APP_RADIO_AUDIO_BUFFERING;
    switch (audio) {
    case APP_RADIO_AUDIO_PLAYING:
        return (status_view_t) {"Playing", COLOR_PILL_PLAYING};
    case APP_RADIO_AUDIO_ERROR:
        return (status_view_t) {"No signal", COLOR_PILL_ERROR};
    default:
        return (status_view_t) {"Tuning", COLOR_PILL_BUSY};
    }
}

static const char *wifi_text(app_radio_link_t link)
{
    switch (link) {
    case APP_RADIO_LINK_ONLINE:
        return "Wi-Fi on";
    case APP_RADIO_LINK_CONNECTING:
        return "Wi-Fi ...";
    default:
        return "No Wi-Fi";
    }
}

static void refresh_status(struct app_radio_ui *s, bool force)
{
    const uint32_t report = atomic_load(&s->report);
    if (!force && s->shown_status.valid && s->shown_status.report == report &&
            s->shown_status.locked == s->tuner.locked &&
            s->shown_status.committed == s->tuner.committed &&
            s->shown_status.paused == s->tuner.paused) {
        return;
    }
    s->shown_status.report = report;
    s->shown_status.locked = s->tuner.locked;
    s->shown_status.committed = s->tuner.committed;
    s->shown_status.paused = s->tuner.paused;
    s->shown_status.valid = true;
    const status_view_t view = status_view(s, report);
    remember(s, gsp_radio_status_set_text(s->ui, view.text));
    remember(s, gsp_radio_status_pill_set_color_rgb888(s->ui, view.color));
    remember(s, gsp_radio_wifi_status_set_text(s->ui, wifi_text(REPORT_LINK(report))));
}

static void status_tick(esp_gsp_handle_t ui, void *ctx)
{
    (void)ui;
    refresh_status(ctx, false);
}

/* ---- dial, station text and chips ------------------------------------ */

static int wrap_station(int index)
{
    const int n = (int)app_stations_count();
    return ((index % n) + n) % n;
}

static void render_chips(struct app_radio_ui *s)
{
    static esp_gsp_err_t (*const chip_text[CHIP_COUNT])(esp_gsp_handle_t, const char *) = {
        gsp_radio_chip_0_text_set_text, gsp_radio_chip_1_text_set_text,
        gsp_radio_chip_2_text_set_text, gsp_radio_chip_3_text_set_text,
        gsp_radio_chip_4_text_set_text,
    };
    const int anchor = s->tuner.locked != APP_STATIONS_NONE ? s->tuner.locked
                       : app_stations_nearest(s->tuner.freq_tenths);
    char buf[8];
    for (int i = 0; i < CHIP_COUNT; ++i) {
        const app_station_t *st = app_stations_get((size_t)wrap_station(anchor - CHIP_CENTER + i));
        format_freq(buf, sizeof(buf), st->freq_tenths);
        remember(s, chip_text[i](s->ui, buf));
        if (i == CHIP_CENTER) {
            remember(s, gsp_radio_chip_lock_text_set_text(s->ui, buf));
        }
    }
    remember(s, gsp_radio_chip_lock_set_visible(s->ui, s->tuner.locked != APP_STATIONS_NONE));
}

/* Redraws everything derived from the dial position; cheap when unchanged so
 * drag samples can call it freely. */
static void render_dial(struct app_radio_ui *s)
{
    const radio_tuner_t *t = &s->tuner;
    if (t->freq_tenths == s->shown_freq && t->locked == s->shown_locked) {
        return;
    }
    char buf[8];
    format_freq(buf, sizeof(buf), t->freq_tenths);
    remember(s, gsp_radio_freq_set_text(s->ui, buf));
    remember(s, gsp_radio_dial_strip_set_x(s->ui, radio_tuner_dial_x(t->freq_tenths)));
    if (t->locked != s->shown_locked) {
        const app_station_t *st = t->locked == APP_STATIONS_NONE ? NULL
                                  : app_stations_get((size_t)t->locked);
        remember(s, gsp_radio_station_name_set_text(s->ui, st ? st->name : "No station"));
        remember(s, gsp_radio_tagline_set_text(s->ui, st ? st->tagline : "Static - keep sweeping"));
    }
    /* Chips follow the nearest station, which changes only between stations. */
    render_chips(s);
    s->shown_freq = t->freq_tenths;
    s->shown_locked = t->locked;
    refresh_status(s, false);
}

static void render_transport(struct app_radio_ui *s)
{
    char buf[12];
    snprintf(buf, sizeof(buf), "Vol %u%%", (unsigned)s->tuner.volume);
    remember(s, gsp_radio_volume_text_set_text(s->ui, buf));
    remember(s, gsp_radio_pause_icon_set_visible(s->ui, !s->tuner.paused));
    remember(s, gsp_radio_play_icon_set_visible(s->ui, s->tuner.paused));
}

/* ---- actions ---------------------------------------------------------- */

static void commit_tuning(struct app_radio_ui *s)
{
    render_dial(s);
    if (radio_tuner_commit(&s->tuner) && s->sink.tune) {
        s->sink.tune(s->tuner.committed, s->sink.ctx);
    }
    refresh_status(s, false);
}

static void on_event(esp_gsp_handle_t ui, const esp_gsp_event_t *event, void *ctx)
{
    (void)ui;
    struct app_radio_ui *s = ctx;
    if (gsp_radio_event_is_dial_jog(event)) {
        radio_tuner_jog(&s->tuner, (int32_t)event->arg);
        render_dial(s);
    } else if (gsp_radio_event_is_dial_release(event)) {
        radio_tuner_drag_end(&s->tuner);
        commit_tuning(s);
    } else if (gsp_radio_event_is_seek_up(event)) {
        radio_tuner_seek(&s->tuner, +1);
        commit_tuning(s);
    } else if (gsp_radio_event_is_seek_down(event)) {
        radio_tuner_seek(&s->tuner, -1);
        commit_tuning(s);
    } else if (gsp_radio_event_is_chip(event)) {
        const int anchor = s->tuner.locked != APP_STATIONS_NONE ? s->tuner.locked
                           : app_stations_nearest(s->tuner.freq_tenths);
        radio_tuner_select(&s->tuner, wrap_station(anchor - CHIP_CENTER + (int)event->arg));
        commit_tuning(s);
    } else if (gsp_radio_event_is_play_pause(event)) {
        s->tuner.paused = !s->tuner.paused;
        if (s->sink.set_paused) {
            s->sink.set_paused(s->tuner.paused, s->sink.ctx);
        }
        render_transport(s);
        refresh_status(s, false);
    } else if (gsp_radio_event_is_volume(event)) {
        const uint8_t volume = radio_tuner_cycle_volume(&s->tuner);
        if (s->sink.set_volume) {
            s->sink.set_volume(volume, s->sink.ctx);
        }
        render_transport(s);
    }
}

/* ---- lifecycle -------------------------------------------------------- */

esp_gsp_err_t app_radio_ui_create(esp_gsp_handle_t ui, const app_radio_ui_config_t *config,
                                  app_radio_ui_handle_t *out_handle)
{
    if (!out_handle) {
        return ESP_GSP_ERR_INVALID_ARG;
    }
    *out_handle = NULL;
    if (!ui || !config || app_stations_count() == 0) {
        return ESP_GSP_ERR_INVALID_ARG;
    }
    struct app_radio_ui *s = calloc(1, sizeof(*s));
    if (!s) {
        return ESP_GSP_ERR_NO_MEM;
    }
    s->ui = ui;
    s->sink = config->sink;
    s->shown_locked = -2; /* force the first render */
    atomic_init(&s->report, REPORT_PACK(APP_RADIO_LINK_CONNECTING, APP_RADIO_AUDIO_IDLE, APP_STATIONS_NONE));
    radio_tuner_init(&s->tuner, config->initial_freq_tenths, config->initial_volume);
    render_dial(s);
    render_transport(s);
    refresh_status(s, true);

    esp_gsp_err_t err = s->last_error;
    if (err == ESP_GSP_OK) {
        s->status_timer = esp_gsp_timer_create(ui, STATUS_REFRESH_MS, status_tick, s);
        err = s->status_timer ? ESP_GSP_OK : ESP_GSP_ERR_NO_MEM;
    }
    if (err == ESP_GSP_OK) {
        /* Publish only a fully initialized context; dispatch may start now. */
        err = esp_gsp_on_event(ui, on_event, s);
    }
    if (err != ESP_GSP_OK) {
        if (s->status_timer) {
            (void)esp_gsp_timer_delete(ui, s->status_timer);
        }
        free(s);
        return err;
    }
    if (s->sink.set_volume) {
        s->sink.set_volume(s->tuner.volume, s->sink.ctx);
    }
    commit_tuning(s);
    *out_handle = s;
    return ESP_GSP_OK;
}

void app_radio_ui_report(app_radio_ui_handle_t handle, app_radio_link_t link,
                         app_radio_audio_t audio, int station)
{
    if (handle) {
        atomic_store(&handle->report, REPORT_PACK(link, audio, station));
    }
}

esp_gsp_err_t app_radio_ui_read(app_radio_ui_handle_t handle, app_radio_ui_snapshot_t *out)
{
    if (!handle || !out) {
        return ESP_GSP_ERR_INVALID_ARG;
    }
    *out = (app_radio_ui_snapshot_t) {
        .freq_tenths = handle->tuner.freq_tenths,
        .locked_station = handle->tuner.locked,
        .committed_station = handle->tuner.committed,
        .paused = handle->tuner.paused,
        .volume = handle->tuner.volume,
        .last_error = handle->last_error,
    };
    return ESP_GSP_OK;
}

esp_gsp_err_t app_radio_ui_destroy(app_radio_ui_handle_t *handle)
{
    if (!handle) {
        return ESP_GSP_ERR_INVALID_ARG;
    }
    struct app_radio_ui *s = *handle;
    if (!s) {
        return ESP_GSP_OK;
    }
    esp_gsp_err_t err = esp_gsp_on_event(s->ui, NULL, NULL);
    if (err == ESP_GSP_OK && s->status_timer) {
        err = esp_gsp_timer_delete(s->ui, s->status_timer);
    }
    if (err != ESP_GSP_OK) {
        return err;
    }
    free(s);
    *handle = NULL;
    return ESP_GSP_OK;
}
