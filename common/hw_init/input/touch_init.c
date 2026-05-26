#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdbool.h>
#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "hw_init.h"
#include "input/touch_rotation_helper.h"

#if HW_USE_TOUCH

#if CONFIG_EXAMPLE_LCD_INTERFACE_RGB
#include "esp_lcd_touch_gt911.h"
#define HW_LCD_TOUCH_RST                (GPIO_NUM_38)
#define HW_LCD_TOUCH_INT                (GPIO_NUM_18)
#define HW_I2C_SDA                      (GPIO_NUM_19)
#define HW_I2C_SCL                      (GPIO_NUM_20)
#define TOUCH_CONTROLLER_NAME           "GT911"
#define TOUCH_ROTATION_TYPE             TOUCH_ROTATION_STANDARD
#define TOUCH_CONTROLLER_TYPE_GT911     1

#elif CONFIG_EXAMPLE_LCD_INTERFACE_MIPI_DSI
#include "esp_lcd_touch_gt911.h"
#define HW_LCD_TOUCH_RST                (GPIO_NUM_NC)
#define HW_LCD_TOUCH_INT                (GPIO_NUM_NC)
#define HW_I2C_SDA                      (GPIO_NUM_7)
#define HW_I2C_SCL                      (GPIO_NUM_8)
#define TOUCH_CONTROLLER_NAME           "GT911"
#define TOUCH_ROTATION_TYPE             TOUCH_ROTATION_MIPI_DSI
#define TOUCH_CONTROLLER_TYPE_GT911     1

#elif CONFIG_EXAMPLE_LCD_INTERFACE_QSPI
#include "esp_lcd_touch_cst816s.h"
#define HW_LCD_TOUCH_RST                (GPIO_NUM_NC)
#define HW_LCD_TOUCH_INT                (GPIO_NUM_10)
#define HW_I2C_SDA                      (GPIO_NUM_2)
#define HW_I2C_SCL                      (GPIO_NUM_1)
#define TOUCH_CONTROLLER_NAME           "CST816S"
#define TOUCH_ROTATION_TYPE             TOUCH_ROTATION_STANDARD
#define TOUCH_CONTROLLER_TYPE_CST816S   1

#elif CONFIG_EXAMPLE_LCD_INTERFACE_SPI_WITH_PSRAM
#include "esp_lcd_touch_gt911.h"
#define HW_LCD_TOUCH_RST                (GPIO_NUM_NC)
#define HW_LCD_TOUCH_INT                (GPIO_NUM_3)
#define HW_I2C_SDA                      (GPIO_NUM_8)
#define HW_I2C_SCL                      (GPIO_NUM_18)
#define TOUCH_CONTROLLER_NAME           "GT911"
#define TOUCH_ROTATION_TYPE             TOUCH_ROTATION_STANDARD
#define TOUCH_CONTROLLER_TYPE_GT911     1

#else
#include "esp_lcd_touch_gt911.h"
#define HW_LCD_TOUCH_RST                (GPIO_NUM_NC)
#define HW_LCD_TOUCH_INT                (GPIO_NUM_NC)
#define HW_I2C_SDA                      (GPIO_NUM_7)
#define HW_I2C_SCL                      (GPIO_NUM_8)
#define TOUCH_CONTROLLER_NAME           "GT911"
#define TOUCH_ROTATION_TYPE             TOUCH_ROTATION_STANDARD
#define TOUCH_CONTROLLER_TYPE_GT911     1
#endif

#define HW_I2C_NUM                          (I2C_NUM_0)
#define TOUCH_INIT_RETRY_COUNT              (5)
#define TOUCH_INIT_RETRY_DELAY_MS           (120)
/*
 * 触摸 I2C：400k 在排线/干扰下易 NACK 或总线挂死；esp_lcd 的 I2C panel_io 对传输使用无限超时 (xfer_timeout=-1)，
 * LVGL 任务会长时间卡在 GT911 读上并触发 Task WDT。默认用 100k 更稳；板子布线极短且可靠时可改回 400000。
 */
#define HW_I2C_CLK_SPEED_HZ                 (100000)

static const char *TAG = "hw_touch_init";
static i2c_master_bus_handle_t s_touch_i2c_bus = NULL;

static esp_err_t hw_touch_i2c_init(void)
{
    if (s_touch_i2c_bus) {
        return ESP_OK;
    }

    i2c_master_bus_config_t i2c_bus_conf = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .sda_io_num = HW_I2C_SDA,
        .scl_io_num = HW_I2C_SCL,
        .i2c_port = HW_I2C_NUM,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .flags = {
            .enable_internal_pullup = 1,
        },
    };
    esp_err_t ret = i2c_new_master_bus(&i2c_bus_conf, &s_touch_i2c_bus);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Touch I2C bus init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "Touch I2C bus: %d Hz, glitch_ignore=7, internal pull-up on", HW_I2C_CLK_SPEED_HZ);

    return ESP_OK;
}

esp_err_t hw_touch_init(esp_lcd_touch_handle_t *ret_touch, esp_lv_adapter_rotation_t rotation)
{
    ESP_LOGI(TAG, "Initializing touch controller (%s)", TOUCH_CONTROLLER_NAME);

    esp_err_t ret = hw_touch_i2c_init();
    if (ret != ESP_OK) {
        return ret;
    }

    bool swap_xy = false;
    bool mirror_x = false;
    bool mirror_y = false;
    touch_get_rotation_flags(TOUCH_ROTATION_TYPE, rotation, &swap_xy, &mirror_x, &mirror_y);

    esp_lcd_touch_config_t tp_cfg = {
        .x_max = HW_LCD_H_RES,
        .y_max = HW_LCD_V_RES,
        .rst_gpio_num = HW_LCD_TOUCH_RST,
        .int_gpio_num = HW_LCD_TOUCH_INT,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = swap_xy,
            .mirror_x = mirror_x,
            .mirror_y = mirror_y,
        },
    };

    esp_lcd_panel_io_handle_t tp_io_handle = NULL;

    /* Initialize touch controller based on type */
#if defined(TOUCH_CONTROLLER_TYPE_GT1151)
    esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_GT1151_CONFIG();
    tp_io_config.scl_speed_hz = HW_I2C_CLK_SPEED_HZ;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(s_touch_i2c_bus, &tp_io_config, &tp_io_handle));
    return esp_lcd_touch_new_i2c_gt1151(tp_io_handle, &tp_cfg, ret_touch);

#elif defined(TOUCH_CONTROLLER_TYPE_CST816S)
    esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_CST816S_CONFIG();
    tp_io_config.scl_speed_hz = HW_I2C_CLK_SPEED_HZ;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(s_touch_i2c_bus, &tp_io_config, &tp_io_handle));
    return esp_lcd_touch_new_i2c_cst816s(tp_io_handle, &tp_cfg, ret_touch);

#elif defined(TOUCH_CONTROLLER_TYPE_GT911)
    static esp_lcd_touch_io_gt911_config_t gt911_cfg = {
        .dev_addr = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS,
    };
    tp_cfg.driver_data = &gt911_cfg;

    for (int i = 1; i <= TOUCH_INIT_RETRY_COUNT; i++) {
        esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
        tp_io_config.scl_speed_hz = HW_I2C_CLK_SPEED_HZ;

        ret = esp_lcd_new_panel_io_i2c(s_touch_i2c_bus, &tp_io_config, &tp_io_handle);
        if (ret == ESP_OK) {
            ret = esp_lcd_touch_new_i2c_gt911(tp_io_handle, &tp_cfg, ret_touch);
        }

        if (ret == ESP_OK) {
            /*
             * GPIO18 is needed during GT911 reset to select the I2C address. After
             * that, keep LVGL in polling mode because some boards don't provide a
             * reliable interrupt edge to the adapter.
             */
#if CONFIG_EXAMPLE_LCD_INTERFACE_RGB
            (*ret_touch)->config.int_gpio_num = GPIO_NUM_NC;
            ESP_LOGI(TAG, "GT911 initialized, LVGL touch polling mode enabled");
#endif
            return ESP_OK;
        }

        ESP_LOGW(TAG, "GT911 init attempt %d/%d failed: %s", i, TOUCH_INIT_RETRY_COUNT, esp_err_to_name(ret));
        if (tp_io_handle != NULL) {
            esp_lcd_panel_io_del(tp_io_handle);
            tp_io_handle = NULL;
        }
        vTaskDelay(pdMS_TO_TICKS(TOUCH_INIT_RETRY_DELAY_MS));
    }

    return ret;

#else
#error "No touch controller type defined"
#endif
}

#endif
