idf_component_register(SRCS "app_pet_audio.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common app_pet_core
    PRIV_REQUIRES esp-mosaico-bsp esp_audio_simple_player esp_codec_dev heap log freertos)
