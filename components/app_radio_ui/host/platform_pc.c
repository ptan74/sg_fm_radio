// SPDX-License-Identifier: Apache-2.0
/* Native simulator backend: runs the shared radio controller and stands in for
 * the device audio player. Tune requests are logged and "start playing" after
 * SIM_BUFFER_MS so the status pill can be exercised without Wi-Fi or audio. */
#include <stdio.h>

#include "app_radio_ui.h"
#include "app_stations.h"
#include "gsp_sim_bridge.h"

#define SIM_BUFFER_MS 700
#define SIM_START_FREQ_TENTHS 950
#define SIM_START_VOLUME 60

/* The bridge ABI has no caller context; keep the backend's owned state here. */
static struct {
    esp_gsp_handle_t ui;
    app_radio_ui_handle_t controller;
    void *buffer_timer;
    int pending_station;
} backend;

static void buffering_done(esp_gsp_handle_t ui, void *ctx)
{
    (void)ctx;
    if (backend.buffer_timer) {
        (void)esp_gsp_timer_delete(ui, backend.buffer_timer);
        backend.buffer_timer = NULL;
    }
    app_radio_ui_report(backend.controller, APP_RADIO_LINK_ONLINE, APP_RADIO_AUDIO_PLAYING,
                        backend.pending_station);
}

static void sim_tune(int station, void *ctx)
{
    (void)ctx;
    const app_station_t *s = station < 0 ? NULL : app_stations_get((size_t)station);
    fprintf(stderr, "radio_backend: tune %s\n", s ? s->name : "(off-station, stop)");
    backend.pending_station = station;
    if (backend.buffer_timer) {
        (void)esp_gsp_timer_delete(backend.ui, backend.buffer_timer);
        backend.buffer_timer = NULL;
    }
    app_radio_ui_report(backend.controller, APP_RADIO_LINK_ONLINE,
                        station < 0 ? APP_RADIO_AUDIO_IDLE : APP_RADIO_AUDIO_BUFFERING, station);
    if (station >= 0) {
        backend.buffer_timer = esp_gsp_timer_create(backend.ui, SIM_BUFFER_MS, buffering_done, NULL);
    }
}

static void sim_paused(bool paused, void *ctx)
{
    (void)ctx;
    fprintf(stderr, "radio_backend: %s\n", paused ? "pause" : "resume");
}

static void sim_volume(uint8_t volume, void *ctx)
{
    (void)ctx;
    fprintf(stderr, "radio_backend: volume %u%%\n", (unsigned)volume);
}

esp_gsp_err_t gsp_bridge_app_init(esp_gsp_handle_t ui)
{
    backend.ui = ui;
    backend.pending_station = -1;
    const app_radio_ui_config_t config = {
        .initial_freq_tenths = SIM_START_FREQ_TENTHS,
        .initial_volume = SIM_START_VOLUME,
        .sink = {.tune = sim_tune, .set_paused = sim_paused, .set_volume = sim_volume},
    };
    const esp_gsp_err_t err = app_radio_ui_create(ui, &config, &backend.controller);
    if (err == ESP_GSP_OK && backend.pending_station >= 0) {
        /* The initial tune ran before the handle existed; report it now. */
        sim_tune(backend.pending_station, NULL);
    }
    return err;
}

void gsp_bridge_app_deinit(esp_gsp_handle_t ui)
{
    app_radio_ui_snapshot_t snap;
    if (app_radio_ui_read(backend.controller, &snap) == ESP_GSP_OK) {
        fprintf(stderr, "radio_backend: freq=%u locked=%d committed=%d paused=%d volume=%u last_error=%d\n",
                (unsigned)snap.freq_tenths, snap.locked_station, snap.committed_station,
                (int)snap.paused, (unsigned)snap.volume, (int)snap.last_error);
    }
    if (backend.buffer_timer) {
        (void)esp_gsp_timer_delete(ui, backend.buffer_timer);
        backend.buffer_timer = NULL;
    }
    /* The bridge calls deinit after poll exits; no callbacks can use the state. */
    const esp_gsp_err_t err = app_radio_ui_destroy(&backend.controller);
    if (err != ESP_GSP_OK) {
        fprintf(stderr, "radio_backend: cleanup failed (%d)\n", err);
    }
}
