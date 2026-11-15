idf_component_register(SRCS "app_pet_net.c" "setup_web.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "private"
    REQUIRES esp_common
    PRIV_REQUIRES app_pet_settings app_pet_core esp_wifi esp_netif esp_event esp_http_server
        nvs_flash log freertos)
