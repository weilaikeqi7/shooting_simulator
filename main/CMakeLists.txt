set(srcs
    "main.c"
    "color_test.c"
    "remote_hid.c"
    "shooting_app.c"
    "assets_init.c"
    "esp_timer_stop_blocking_compat.c"
    "esp_hid_gap.c"
)
set(include_dirs ".")

set(mmap_asset_build_dir "${CMAKE_CURRENT_BINARY_DIR}/mmap_assets")
set(mmap_asset_files
    "${mmap_asset_build_dir}/front_sight.png"
    "${mmap_asset_build_dir}/MiSans-Regular.ttf"
)

# Keep the RGB partial draw buffer in PSRAM. esp_lvgl_adapter v0.5.0
# allocates this buffer in internal RAM even when the display profile uses PSRAM.
set(esp_lvgl_adapter_display_manager
    "${PROJECT_DIR}/managed_components/espressif__esp_lvgl_adapter/src/display/display_manager.c")
if(EXISTS "${esp_lvgl_adapter_display_manager}")
    file(READ "${esp_lvgl_adapter_display_manager}" adapter_display_manager_src)
    string(REPLACE
        "void *buf = display_manager_alloc_draw_buffer(cfg->draw_buf_pixels * color_size, false);"
        "void *buf = display_manager_alloc_draw_buffer(cfg->draw_buf_pixels * color_size, profile->use_psram);"
        adapter_display_manager_patched
        "${adapter_display_manager_src}"
    )
    if(NOT adapter_display_manager_patched STREQUAL adapter_display_manager_src)
        file(WRITE "${esp_lvgl_adapter_display_manager}" "${adapter_display_manager_patched}")
    endif()
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS ${include_dirs}
    REQUIRES driver espressif__button esp_lvgl_adapter esp_lv_decoder esp_lv_fs esp_mmap_assets hw_init esp_hid nvs_flash bt esp_event heap esp_psram esp_timer esp_system
)

add_custom_command(
    OUTPUT ${mmap_asset_files}
    COMMAND ${CMAKE_COMMAND} -E make_directory "${mmap_asset_build_dir}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_LIST_DIR}/assets/front_sight.png"
            "${mmap_asset_build_dir}/front_sight.png"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_LIST_DIR}/assets/MiSans/ttf/MiSans-Regular.ttf"
            "${mmap_asset_build_dir}/MiSans-Regular.ttf"
    DEPENDS
            "${CMAKE_CURRENT_LIST_DIR}/assets/front_sight.png"
            "${CMAKE_CURRENT_LIST_DIR}/assets/MiSans/ttf/MiSans-Regular.ttf"
    VERBATIM
)
add_custom_target(app_mmap_assets_prepare DEPENDS ${mmap_asset_files})

# ESP-IDF v6.0.1 does not expose esp_timer_stop_blocking(), but esp_lvgl_adapter
# v0.4.3 uses it when building against IDF 6.x. Inject a compatibility prototype
# for that managed component and provide a weak fallback implementation in main.
idf_component_get_property(esp_lvgl_adapter_lib espressif__esp_lvgl_adapter COMPONENT_LIB)
target_compile_options(${esp_lvgl_adapter_lib} PRIVATE
    "SHELL:-include ${CMAKE_CURRENT_LIST_DIR}/esp_timer_stop_blocking_compat.h"
)
string(CONCAT mmap_assets_pack_command "spi" "ffs_create_partition_assets")
cmake_language(CALL ${mmap_assets_pack_command}
    assets
    "${mmap_asset_build_dir}"
    FLASH_IN_PROJECT
    MMAP_FILE_SUPPORT_FORMAT ".png,.ttf"
    IMPORT_INC_PATH "${CMAKE_CURRENT_BINARY_DIR}"
    DEPENDS app_mmap_assets_prepare
)

# Optional: idf.py build -DAPP_ENABLE_COLOR_TEST=1
if(DEFINED APP_ENABLE_COLOR_TEST)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE APP_ENABLE_COLOR_TEST=${APP_ENABLE_COLOR_TEST})
endif()
