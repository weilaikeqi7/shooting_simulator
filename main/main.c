#include <stdbool.h>

/**
 * @file main.c
 * @brief Unified LVGL demo supporting multiple LCD interface types
 *
 * This example demonstrates how to use LVGL with different LCD interfaces
 * (MIPI DSI, QSPI, RGB, SPI) in a single unified codebase.
 * The interface type and hardware configuration can be selected via menuconfig.
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "sdkconfig.h"
#include "hw_init.h"
#include "esp_lv_adapter.h"
#include "app_features.h"
#include "remote_hid.h"
#include "assets_init.h"
#include "shooting_app.h"
#if APP_ENABLE_COLOR_TEST
#include "driver/gpio.h"
#include "iot_button.h"
#include "button_gpio.h"
#include "color_test.h"
#endif

static const char *TAG = "main";

#if APP_ENABLE_COLOR_TEST
#define GPIO0_KEY_GPIO       GPIO_NUM_0
#endif

/* FPS monitor task configuration */
#define FPS_MONITOR_TASK_STACK_SIZE    4096
#define FPS_MONITOR_TASK_PRIORITY      3
#define FPS_MONITOR_INTERVAL_MS        1000

/* Memory monitor task configuration */
#define MEM_MONITOR_TASK_STACK_SIZE    4096
#define MEM_MONITOR_FALLBACK_STACK_SIZE 2048
#define MEM_MONITOR_TASK_PRIORITY      2
#define MEM_MONITOR_INTERVAL_MS        5000

/**
 * @brief Get the configured display rotation from Kconfig
 *
 * @return esp_lv_adapter_rotation_t Rotation angle
 */
static esp_lv_adapter_rotation_t get_configured_rotation(void)
{
#if CONFIG_EXAMPLE_DISPLAY_ROTATION_0
    return ESP_LV_ADAPTER_ROTATE_0;
#elif CONFIG_EXAMPLE_DISPLAY_ROTATION_90
    return ESP_LV_ADAPTER_ROTATE_90;
#elif CONFIG_EXAMPLE_DISPLAY_ROTATION_180
    return ESP_LV_ADAPTER_ROTATE_180;
#elif CONFIG_EXAMPLE_DISPLAY_ROTATION_270
    return ESP_LV_ADAPTER_ROTATE_270;
#else
    return ESP_LV_ADAPTER_ROTATE_0;
#endif
}

static uint32_t heap_used_percent_x10(size_t used, size_t total)
{
    if (total == 0) {
        return 0;
    }
    return (uint32_t)(((uint64_t)used * 1000U) / total);
}

typedef struct {
    size_t total;
    size_t used;
    size_t free_bytes;
    size_t min_free;
    size_t largest_free;
} heap_usage_t;

typedef struct {
    size_t internal_used;
    size_t psram_used;
} heap_checkpoint_t;

static heap_checkpoint_t s_last_heap_checkpoint;
static bool s_heap_checkpoint_valid;

static heap_usage_t get_heap_usage(uint32_t caps)
{
    multi_heap_info_t info = {0};
    heap_caps_get_info(&info, caps);

    heap_usage_t usage = {
        .total = info.total_free_bytes + info.total_allocated_bytes,
        .used = info.total_allocated_bytes,
        .free_bytes = info.total_free_bytes,
        .min_free = info.minimum_free_bytes,
        .largest_free = info.largest_free_block,
    };
    return usage;
}

static int32_t heap_delta_i32(size_t current, size_t previous)
{
    if (current >= previous) {
        return (int32_t)(current - previous);
    }
    return -(int32_t)(previous - current);
}

static void log_heap_usage(const char *name, uint32_t caps)
{
    heap_usage_t usage = get_heap_usage(caps);
    uint32_t used_pct_x10 = heap_used_percent_x10(usage.used, usage.total);

    ESP_LOGI(TAG, "%s used=%u/%u (%u.%u%%), free=%u, min_free=%u, largest=%u",
             name,
             (unsigned)usage.used,
             (unsigned)usage.total,
             (unsigned)(used_pct_x10 / 10U),
             (unsigned)(used_pct_x10 % 10U),
             (unsigned)usage.free_bytes,
             (unsigned)usage.min_free,
             (unsigned)usage.largest_free);
}

static void log_heap_checkpoint(const char *stage)
{
    heap_usage_t internal = get_heap_usage(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    heap_usage_t psram = get_heap_usage(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int32_t internal_delta = 0;
    int32_t psram_delta = 0;

    if (s_heap_checkpoint_valid) {
        internal_delta = heap_delta_i32(internal.used, s_last_heap_checkpoint.internal_used);
        psram_delta = heap_delta_i32(psram.used, s_last_heap_checkpoint.psram_used);
    }

    ESP_LOGI(TAG,
             "HEAP[%s] INTERNAL used=%u delta=%+d free=%u largest=%u | PSRAM used=%u delta=%+d free=%u largest=%u",
             stage,
             (unsigned)internal.used,
             (int)internal_delta,
             (unsigned)internal.free_bytes,
             (unsigned)internal.largest_free,
             (unsigned)psram.used,
             (int)psram_delta,
             (unsigned)psram.free_bytes,
             (unsigned)psram.largest_free);

    s_last_heap_checkpoint.internal_used = internal.used;
    s_last_heap_checkpoint.psram_used = psram.used;
    s_heap_checkpoint_valid = true;
}

static void memory_monitor_task(void *arg)
{
    (void)arg;

    while (1) {
        log_heap_usage("INTERNAL", MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        log_heap_usage("PSRAM", MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        vTaskDelay(pdMS_TO_TICKS(MEM_MONITOR_INTERVAL_MS));
    }
}

static void start_memory_monitor(void)
{
    log_heap_usage("INTERNAL", MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    log_heap_usage("PSRAM", MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    BaseType_t mem_task_ok = xTaskCreateWithCaps(memory_monitor_task, "mem_monitor",
                                                 MEM_MONITOR_TASK_STACK_SIZE,
                                                 NULL, MEM_MONITOR_TASK_PRIORITY, NULL,
                                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (mem_task_ok != pdPASS) {
        ESP_LOGW(TAG, "Memory monitor PSRAM stack failed, trying internal stack");
        mem_task_ok = xTaskCreate(memory_monitor_task, "mem_monitor",
                                  MEM_MONITOR_FALLBACK_STACK_SIZE,
                                  NULL, MEM_MONITOR_TASK_PRIORITY, NULL);
    }

    if (mem_task_ok != pdPASS) {
        ESP_LOGW(TAG, "Memory monitor task disabled: no memory");
    }
}
#if CONFIG_ESP_LVGL_ADAPTER_ENABLE_FPS_STATS
/**
 * @brief Task to monitor and log FPS statistics
 *
 * @param arg Pointer to lv_display_t
 */
static void fps_monitor_task(void *arg)
{
    lv_display_t *disp = (lv_display_t *)arg;
    uint32_t fps;

    while (1) {
        if (esp_lv_adapter_get_fps(disp, &fps) == ESP_OK) {
            ESP_LOGI(TAG, "Current FPS: %lu", fps);
        }
        vTaskDelay(pdMS_TO_TICKS(FPS_MONITOR_INTERVAL_MS));
    }
}
#endif

#if APP_ENABLE_COLOR_TEST
/** espressif/button：单击切换 Widgets / 纯色测试（回调在非 LVGL 任务中执行，需加锁） */
static void gpio0_single_click_cb(void *button_handle, void *usr_data)
{
    (void)button_handle;
    (void)usr_data;
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        color_test_toggle();
        esp_lv_adapter_unlock();
    }
}
#endif

void app_main()
{
    esp_lcd_panel_handle_t display_panel = NULL;
    esp_lcd_panel_io_handle_t display_io_handle = NULL;
    esp_lv_adapter_rotation_t rotation = get_configured_rotation();
    log_heap_checkpoint("app_main enter");

    /* Select tear effect mode based on LCD interface type */
#if CONFIG_EXAMPLE_LCD_INTERFACE_MIPI_DSI
    esp_lv_adapter_tear_avoid_mode_t tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_DEFAULT_MIPI_DSI;
    ESP_LOGI(TAG, "Selected LCD interface: MIPI DSI");
#elif CONFIG_EXAMPLE_LCD_INTERFACE_RGB
    esp_lv_adapter_tear_avoid_mode_t tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_DEFAULT_RGB;
    ESP_LOGI(TAG, "Selected LCD interface: RGB");
#else
    esp_lv_adapter_tear_avoid_mode_t tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_DEFAULT;
#if CONFIG_EXAMPLE_LCD_INTERFACE_QSPI
    ESP_LOGI(TAG, "Selected LCD interface: QSPI");
#elif CONFIG_EXAMPLE_LCD_INTERFACE_SPI_WITH_PSRAM
    ESP_LOGI(TAG, "Selected LCD interface: SPI (with PSRAM)");
#elif CONFIG_EXAMPLE_LCD_INTERFACE_SPI_WITHOUT_PSRAM
    ESP_LOGI(TAG, "Selected LCD interface: SPI (without PSRAM)");
#endif
#endif

    /* Initialize the LCD hardware panel */
    ESP_LOGI(TAG, "Initializing LCD: %dx%d", HW_LCD_H_RES, HW_LCD_V_RES);
    ESP_ERROR_CHECK(hw_lcd_init(&display_panel, &display_io_handle, tear_avoid_mode, rotation));
    log_heap_checkpoint("after lcd init");

    /* Initialize the LVGL adapter */
    esp_lv_adapter_config_t adapter_config = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    ESP_ERROR_CHECK(esp_lv_adapter_init(&adapter_config));
    log_heap_checkpoint("after lvgl adapter init");

    /* Register the display to the LVGL adapter with appropriate configuration */
#if CONFIG_EXAMPLE_LCD_INTERFACE_MIPI_DSI
    esp_lv_adapter_display_config_t display_config = ESP_LV_ADAPTER_DISPLAY_MIPI_DEFAULT_CONFIG(
                                                         display_panel, display_io_handle, HW_LCD_H_RES, HW_LCD_V_RES, rotation);
#elif CONFIG_EXAMPLE_LCD_INTERFACE_RGB
    esp_lv_adapter_display_config_t display_config = ESP_LV_ADAPTER_DISPLAY_RGB_DEFAULT_CONFIG(
                                                         display_panel, display_io_handle, HW_LCD_H_RES, HW_LCD_V_RES, rotation);
#elif CONFIG_EXAMPLE_LCD_INTERFACE_SPI_WITHOUT_PSRAM
    esp_lv_adapter_display_config_t display_config = ESP_LV_ADAPTER_DISPLAY_SPI_WITHOUT_PSRAM_DEFAULT_CONFIG(
                                                         display_panel, display_io_handle, HW_LCD_H_RES, HW_LCD_V_RES, rotation);
#else  /* QSPI or SPI with PSRAM */
    esp_lv_adapter_display_config_t display_config = ESP_LV_ADAPTER_DISPLAY_SPI_WITH_PSRAM_DEFAULT_CONFIG(
                                                         display_panel, display_io_handle, HW_LCD_H_RES, HW_LCD_V_RES, rotation);
#endif
#if CONFIG_EXAMPLE_LCD_INTERFACE_RGB
    /* Keep the RGB partial draw buffer out of internal RAM. */
    display_config.profile.use_psram = true;
#endif

    lv_display_t *disp = esp_lv_adapter_register_display(&display_config);
    if (disp == NULL) {
        ESP_LOGE(TAG, "Failed to register display");
        return;
    }
    log_heap_checkpoint("after display register");

    /* Initialize input device based on interface type */
#if HW_USE_TOUCH
    ESP_LOGI(TAG, "Initializing touch panel");
    esp_lcd_touch_handle_t touch_handle = NULL;
    esp_err_t touch_ret = hw_touch_init(&touch_handle, rotation);
    if (touch_ret == ESP_OK && touch_handle != NULL) {
        /* Use the default config macro for quick setup with 1:1 coordinate scaling */
        esp_lv_adapter_touch_config_t touch_config = ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(disp, touch_handle);
        lv_indev_t *touch = esp_lv_adapter_register_touch(&touch_config);
        if (touch == NULL) {
            ESP_LOGW(TAG, "Touch disabled: failed to register touch input");
        }
    } else {
        ESP_LOGW(TAG, "Touch disabled: %s", esp_err_to_name(touch_ret));
    }

#elif HW_USE_ENCODER && CONFIG_ESP_LVGL_ADAPTER_ENABLE_KNOB
    ESP_LOGI(TAG, "Initializing encoder/knob");
    esp_lv_adapter_encoder_config_t encoder_config = {
        .disp = disp,
        .encoder_a_b = hw_knob_get_config(),
        .encoder_enter = hw_knob_get_button(),
    };
    lv_indev_t *encoder = esp_lv_adapter_register_encoder(&encoder_config);
    if (encoder == NULL) {
        ESP_LOGE(TAG, "Failed to register encoder");
        return;
    }
#endif

    log_heap_checkpoint("after input init");

    /* Start the LVGL adapter */
    ESP_ERROR_CHECK(esp_lv_adapter_start());
    log_heap_checkpoint("after lvgl adapter start");
    esp_err_t assets_ret = esp_lv_adapter_lock(-1);
    if (assets_ret == ESP_OK) {
        assets_ret = app_assets_init();
        esp_lv_adapter_unlock();
    }
    ESP_ERROR_CHECK(assets_ret);
    log_heap_checkpoint("after assets init");

    /* BLE remote: scan and connect to 68:C4:92:10:A3:66 */
    esp_err_t remote_ret = remote_hid_start(disp);
    if (remote_ret != ESP_OK) {
        ESP_LOGW(TAG, "Remote control disabled: %s", esp_err_to_name(remote_ret));
    }
    log_heap_checkpoint("after remote start");

    /* Optional: Enable FPS statistics for performance monitoring */
#if CONFIG_ESP_LVGL_ADAPTER_ENABLE_FPS_STATS
    ESP_ERROR_CHECK(esp_lv_adapter_fps_stats_enable(disp, true));
    xTaskCreate(fps_monitor_task, "fps_monitor", FPS_MONITOR_TASK_STACK_SIZE, disp, FPS_MONITOR_TASK_PRIORITY, NULL);
#endif

    start_memory_monitor();
    log_heap_checkpoint("after memory monitor start");

    ESP_LOGI(TAG, "Starting shooting training app");
    /* Lock the mutex due to the LVGL APIs are not thread-safe */
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        shooting_app_start(disp);
#if APP_ENABLE_COLOR_TEST
        color_test_init(lv_screen_active());
#endif
        log_heap_checkpoint("after shooting UI");
        esp_lv_adapter_unlock();
    }

#if APP_ENABLE_COLOR_TEST
    /* GPIO0：乐鑫 espressif/button，单击在 Widgets 与纯色测试界面之间切换 */
    const button_config_t btn_cfg = {0};
    const button_gpio_config_t btn_gpio_cfg = {
        .gpio_num = GPIO0_KEY_GPIO,
        .active_level = 0,
        .enable_power_save = false,
        .disable_pull = false,
    };
    button_handle_t gpio0_btn = NULL;
    ESP_ERROR_CHECK(iot_button_new_gpio_device(&btn_cfg, &btn_gpio_cfg, &gpio0_btn));
    ESP_ERROR_CHECK(iot_button_register_cb(gpio0_btn, BUTTON_SINGLE_CLICK, NULL, gpio0_single_click_cb, NULL));
#endif
}
