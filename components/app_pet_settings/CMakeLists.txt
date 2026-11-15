idf_component_register(SRCS "app_pet_settings.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common app_pet_core
    PRIV_REQUIRES nvs_flash esp_hw_support log freertos)
