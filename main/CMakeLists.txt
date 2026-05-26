set(srcs
    "main.c"
    "color_test.c"
    "remote_hid.c"
    "shooting_app.c"
    "assets_init.c"
    "esp_hid_gap.c"
)
set(include_dirs ".")

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS ${include_dirs}
    REQUIRES driver espressif__button esp_lvgl_adapter esp_lv_decoder hw_init esp_hid nvs_flash bt esp_event spiffs
)

spiffs_create_partition_image(assets assets FLASH_IN_PROJECT)

# 可选：idf.py build -DAPP_ENABLE_COLOR_TEST=1
if(DEFINED APP_ENABLE_COLOR_TEST)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE APP_ENABLE_COLOR_TEST=${APP_ENABLE_COLOR_TEST})
endif()
