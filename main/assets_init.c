#include "assets_init.h"

#include <stdbool.h>
#include <stddef.h>

#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_lv_decoder.h"
#include "esp_spiffs.h"

static const char *TAG = "assets";

static esp_lv_decoder_handle_t s_decoder;
static bool s_spiffs_ready;

static esp_err_t mount_assets_spiffs(void)
{
    if (s_spiffs_ready) {
        return ESP_OK;
    }

    const esp_vfs_spiffs_conf_t conf = {
        .base_path = "/assets",
        .partition_label = "assets",
        .max_files = 4,
        .format_if_mount_failed = false,
    };

    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "Assets SPIFFS already mounted");
    } else if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount assets SPIFFS: %s", esp_err_to_name(ret));
        return ret;
    }

    size_t total = 0;
    size_t used = 0;
    ret = esp_spiffs_info("assets", &total, &used);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Assets SPIFFS mounted: %u/%u bytes", (unsigned)used, (unsigned)total);
    } else {
        ESP_LOGW(TAG, "Assets SPIFFS info unavailable: %s", esp_err_to_name(ret));
    }

    s_spiffs_ready = true;
    return ESP_OK;
}

esp_err_t app_assets_init(void)
{
    ESP_RETURN_ON_ERROR(mount_assets_spiffs(), TAG, "mount assets");

    if (s_decoder == NULL) {
        ESP_RETURN_ON_ERROR(esp_lv_decoder_init(&s_decoder), TAG, "init LVGL image decoder");
        ESP_LOGI(TAG, "LVGL image decoder initialized");
    }

    return ESP_OK;
}