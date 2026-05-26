#pragma once

#include "esp_err.h"

/* Mount image assets and register the LVGL image decoder. Call after LVGL starts. */
esp_err_t app_assets_init(void);