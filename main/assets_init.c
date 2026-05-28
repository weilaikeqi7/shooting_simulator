#include "assets_init.h"

#include <stdbool.h>

#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_lv_decoder.h"
#include "esp_lv_fs.h"
#include "esp_mmap_assets.h"
#include "lvgl.h"
#include "src/libs/freetype/lv_freetype_private.h"

static const char *TAG = "assets";

#define ASSETS_PARTITION_LABEL "assets"
#define ASSETS_FS_LETTER      'A'
#define MISANS_FONT_PATH      "A:MiSans-Regular.ttf"
#define FREETYPE_GLYPH_CACHE  96

static mmap_assets_handle_t s_mmap_assets;
static esp_lv_fs_handle_t s_lv_fs;
static esp_lv_decoder_handle_t s_decoder;
static bool s_freetype_ready;

static lv_font_t *s_font_14;
static lv_font_t *s_font_20;
static lv_font_t *s_font_24;
static lv_font_t *s_font_26;

static esp_err_t init_mmap_assets(void)
{
    if (s_mmap_assets && s_lv_fs) {
        return ESP_OK;
    }

    const mmap_assets_config_t asset_cfg = {
        .partition_label = ASSETS_PARTITION_LABEL,
        .flags = {
            .mmap_enable = true,
            .metadata_check = true,
        },
    };

    ESP_RETURN_ON_ERROR(mmap_assets_new(&asset_cfg, &s_mmap_assets), TAG, "mount mmap assets");

    const int stored_files = mmap_assets_get_stored_files(s_mmap_assets);
    ESP_RETURN_ON_FALSE(stored_files > 0, ESP_ERR_NOT_FOUND, TAG, "no assets in mmap partition");

    const fs_cfg_t fs_cfg = {
        .fs_letter = ASSETS_FS_LETTER,
        .fs_nums = stored_files,
        .fs_assets = s_mmap_assets,
    };
    ESP_RETURN_ON_ERROR(esp_lv_fs_desc_init(&fs_cfg, &s_lv_fs), TAG, "register LVGL mmap fs");

    for (int i = 0; i < stored_files; i++) {
        ESP_LOGI(TAG, "Asset[%d]: %s (%d bytes)", i,
                 mmap_assets_get_name(s_mmap_assets, i),
                 mmap_assets_get_size(s_mmap_assets, i));
    }

    return ESP_OK;
}

static esp_err_t init_decoder(void)
{
    if (s_decoder) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(esp_lv_decoder_init(&s_decoder), TAG, "init LVGL image decoder");
    ESP_LOGI(TAG, "LVGL image decoder initialized");
    return ESP_OK;
}

static esp_err_t init_freetype(void)
{
#if LV_USE_FREETYPE
    if (!s_freetype_ready) {
        if (!lv_freetype_get_context()) {
            ESP_RETURN_ON_FALSE(lv_freetype_init(FREETYPE_GLYPH_CACHE) == LV_RESULT_OK,
                                ESP_FAIL, TAG, "init FreeType failed");
        }
        ESP_RETURN_ON_FALSE(lv_freetype_get_context(), ESP_FAIL, TAG, "FreeType context unavailable");
        s_freetype_ready = true;
        ESP_LOGI(TAG, "FreeType ready");
    }
    return ESP_OK;
#else
    ESP_LOGE(TAG, "LVGL FreeType is disabled");
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

static esp_err_t create_font(lv_font_t **font, uint32_t size)
{
#if LV_USE_FREETYPE
    if (*font) {
        return ESP_OK;
    }

    *font = lv_freetype_font_create(MISANS_FONT_PATH,
                                    LV_FREETYPE_FONT_RENDER_MODE_BITMAP,
                                    size,
                                    LV_FREETYPE_FONT_STYLE_NORMAL);
    ESP_RETURN_ON_FALSE(*font, ESP_ERR_NO_MEM, TAG,
                        "create FreeType font %lu from %s failed",
                        (unsigned long)size, MISANS_FONT_PATH);
    return ESP_OK;
#else
    (void)font;
    (void)size;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

static esp_err_t init_fonts(void)
{
    ESP_RETURN_ON_ERROR(init_freetype(), TAG, "init freetype");
    ESP_RETURN_ON_ERROR(create_font(&s_font_14, 14), TAG, "create font 14");
    ESP_RETURN_ON_ERROR(create_font(&s_font_20, 20), TAG, "create font 20");
    ESP_RETURN_ON_ERROR(create_font(&s_font_24, 24), TAG, "create font 24");
    ESP_RETURN_ON_ERROR(create_font(&s_font_26, 26), TAG, "create font 26");
    return ESP_OK;
}

esp_err_t app_assets_init(void)
{
    ESP_RETURN_ON_ERROR(init_mmap_assets(), TAG, "init mmap assets");
    ESP_RETURN_ON_ERROR(init_decoder(), TAG, "init image decoder");
    ESP_RETURN_ON_ERROR(init_fonts(), TAG, "init fonts");
    return ESP_OK;
}

const lv_font_t *app_assets_font_14(void)
{
    return s_font_14 ? s_font_14 : &lv_font_montserrat_14;
}

const lv_font_t *app_assets_font_20(void)
{
    return s_font_20 ? s_font_20 : &lv_font_montserrat_20;
}

const lv_font_t *app_assets_font_24(void)
{
    return s_font_24 ? s_font_24 : &lv_font_montserrat_24;
}

const lv_font_t *app_assets_font_26(void)
{
    return s_font_26 ? s_font_26 : &lv_font_montserrat_26;
}
