idf_component_register(SRCS "main.c"
    PRIV_REQUIRES app_display app_pet_ui app_pet_core app_pet_audio app_pet_brain app_pet_info app_pet_market app_pet_net
        app_pet_settings app_stations esp_iris esp_mosaico_app_recovery esp_mosaico_gsp_bundle
        esp_mosaico_gsp_iris nvs_flash esp_hw_support log freertos)
