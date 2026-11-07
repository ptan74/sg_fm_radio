// SPDX-License-Identifier: Apache-2.0
#include "app_wifi.h"

#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "app_wifi";

/* Vibe Mode keeps the user's network in the retained system NVS. Read it only;
 * Vibe Mode owns writes (Settings > Wi-Fi there). */
#define SHARED_PARTITION "sysmeta"
#define SHARED_NAMESPACE "shared"
#define SHARED_SSID_KEY "wifi_ssid"
#define SHARED_PASSWORD_KEY "wifi_password"

#define RETRY_MIN_MS 1000U
#define RETRY_MAX_MS 30000U

/* Single instance per boot. After start, fields are touched only from the
 * default event task and the retry timer callback, which ESP-IDF serializes
 * with respect to this module's use (timer only calls esp_wifi_connect). */
static struct {
    bool started;
    app_wifi_state_cb_t cb;
    void *ctx;
    TimerHandle_t retry_timer;
    uint32_t retry_ms;
} s_wifi;

static void notify(app_wifi_state_t state)
{
    if (s_wifi.cb) {
        s_wifi.cb(state, s_wifi.ctx);
    }
}

static bool load_shared_credentials(wifi_config_t *cfg)
{
    /* Iris normally opens sysmeta first; initializing again is harmless. */
    esp_err_t err = nvs_flash_init_partition(SHARED_PARTITION);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "system NVS unavailable (%s)", esp_err_to_name(err));
        return false;
    }
    nvs_handle_t h;
    err = nvs_open_from_partition(SHARED_PARTITION, SHARED_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return false;
    }
    size_t ssid_len = sizeof(cfg->sta.ssid);
    size_t pass_len = sizeof(cfg->sta.password);
    err = nvs_get_str(h, SHARED_SSID_KEY, (char *)cfg->sta.ssid, &ssid_len);
    if (err == ESP_OK) {
        esp_err_t perr = nvs_get_str(h, SHARED_PASSWORD_KEY, (char *)cfg->sta.password, &pass_len);
        if (perr == ESP_ERR_NVS_NOT_FOUND) {
            cfg->sta.password[0] = '\0';
        } else if (perr != ESP_OK) {
            err = perr;
        }
    }
    nvs_close(h);
    return err == ESP_OK && cfg->sta.ssid[0] != '\0';
}

static bool load_credentials(wifi_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    if (load_shared_credentials(cfg)) {
        ESP_LOGI(TAG, "using Vibe Mode network \"%s\"", (const char *)cfg->sta.ssid);
        return true;
    }
    memset(cfg, 0, sizeof(*cfg));
    if (CONFIG_APP_WIFI_SSID[0] == '\0') {
        return false;
    }
    strlcpy((char *)cfg->sta.ssid, CONFIG_APP_WIFI_SSID, sizeof(cfg->sta.ssid));
    strlcpy((char *)cfg->sta.password, CONFIG_APP_WIFI_PASSWORD, sizeof(cfg->sta.password));
    ESP_LOGI(TAG, "using build-time network \"%s\"", CONFIG_APP_WIFI_SSID);
    return true;
}

static void retry_cb(TimerHandle_t timer)
{
    (void)timer;
    (void)esp_wifi_connect();
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        (void)esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *ev = data;
        ESP_LOGW(TAG, "disconnected (reason %u); retry in %u ms", ev->reason, (unsigned)s_wifi.retry_ms);
        notify(APP_WIFI_CONNECTING);
        /* Back off so a wrong password or absent AP does not spin the radio. */
        xTimerChangePeriod(s_wifi.retry_timer, pdMS_TO_TICKS(s_wifi.retry_ms), 0);
        s_wifi.retry_ms = s_wifi.retry_ms * 2 > RETRY_MAX_MS ? RETRY_MAX_MS : s_wifi.retry_ms * 2;
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *ev = data;
        ESP_LOGI(TAG, "connected, ip " IPSTR, IP2STR(&ev->ip_info.ip));
        s_wifi.retry_ms = RETRY_MIN_MS;
        notify(APP_WIFI_CONNECTED);
    }
}

static esp_err_t allow_existing(esp_err_t err)
{
    return err == ESP_ERR_INVALID_STATE ? ESP_OK : err;
}

esp_err_t app_wifi_start(app_wifi_state_cb_t state_cb, void *ctx)
{
    ESP_RETURN_ON_FALSE(!s_wifi.started, ESP_ERR_INVALID_STATE, TAG, "already started");
    s_wifi.cb = state_cb;
    s_wifi.ctx = ctx;
    s_wifi.retry_ms = RETRY_MIN_MS;

    wifi_config_t cfg;
    if (!load_credentials(&cfg)) {
        ESP_LOGW(TAG, "no Wi-Fi saved; set it in Vibe Mode or CONFIG_APP_WIFI_SSID");
        notify(APP_WIFI_NO_CREDENTIALS);
        return ESP_OK;
    }
    cfg.sta.threshold.authmode = cfg.sta.password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    s_wifi.retry_timer = xTimerCreate("wifi_retry", pdMS_TO_TICKS(RETRY_MIN_MS), pdFALSE, NULL, retry_cb);
    ESP_RETURN_ON_FALSE(s_wifi.retry_timer, ESP_ERR_NO_MEM, TAG, "retry timer");

    esp_err_t err = allow_existing(esp_netif_init());
    if (err == ESP_OK) {
        err = allow_existing(esp_event_loop_create_default());
    }
    if (err == ESP_OK && esp_netif_create_default_wifi_sta() == NULL) {
        err = ESP_ERR_NO_MEM;
    }
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    if (err == ESP_OK) {
        err = esp_wifi_init(&init);
    }
    if (err == ESP_OK) {
        err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    }
    if (err == ESP_OK) {
        err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);
    }
    if (err == ESP_OK) {
        /* Credentials stay in RAM; Vibe Mode remains the only writer. */
        err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    }
    if (err == ESP_OK) {
        err = esp_wifi_set_mode(WIFI_MODE_STA);
    }
    if (err == ESP_OK) {
        err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    }
    memset(&cfg, 0, sizeof(cfg)); /* do not leave the password on the stack */
    if (err == ESP_OK) {
        notify(APP_WIFI_CONNECTING);
        err = esp_wifi_start();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi start failed: %s", esp_err_to_name(err));
        (void)esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event);
        (void)esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event);
        (void)esp_wifi_deinit();
        xTimerDelete(s_wifi.retry_timer, 0);
        s_wifi.retry_timer = NULL;
        notify(APP_WIFI_NO_CREDENTIALS);
        return err;
    }
    s_wifi.started = true;
    return ESP_OK;
}
