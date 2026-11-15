idf_component_register(SRCS "app_pet_brain.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common
    PRIV_REQUIRES app_pet_settings app_pet_core app_stations esp_http_client mbedtls
        espressif__cjson heap log freertos)
