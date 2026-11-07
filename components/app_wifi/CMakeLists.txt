idf_component_register(SRCS "app_wifi.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common
    PRIV_REQUIRES esp_wifi esp_netif esp_event nvs_flash log freertos)
