// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque stream player; contracts in ../README.md. */
typedef struct app_radio_player *app_radio_player_handle_t;

typedef enum {
    APP_RADIO_PLAYER_IDLE = 0,   /**< Stopped, paused, off-station or offline. */
    APP_RADIO_PLAYER_BUFFERING,  /**< Connecting to the stream / decoding first frames. */
    APP_RADIO_PLAYER_PLAYING,    /**< Audio is reaching the speaker. */
    APP_RADIO_PLAYER_ERROR,      /**< Stream failed; retrying with backoff. */
} app_radio_player_state_t;

/** Runs on the player's own tasks; must not block. station is the
 * app_stations index the state refers to (or -1). */
typedef void (*app_radio_player_status_cb_t)(app_radio_player_state_t state, int station, void *ctx);

typedef struct {
    uint8_t initial_volume;                 /**< Percent 0..100. */
    app_radio_player_status_cb_t status_cb; /**< May be NULL. */
    void *ctx;
} app_radio_player_config_t;

/** Last station frequency and volume saved by a previous boot. Leaves the
 * outputs unchanged and returns ESP_ERR_NOT_FOUND when nothing is saved. */
esp_err_t app_radio_player_load_saved(uint16_t *out_freq_tenths, uint8_t *out_volume);

/** Initialise the speaker and start the player task (once per boot). */
esp_err_t app_radio_player_start(const app_radio_player_config_t *config,
                                 app_radio_player_handle_t *out_handle);

/* Requests below are non-blocking, callable from any task (including the GSP
 * render task), and latest-wins: only the newest value of each is applied. */

/** Play app_stations index station, or stop when station < 0. */
void app_radio_player_tune(app_radio_player_handle_t h, int station);
void app_radio_player_set_paused(app_radio_player_handle_t h, bool paused);
void app_radio_player_set_volume(app_radio_player_handle_t h, uint8_t volume_percent);
/** Tell the player whether the network can reach the internet. */
void app_radio_player_set_online(app_radio_player_handle_t h, bool online);

#ifdef __cplusplus
}
#endif
