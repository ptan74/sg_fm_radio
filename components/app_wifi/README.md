# app_wifi

Brings up the Wi-Fi station once per boot and keeps it connected, reporting
`NO_CREDENTIALS`, `CONNECTING` and `CONNECTED` through a callback.

**Credentials.** The network saved in Vibe Mode (Settings > Wi-Fi) is read,
read-only, from the retained `sysmeta` NVS partition, namespace `shared`, keys
`wifi_ssid`/`wifi_password`. If none is saved, `CONFIG_APP_WIFI_SSID` and
`CONFIG_APP_WIFI_PASSWORD` (menu "SG FM Radio Wi-Fi") are used. Keep real
values out of Git, for example in an untracked `sdkconfig.local.defaults`.
Credentials are applied with `WIFI_STORAGE_RAM`, so this app never writes
Wi-Fi settings to flash; Vibe Mode stays the only writer.

**Lifecycle and threading.** `app_wifi_start()` is called once from
`app_main()` after `nvs_flash_init()`; a second call returns
`ESP_ERR_INVALID_STATE`. It creates the default event loop and STA netif if
they do not exist. The state callback runs on the ESP-IDF event task (and once
synchronously inside `app_wifi_start()`); keep it non-blocking. Disconnects
are retried with exponential backoff from 1 s up to 30 s. There is no stop
function: Wi-Fi lives until reboot.

ESP-Iris is built with `CONFIG_ESP_IRIS_TCP_DEFER_UNTIL_NETIF`, so its TCP
transport starts automatically once this component creates the netif.

Dependencies: `esp_wifi`, `esp_netif`, `esp_event`, `nvs_flash`, FreeRTOS timers.

```c
static void on_wifi(app_wifi_state_t state, void *ctx) { /* forward */ }
ESP_ERROR_CHECK(app_wifi_start(on_wifi, NULL));
```
