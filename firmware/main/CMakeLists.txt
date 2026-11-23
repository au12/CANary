set(GEN "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(REPO "${CMAKE_CURRENT_LIST_DIR}/../..")
file(GLOB WEB_FILES "${REPO}/firmware/web/*")
file(GLOB PROFILE_FILES "${REPO}/profiles/*.json")

idf_component_register(
    SRCS "app_main.c" "hal.c" "sdcard.c" "plat.c" "net.c" "web.c" "profiles.c"
         "${GEN}/web_assets.c" "${GEN}/profiles_data.c"
    INCLUDE_DIRS "."
    REQUIRES canary_core
    PRIV_REQUIRES driver esp_adc esp_driver_uart esp_driver_gpio esp_driver_i2c esp_driver_sdmmc esp_timer esp_wifi esp_netif
                  esp_event nvs_flash fatfs sdmmc esp_http_server esp_http_client esp-tls app_update mbedtls lwip esp_system
                  esp_hw_support heap)

add_custom_command(
    OUTPUT "${GEN}/web_assets.c" "${GEN}/profiles_data.c"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${GEN}"
    COMMAND python "${REPO}/tools/gen_assets.py" --web "${REPO}/firmware/web" --profiles "${REPO}/profiles" --out "${GEN}"
            --version "${PROJECT_VER}"
    DEPENDS ${WEB_FILES} ${PROFILE_FILES} "${REPO}/tools/gen_assets.py"
    COMMENT "Packing web UI and vehicle profiles"
    VERBATIM)
set_source_files_properties("${GEN}/web_assets.c" "${GEN}/profiles_data.c" PROPERTIES GENERATED TRUE)
