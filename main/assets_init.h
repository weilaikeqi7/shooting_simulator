#pragma once

#include "esp_err.h"
#include "lvgl.h"

/* Initialize mmap assets, the LVGL flash filesystem, image decoder, and MiSans FreeType fonts. */
esp_err_t app_assets_init(void);

const lv_font_t *app_assets_font_14(void);
const lv_font_t *app_assets_font_20(void);
const lv_font_t *app_assets_font_24(void);
const lv_font_t *app_assets_font_26(void);
