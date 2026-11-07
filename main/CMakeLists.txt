idf_component_register(SRCS "main.c"
    PRIV_REQUIRES app_display app_radio_ui app_radio_player app_wifi esp_iris
        esp_mosaico_app_recovery esp_mosaico_gsp_bundle esp_mosaico_gsp_iris nvs_flash log)
