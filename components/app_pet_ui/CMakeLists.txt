idf_component_register(SRCS "app_pet_ui.c" "private/radio_tuner.c" "private/radio_screen.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "private"
    REQUIRES esp-gsp app_pet_core
    PRIV_REQUIRES app_stations app_pet_info)
include("${MOSAICO_UTILS_ROOT}/mosaico-tools/cmake/gsp_bundle.cmake")
mosaico_gsp_add_ui_bundle(${COMPONENT_LIB} "ui/main.json")
