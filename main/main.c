// SPDX-License-Identifier: Apache-2.0
#include <stdatomic.h>

#include "app_display.h"
#include "app_radio_player.h"
#include "app_radio_ui.h"
#include "app_wifi.h"
#include "esp_iris.h"
#include "esp_log.h"
#include "iris_ota_support.h"
#include "iris_screen_mirror.h"
#include "nvs_flash.h"
#include "ui_bundle.h"

static const char *TAG = "sg_fm_radio";

#define DEFAULT_FREQ_TENTHS 950 /* Class 95 */
#define DEFAULT_VOLUME 60

/* The composition root retains owned handles, never component internals.
 * `controller` and `link` are read from Wi-Fi/player tasks, hence atomics;
 * all handles live until reboot. */
static struct {
    esp_gsp_handle_t ui;
    _Atomic(app_radio_ui_handle_t) controller;
    app_radio_player_handle_t player;
    _Atomic app_radio_link_t link;
} application;

/* ---- wiring: UI -> player (GSP render task; all calls are non-blocking) --- */

static void ui_tune(int station, void *ctx)
{
    (void)ctx;
    app_radio_player_tune(application.player, station);
}

static void ui_paused(bool paused, void *ctx)
{
    (void)ctx;
    app_radio_player_set_paused(application.player, paused);
}

static void ui_volume(uint8_t volume, void *ctx)
{
    (void)ctx;
    app_radio_player_set_volume(application.player, volume);
}

/* ---- wiring: player/Wi-Fi -> UI (their own tasks) ------------------------- */

static void player_status(app_radio_player_state_t state, int station, void *ctx)
{
    (void)ctx;
    static const app_radio_audio_t map[] = {
        [APP_RADIO_PLAYER_IDLE] = APP_RADIO_AUDIO_IDLE,
        [APP_RADIO_PLAYER_BUFFERING] = APP_RADIO_AUDIO_BUFFERING,
        [APP_RADIO_PLAYER_PLAYING] = APP_RADIO_AUDIO_PLAYING,
        [APP_RADIO_PLAYER_ERROR] = APP_RADIO_AUDIO_ERROR,
    };
    app_radio_ui_report(atomic_load(&application.controller), atomic_load(&application.link),
                        map[state], station);
}

static void wifi_state(app_wifi_state_t state, void *ctx)
{
    (void)ctx;
    const app_radio_link_t link = state == APP_WIFI_CONNECTED ? APP_RADIO_LINK_ONLINE
                                  : state == APP_WIFI_CONNECTING ? APP_RADIO_LINK_CONNECTING
                                  : APP_RADIO_LINK_OFFLINE;
    atomic_store(&application.link, link);
    app_radio_player_set_online(application.player, link == APP_RADIO_LINK_ONLINE);
    /* Audio state follows from the player once it reacts to the link change. */
    app_radio_ui_report(atomic_load(&application.controller), link, APP_RADIO_AUDIO_IDLE, -1);
}

void app_main(void)
{
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_iris_boot_probe());
    ESP_ERROR_CHECK(nvs_flash_init());
    /* Recovery must remain reachable even when the UI bundle cannot be loaded. */
    iris_ota_support_start();

    esp_gsp_config_t config;
    const esp_err_t err = ui_bundle_open(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GSP UI unavailable (0x%x); Recovery RPC remains active", err);
        return;
    }
    esp_gsp_esp_lcd_config_t display;
    const esp_err_t display_err = app_display_init(&display);
    if (display_err != ESP_OK) {
        ESP_LOGE(TAG, "Display unavailable (0x%x); Recovery RPC remains active", display_err);
        return;
    }
    ESP_ERROR_CHECK(iris_screen_mirror_init());
    ESP_ERROR_CHECK(esp_gsp_esp_lcd_start(&config, &display, &application.ui));
    ESP_ERROR_CHECK(iris_screen_mirror_attach(application.ui));

    uint16_t freq = DEFAULT_FREQ_TENTHS;
    uint8_t volume = DEFAULT_VOLUME;
    (void)app_radio_player_load_saved(&freq, &volume);

    atomic_init(&application.link, APP_RADIO_LINK_CONNECTING);
    const app_radio_player_config_t player_cfg = {
        .initial_volume = volume,
        .status_cb = player_status,
    };
    /* Without audio the dial still works; the player setters ignore NULL. */
    ESP_ERROR_CHECK_WITHOUT_ABORT(app_radio_player_start(&player_cfg, &application.player));

    const app_radio_ui_config_t ui_cfg = {
        .initial_freq_tenths = freq,
        .initial_volume = volume,
        .sink = {.tune = ui_tune, .set_paused = ui_paused, .set_volume = ui_volume},
    };
    app_radio_ui_handle_t controller;
    ESP_ERROR_CHECK(app_radio_ui_create(application.ui, &ui_cfg, &controller));
    atomic_store(&application.controller, controller);

    /* Started last so its first reports reach a live controller and player. */
    ESP_ERROR_CHECK_WITHOUT_ABORT(app_wifi_start(wifi_state, NULL));
    /* GSP retains the controller context; UI, controller and player live until
     * reboot. Teardown would require quiescing GSP before app_radio_ui_destroy(). */
    ESP_LOGI(TAG, "SG FM radio ready at %u.%u MHz", (unsigned)(freq / 10), (unsigned)(freq % 10));
}
