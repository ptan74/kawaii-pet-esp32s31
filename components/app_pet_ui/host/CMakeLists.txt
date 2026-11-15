cmake_minimum_required(VERSION 3.20)
project(pet_backend LANGUAGES C)

if(NOT ESP_GSP_COMPONENT_DIR AND DEFINED ENV{ESP_GSP_COMPONENT_DIR})
    set(ESP_GSP_COMPONENT_DIR "$ENV{ESP_GSP_COMPONENT_DIR}")
endif()
if(NOT ESP_GSP_COMPONENT_DIR)
    foreach(candidate IN ITEMS
            "${CMAKE_CURRENT_LIST_DIR}/../../../managed_components/espressif__esp-gsp")
        get_filename_component(candidate "${candidate}" ABSOLUTE)
        if(EXISTS "${candidate}/tools/sim_bridge/CMakeLists.txt")
            set(ESP_GSP_COMPONENT_DIR "${candidate}")
            break()
        endif()
    endforeach()
endif()
if(NOT EXISTS "${ESP_GSP_COMPONENT_DIR}/tools/sim_bridge/CMakeLists.txt")
    message(FATAL_ERROR
        "Install espressif/esp-gsp first, or set ESP_GSP_COMPONENT_DIR")
endif()

set(CORE ../../app_pet_core)
add_subdirectory("${ESP_GSP_COMPONENT_DIR}/tools/sim_bridge" sim_bridge)
gsp_add_backend(pet_backend
    SOURCES ../app_pet_ui.c ../private/radio_tuner.c ../private/radio_screen.c ../../app_stations/app_stations.c ${CORE}/pet_types.c ${CORE}/pet_text.c ${CORE}/pet_render.c
        ${CORE}/pet_life.c ${CORE}/pet_synth.c platform_pc.c
    INCLUDE_DIRECTORIES ../include ../private ../../app_stations/include  ${CORE}/include
    SCENES ../ui/main.json)
if(NOT MSVC)
    target_link_libraries(pet_backend PRIVATE m)
endif()
