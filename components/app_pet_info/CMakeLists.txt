idf_component_register(SRCS "app_pet_info.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_http_client mbedtls espressif__cjson lwip log freertos)
