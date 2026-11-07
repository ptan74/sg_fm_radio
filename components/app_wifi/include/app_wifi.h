// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    APP_WIFI_NO_CREDENTIALS = 0, /**< Nothing saved in Vibe Mode or Kconfig. */
    APP_WIFI_CONNECTING,         /**< Associating, or reconnecting after a drop. */
    APP_WIFI_CONNECTED,          /**< Station has an IPv4 address. */
} app_wifi_state_t;

/** Called from the ESP-IDF default event task; must not block. */
typedef void (*app_wifi_state_cb_t)(app_wifi_state_t state, void *ctx);

/** Start the Wi-Fi station once per boot and keep it connected.
 * Credentials: Vibe Mode's shared setting, else CONFIG_APP_WIFI_SSID.
 * state_cb (may be NULL) receives the initial state before this returns.
 * Requires nvs_flash_init(). Returns ESP_OK, or an error with Wi-Fi stopped. */
esp_err_t app_wifi_start(app_wifi_state_cb_t state_cb, void *ctx);

#ifdef __cplusplus
}
#endif
