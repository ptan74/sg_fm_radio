// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_gsp.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque radio screen controller; contracts in ../README.md. */
typedef struct app_radio_ui *app_radio_ui_handle_t;

/** Network state reported by the platform. */
typedef enum {
    APP_RADIO_LINK_OFFLINE = 0,  /**< No credentials, or gave up connecting. */
    APP_RADIO_LINK_CONNECTING,
    APP_RADIO_LINK_ONLINE,
} app_radio_link_t;

/** Playback state of the station most recently passed to sink.tune. */
typedef enum {
    APP_RADIO_AUDIO_IDLE = 0,
    APP_RADIO_AUDIO_BUFFERING,
    APP_RADIO_AUDIO_PLAYING,
    APP_RADIO_AUDIO_ERROR,
} app_radio_audio_t;

/** Audio side of the radio. Callbacks run on the GSP render task and must
 * return without blocking (queue the work). Any member may be NULL. */
typedef struct {
    /** Play station index (app_stations), or stop when station < 0. */
    void (*tune)(int station, void *ctx);
    void (*set_paused)(bool paused, void *ctx);
    /** volume_percent is 20..100 in steps of 20. */
    void (*set_volume)(uint8_t volume_percent, void *ctx);
    void *ctx;
} app_radio_ui_sink_t;

typedef struct {
    uint16_t initial_freq_tenths; /**< 875..1080; clamped. */
    uint8_t initial_volume;       /**< Percent; rounded to 20..100. */
    app_radio_ui_sink_t sink;     /**< Copied; ctx must outlive the controller. */
} app_radio_ui_config_t;

/** Diagnostic copy of controller state. */
typedef struct {
    uint16_t freq_tenths;
    int locked_station;      /**< Station under the needle, or -1. */
    int committed_station;   /**< Station last sent to sink.tune, or -1. */
    bool paused;
    uint8_t volume;
    esp_gsp_err_t last_error;
} app_radio_ui_snapshot_t;

/** Bind the radio scene on ui, install input handling and immediately tune
 * to the initial station (sink.tune is called before this returns).
 * Returns ESP_GSP_OK or a GSP error; *out_handle is NULL on failure. */
esp_gsp_err_t app_radio_ui_create(esp_gsp_handle_t ui, const app_radio_ui_config_t *config,
                                  app_radio_ui_handle_t *out_handle);

/** Report platform state. Thread-safe and non-blocking; callable from any
 * task. station identifies which tune request audio refers to; reports for
 * another station are ignored. The screen refreshes within ~150 ms. */
void app_radio_ui_report(app_radio_ui_handle_t handle, app_radio_link_t link,
                         app_radio_audio_t audio, int station);

/** Copy diagnostics; call on the render task or after dispatch stops. */
esp_gsp_err_t app_radio_ui_read(app_radio_ui_handle_t handle, app_radio_ui_snapshot_t *out);

/** Detach callbacks and free the controller. Quiesce GSP dispatch first. */
esp_gsp_err_t app_radio_ui_destroy(app_radio_ui_handle_t *handle);

#ifdef __cplusplus
}
#endif
