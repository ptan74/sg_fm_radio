idf_component_register(SRCS "app_radio_player.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common
    PRIV_REQUIRES app_stations esp-mosaico-bsp esp_audio_simple_player esp_codec_dev
        nvs_flash log freertos)
