#include "remote_hid.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_bt.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_hid_gap.h"
#include "esp_hidh.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "nvs_flash.h"

#if CONFIG_BT_BLE_ENABLED && !CONFIG_BT_NIMBLE_ENABLED
#include "esp_bt_device.h"
#include "esp_gap_ble_api.h"
#include "esp_gatt_defs.h"
#include "esp_gattc_api.h"
#endif

static const char *TAG = "remote_hid";

#define REMOTE_SCAN_SECONDS          5
#define REMOTE_RESCAN_DELAY_MS       3000
#define REMOTE_KEY_QUEUE_LEN         16
#define REMOTE_SCAN_TASK_STACK_SIZE  (4 * 1024)
#define LOOKBON_GATTC_APP_ID         0
#define LOOKBON_VENDOR_UUID_BASE     0xae00
#define LOOKBON_VENDOR_UUID_MASK     0xfe00
#define LOOKBON_MAX_SERVICES         6
#define LOOKBON_MAX_NOTIFY_CHARS     8
#define INVALID_HANDLE               0

#if CONFIG_BT_BLE_ENABLED && !CONFIG_BT_NIMBLE_ENABLED
typedef struct {
    uint16_t uuid16;
    uint16_t start_handle;
    uint16_t end_handle;
} lookbon_service_t;

typedef struct {
    uint16_t handle;
    uint16_t service_start_handle;
    uint16_t service_end_handle;
    esp_gatt_char_prop_t properties;
    esp_bt_uuid_t uuid;
} lookbon_notify_char_t;
#endif

static const uint8_t s_remote_bda[6] = {0x68, 0xC4, 0x92, 0x10, 0xA3, 0x66};

static QueueHandle_t s_key_queue;
static lv_indev_t *s_keypad_indev;
static lv_group_t *s_keypad_group;
static uint32_t s_last_key = LV_KEY_ENTER;
static bool s_connected;
static bool s_connecting;

#if CONFIG_BT_BLE_ENABLED && !CONFIG_BT_NIMBLE_ENABLED
static esp_gatt_if_t s_gattc_if = ESP_GATT_IF_NONE;
static uint16_t s_conn_id = 0xffff;
static esp_bd_addr_t s_connected_bda;
static lookbon_service_t s_services[LOOKBON_MAX_SERVICES];
static uint8_t s_service_count;
static lookbon_notify_char_t s_notify_chars[LOOKBON_MAX_NOTIFY_CHARS];
static uint8_t s_notify_char_count;
#endif

static void bda_to_str(const uint8_t bda[6], char out[18])
{
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x",
             bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
}

static bool remote_bda_matches(const uint8_t bda[6])
{
    return memcmp(bda, s_remote_bda, sizeof(s_remote_bda)) == 0;
}

static void keypad_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;

    uint32_t key;
    if (s_key_queue && xQueueReceive(s_key_queue, &key, 0) == pdTRUE) {
        s_last_key = key;
        data->key = key;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->key = s_last_key;
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

static void register_lvgl_keypad(lv_display_t *disp)
{
    (void)disp;

    if (s_keypad_indev) {
        return;
    }

    if (esp_lv_adapter_lock(-1) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to lock LVGL for keypad registration");
        return;
    }

    s_keypad_group = lv_group_create();
    lv_group_set_default(s_keypad_group);

    s_keypad_indev = lv_indev_create();
    lv_indev_set_type(s_keypad_indev, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(s_keypad_indev, keypad_read_cb);
    lv_indev_set_display(s_keypad_indev, disp);
    lv_indev_set_group(s_keypad_indev, s_keypad_group);

    esp_lv_adapter_unlock();
}

static const char *lv_key_name(uint32_t key)
{
    switch (key) {
    case LV_KEY_UP:
        return "UP";
    case LV_KEY_DOWN:
        return "DOWN";
    case LV_KEY_LEFT:
        return "LEFT";
    case LV_KEY_RIGHT:
        return "RIGHT";
    case LV_KEY_ENTER:
        return "ENTER";
    case LV_KEY_ESC:
        return "ESC";
    case LV_KEY_NEXT:
        return "NEXT";
    case LV_KEY_PREV:
        return "PREV";
    default:
        return "?";
    case REMOTE_KEY_LOOKBON_JOY_UP:
        return "LOOKBON_JOY_UP";
    case REMOTE_KEY_LOOKBON_JOY_DOWN:
        return "LOOKBON_JOY_DOWN";
    case REMOTE_KEY_LOOKBON_JOY_LEFT:
        return "LOOKBON_JOY_LEFT";
    case REMOTE_KEY_LOOKBON_JOY_RIGHT:
        return "LOOKBON_JOY_RIGHT";
    case REMOTE_KEY_LOOKBON_BTN_O:
        return "LOOKBON_O";
    case REMOTE_KEY_LOOKBON_BTN_A:
        return "LOOKBON_A";
    case REMOTE_KEY_LOOKBON_BTN_B:
        return "LOOKBON_B";
    case REMOTE_KEY_LOOKBON_BTN_C:
        return "LOOKBON_C";
    case REMOTE_KEY_LOOKBON_BTN_D:
        return "LOOKBON_D";
    case REMOTE_KEY_LOOKBON_BTN_BACK:
        return "LOOKBON_BACK";
    }
}

static void queue_lvgl_key(uint32_t key)
{
    if (!s_key_queue) {
        return;
    }

    if (xQueueSend(s_key_queue, &key, 0) != pdTRUE) {
        uint32_t dropped = 0;
        (void)xQueueReceive(s_key_queue, &dropped, 0);
        (void)xQueueSend(s_key_queue, &key, 0);
    }

    ESP_LOGD(TAG, "Remote key -> LVGL %s", lv_key_name(key));
}

static bool lookbon_key_from_code(uint8_t code, uint32_t *key)
{
    /*
     * LOOKBON single-byte states:
     * joystick: D1 up, D2 down, D3 left, D4 right, D0 neutral
     * buttons: O=A1, A=A2, B=A3, C=A4, D=A5, back=A7
     * Button meaning is scene-specific, so keep the raw LOOKBON identity here.
     */
    if (code == 0x00 || code == 0xd0) {
        return false;
    }

    switch (code) {
    case 0xd1:
        *key = REMOTE_KEY_LOOKBON_JOY_UP;
        return true;
    case 0xd2:
        *key = REMOTE_KEY_LOOKBON_JOY_DOWN;
        return true;
    case 0xd3:
        *key = REMOTE_KEY_LOOKBON_JOY_LEFT;
        return true;
    case 0xd4:
        *key = REMOTE_KEY_LOOKBON_JOY_RIGHT;
        return true;
    case 0xa1:
        *key = REMOTE_KEY_LOOKBON_BTN_O;
        return true;
    case 0xa2:
        *key = REMOTE_KEY_LOOKBON_BTN_A;
        return true;
    case 0xa3:
        *key = REMOTE_KEY_LOOKBON_BTN_B;
        return true;
    case 0xa4:
        *key = REMOTE_KEY_LOOKBON_BTN_C;
        return true;
    case 0xa5:
        *key = REMOTE_KEY_LOOKBON_BTN_D;
        return true;
    case 0xa7:
        *key = REMOTE_KEY_LOOKBON_BTN_BACK;
        return true;
    }

    return false;
}

static bool key_from_usage(uint8_t usage, uint32_t *key)
{
    if (lookbon_key_from_code(usage, key)) {
        return true;
    }

    switch (usage) {
    case 0x52: /* HID keyboard up arrow */
    case LV_KEY_UP:
        *key = LV_KEY_UP;
        return true;
    case 0x51: /* HID keyboard down arrow */
    case LV_KEY_DOWN:
        *key = LV_KEY_DOWN;
        return true;
    case 0x50: /* HID keyboard left arrow */
    case LV_KEY_LEFT:
        *key = LV_KEY_LEFT;
        return true;
    case 0x4f: /* HID keyboard right arrow */
    case LV_KEY_RIGHT:
        *key = LV_KEY_RIGHT;
        return true;
    case 0x28: /* HID keyboard enter */
    case 0x0d:
    case LV_KEY_ENTER:
        *key = LV_KEY_ENTER;
        return true;
    case 0x29: /* HID keyboard escape/back */
    case LV_KEY_ESC:
        *key = LV_KEY_ESC;
        return true;
    case 0x2e: /* HID keyboard =/+ */
    case 0x4e: /* HID keyboard page down */
    case 0xe9: /* Consumer volume up */
    case LV_KEY_NEXT:
        *key = LV_KEY_NEXT;
        return true;
    case 0x2d: /* HID keyboard -/_ */
    case 0x4b: /* HID keyboard page up */
    case 0xea: /* Consumer volume down */
    case LV_KEY_PREV:
        *key = LV_KEY_PREV;
        return true;
    default:
        return false;
    }
}

static bool decode_remote_key(const uint8_t *data, uint16_t len, uint32_t *key)
{
    for (uint16_t i = 0; i < len; i++) {
        if (data[i] != 0) {
            if (key_from_usage(data[i], key)) {
                return true;
            }
        }
    }
    return false;
}

#if CONFIG_BT_BLE_ENABLED && !CONFIG_BT_NIMBLE_ENABLED
static void uuid_to_str(const esp_bt_uuid_t *uuid, char *out, size_t out_len)
{
    if (!uuid || !out || out_len == 0) {
        return;
    }

    switch (uuid->len) {
    case ESP_UUID_LEN_16:
        snprintf(out, out_len, "0x%04x", uuid->uuid.uuid16);
        break;
    case ESP_UUID_LEN_32:
        snprintf(out, out_len, "0x%08" PRIx32, uuid->uuid.uuid32);
        break;
    case ESP_UUID_LEN_128:
        snprintf(out, out_len,
                 "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                 uuid->uuid.uuid128[15], uuid->uuid.uuid128[14],
                 uuid->uuid.uuid128[13], uuid->uuid.uuid128[12],
                 uuid->uuid.uuid128[11], uuid->uuid.uuid128[10],
                 uuid->uuid.uuid128[9], uuid->uuid.uuid128[8],
                 uuid->uuid.uuid128[7], uuid->uuid.uuid128[6],
                 uuid->uuid.uuid128[5], uuid->uuid.uuid128[4],
                 uuid->uuid.uuid128[3], uuid->uuid.uuid128[2],
                 uuid->uuid.uuid128[1], uuid->uuid.uuid128[0]);
        break;
    default:
        snprintf(out, out_len, "len:%u", uuid->len);
        break;
    }
}

static void reset_lookbon_gatt_state(void)
{
    s_conn_id = 0xffff;
    s_service_count = 0;
    s_notify_char_count = 0;
    memset(s_connected_bda, 0, sizeof(s_connected_bda));
    memset(s_services, 0, sizeof(s_services));
    memset(s_notify_chars, 0, sizeof(s_notify_chars));
}

static bool is_lookbon_vendor_uuid(uint16_t uuid16)
{
    return (uuid16 & LOOKBON_VENDOR_UUID_MASK) == LOOKBON_VENDOR_UUID_BASE;
}

static void remember_lookbon_service(uint16_t uuid16, uint16_t start_handle, uint16_t end_handle)
{
    for (uint8_t i = 0; i < s_service_count; i++) {
        if (s_services[i].start_handle == start_handle) {
            return;
        }
    }

    if (s_service_count >= LOOKBON_MAX_SERVICES) {
        ESP_LOGW(TAG, "Too many LOOKBON services, ignoring uuid:0x%04x", uuid16);
        return;
    }

    s_services[s_service_count].uuid16 = uuid16;
    s_services[s_service_count].start_handle = start_handle;
    s_services[s_service_count].end_handle = end_handle;
    s_service_count++;
}

static lookbon_notify_char_t *find_notify_char(uint16_t handle)
{
    for (uint8_t i = 0; i < s_notify_char_count; i++) {
        if (s_notify_chars[i].handle == handle) {
            return &s_notify_chars[i];
        }
    }
    return NULL;
}

static void remember_notify_char(const lookbon_service_t *service, const esp_gattc_char_elem_t *char_elem)
{
    if (find_notify_char(char_elem->char_handle)) {
        return;
    }
    if (s_notify_char_count >= LOOKBON_MAX_NOTIFY_CHARS) {
        ESP_LOGW(TAG, "Too many notify chars, ignoring handle %u", char_elem->char_handle);
        return;
    }

    s_notify_chars[s_notify_char_count].handle = char_elem->char_handle;
    s_notify_chars[s_notify_char_count].service_start_handle = service->start_handle;
    s_notify_chars[s_notify_char_count].service_end_handle = service->end_handle;
    s_notify_chars[s_notify_char_count].properties = char_elem->properties;
    s_notify_chars[s_notify_char_count].uuid = char_elem->uuid;
    s_notify_char_count++;
}

static void enable_notify_for_handle(esp_gatt_if_t gattc_if, uint16_t char_handle)
{
    lookbon_notify_char_t *notify_char = find_notify_char(char_handle);
    if (!notify_char) {
        ESP_LOGW(TAG, "Notify registration returned unknown handle %u", char_handle);
        return;
    }

    uint16_t count = 0;
    esp_gatt_status_t status = esp_ble_gattc_get_attr_count(gattc_if,
                                                            s_conn_id,
                                                            ESP_GATT_DB_DESCRIPTOR,
                                                            notify_char->service_start_handle,
                                                            notify_char->service_end_handle,
                                                            char_handle,
                                                            &count);
    if (status != ESP_GATT_OK || count == 0) {
        ESP_LOGW(TAG, "No descriptor found for handle %u status:0x%x count:%u", char_handle, status, count);
        return;
    }

    esp_gattc_descr_elem_t *descrs = calloc(count, sizeof(esp_gattc_descr_elem_t));
    if (!descrs) {
        ESP_LOGE(TAG, "No memory for descriptors");
        return;
    }

    uint16_t descr_count = count;
    status = esp_ble_gattc_get_all_descr(gattc_if, s_conn_id, char_handle, descrs, &descr_count, 0);
    if (status != ESP_GATT_OK) {
        ESP_LOGE(TAG, "get_all_descr failed status:0x%x", status);
        free(descrs);
        return;
    }

    for (uint16_t i = 0; i < descr_count; i++) {
        char uuid_str[40] = {0};
        uuid_to_str(&descrs[i].uuid, uuid_str, sizeof(uuid_str));
        ESP_LOGI(TAG, "LOOKBON descr handle:%u uuid:%s", descrs[i].handle, uuid_str);

        if (descrs[i].uuid.len == ESP_UUID_LEN_16 &&
            descrs[i].uuid.uuid.uuid16 == ESP_GATT_UUID_CHAR_CLIENT_CONFIG) {
            uint8_t cccd[2] = {0x01, 0x00};
            if (!(notify_char->properties & ESP_GATT_CHAR_PROP_BIT_NOTIFY) &&
                (notify_char->properties & ESP_GATT_CHAR_PROP_BIT_INDICATE)) {
                cccd[0] = 0x02;
            }

            esp_err_t ret = esp_ble_gattc_write_char_descr(gattc_if,
                                                           s_conn_id,
                                                           descrs[i].handle,
                                                           sizeof(cccd),
                                                           cccd,
                                                           ESP_GATT_WRITE_TYPE_RSP,
                                                           ESP_GATT_AUTH_REQ_NONE);
            ESP_LOGI(TAG, "Enable LOOKBON %s on char:%u descr:%u ret:%s",
                     cccd[0] == 0x01 ? "notify" : "indicate",
                     char_handle,
                     descrs[i].handle,
                     esp_err_to_name(ret));
            break;
        }
    }

    free(descrs);
}

static void enumerate_lookbon_service_chars(esp_gatt_if_t gattc_if, const lookbon_service_t *service)
{
    uint16_t count = 0;
    esp_gatt_status_t status = esp_ble_gattc_get_attr_count(gattc_if,
                                                            s_conn_id,
                                                            ESP_GATT_DB_CHARACTERISTIC,
                                                            service->start_handle,
                                                            service->end_handle,
                                                            INVALID_HANDLE,
                                                            &count);
    if (status != ESP_GATT_OK || count == 0) {
        ESP_LOGW(TAG, "No LOOKBON service 0x%04x characteristics status:0x%x count:%u",
                 service->uuid16,
                 status,
                 count);
        return;
    }

    esp_gattc_char_elem_t *chars = calloc(count, sizeof(esp_gattc_char_elem_t));
    if (!chars) {
        ESP_LOGE(TAG, "No memory for characteristics");
        return;
    }

    uint16_t char_count = count;
    status = esp_ble_gattc_get_all_char(gattc_if,
                                        s_conn_id,
                                        service->start_handle,
                                        service->end_handle,
                                        chars,
                                        &char_count,
                                        0);
    if (status != ESP_GATT_OK) {
        ESP_LOGE(TAG, "get_all_char failed status:0x%x", status);
        free(chars);
        return;
    }

    for (uint16_t i = 0; i < char_count; i++) {
        char uuid_str[40] = {0};
        uuid_to_str(&chars[i].uuid, uuid_str, sizeof(uuid_str));
        ESP_LOGI(TAG, "LOOKBON service:0x%04x char handle:%u uuid:%s props:0x%02x%s%s",
                 service->uuid16,
                 chars[i].char_handle,
                 uuid_str,
                 chars[i].properties,
                 (chars[i].properties & ESP_GATT_CHAR_PROP_BIT_NOTIFY) ? " notify" : "",
                 (chars[i].properties & ESP_GATT_CHAR_PROP_BIT_INDICATE) ? " indicate" : "");

        if (chars[i].properties & (ESP_GATT_CHAR_PROP_BIT_NOTIFY | ESP_GATT_CHAR_PROP_BIT_INDICATE)) {
            remember_notify_char(service, &chars[i]);
            esp_err_t ret = esp_ble_gattc_register_for_notify(gattc_if,
                                                              s_connected_bda,
                                                              chars[i].char_handle);
            ESP_LOGI(TAG, "Register notify char:%u ret:%s", chars[i].char_handle, esp_err_to_name(ret));
        }
    }

    free(chars);
}

static void enumerate_lookbon_chars(esp_gatt_if_t gattc_if)
{
    if (s_service_count == 0) {
        ESP_LOGE(TAG, "No LOOKBON vendor services found after GATT search");
        return;
    }

    for (uint8_t i = 0; i < s_service_count; i++) {
        enumerate_lookbon_service_chars(gattc_if, &s_services[i]);
    }

    if (s_notify_char_count == 0) {
        ESP_LOGW(TAG, "No notify/indicate characteristic found. Pressing keys may require polling a readable char.");
    }
}

static void handle_lookbon_notify(uint16_t handle, const uint8_t *data, uint16_t len, bool is_notify)
{
    ESP_LOGD(TAG, "LOOKBON RX handle:%u len:%u %s", handle, len, is_notify ? "notify" : "indicate");

    uint32_t key = 0;
    if (decode_remote_key(data, len, &key)) {
        queue_lvgl_key(key);
    }
}

static void lookbon_gattc_event_handler(esp_gattc_cb_event_t event,
                                        esp_gatt_if_t gattc_if,
                                        esp_ble_gattc_cb_param_t *param)
{
    if (event == ESP_GATTC_REG_EVT) {
        ESP_LOGI(TAG, "LOOKBON GATTC registered status:%d app_id:%d if:%d",
                 param->reg.status,
                 param->reg.app_id,
                 gattc_if);
        if (param->reg.status == ESP_GATT_OK) {
            s_gattc_if = gattc_if;
        }
        return;
    }

    if (gattc_if != ESP_GATT_IF_NONE && gattc_if != s_gattc_if) {
        return;
    }

    switch (event) {
    case ESP_GATTC_CONNECT_EVT:
        s_conn_id = param->connect.conn_id;
        memcpy(s_connected_bda, param->connect.remote_bda, sizeof(s_connected_bda));
        ESP_LOGI(TAG, "LOOKBON connected conn_id:%u remote:" ESP_BD_ADDR_STR,
                 s_conn_id,
                 ESP_BD_ADDR_HEX(param->connect.remote_bda));
        esp_ble_gattc_send_mtu_req(gattc_if, s_conn_id);
        break;

    case ESP_GATTC_OPEN_EVT:
        if (param->open.status != ESP_GATT_OK) {
            s_connected = false;
            s_connecting = false;
            ESP_LOGE(TAG, "LOOKBON open failed status:0x%x", param->open.status);
            break;
        }
        s_connected = true;
        s_connecting = false;
        s_conn_id = param->open.conn_id;
        memcpy(s_connected_bda, param->open.remote_bda, sizeof(s_connected_bda));
        ESP_LOGI(TAG, "LOOKBON open ok conn_id:%u mtu:%u", s_conn_id, param->open.mtu);
        break;

    case ESP_GATTC_CFG_MTU_EVT:
        ESP_LOGI(TAG, "LOOKBON MTU status:0x%x mtu:%u", param->cfg_mtu.status, param->cfg_mtu.mtu);
        break;

    case ESP_GATTC_DIS_SRVC_CMPL_EVT:
        if (param->dis_srvc_cmpl.status != ESP_GATT_OK) {
            ESP_LOGE(TAG, "LOOKBON service discovery failed status:0x%x", param->dis_srvc_cmpl.status);
            break;
        }
        ESP_LOGI(TAG, "LOOKBON service discovery complete, searching services");
        esp_ble_gattc_search_service(gattc_if, param->dis_srvc_cmpl.conn_id, NULL);
        break;

    case ESP_GATTC_SEARCH_RES_EVT: {
        char uuid_str[40] = {0};
        uuid_to_str(&param->search_res.srvc_id.uuid, uuid_str, sizeof(uuid_str));
        ESP_LOGI(TAG, "LOOKBON service uuid:%s start:%u end:%u primary:%d",
                 uuid_str,
                 param->search_res.start_handle,
                 param->search_res.end_handle,
                 param->search_res.is_primary);

        if (param->search_res.srvc_id.uuid.len == ESP_UUID_LEN_16 &&
            is_lookbon_vendor_uuid(param->search_res.srvc_id.uuid.uuid.uuid16)) {
            remember_lookbon_service(param->search_res.srvc_id.uuid.uuid.uuid16,
                                     param->search_res.start_handle,
                                     param->search_res.end_handle);
        }
        break;
    }

    case ESP_GATTC_SEARCH_CMPL_EVT:
        if (param->search_cmpl.status != ESP_GATT_OK) {
            ESP_LOGE(TAG, "LOOKBON service search failed status:0x%x", param->search_cmpl.status);
            break;
        }
        ESP_LOGI(TAG, "LOOKBON service search complete");
        enumerate_lookbon_chars(gattc_if);
        break;

    case ESP_GATTC_REG_FOR_NOTIFY_EVT:
        if (param->reg_for_notify.status != ESP_GATT_OK) {
            ESP_LOGE(TAG, "LOOKBON register notify failed handle:%u status:0x%x",
                     param->reg_for_notify.handle,
                     param->reg_for_notify.status);
            break;
        }
        ESP_LOGI(TAG, "LOOKBON register notify ok handle:%u", param->reg_for_notify.handle);
        enable_notify_for_handle(gattc_if, param->reg_for_notify.handle);
        break;

    case ESP_GATTC_WRITE_DESCR_EVT:
        ESP_LOGI(TAG, "LOOKBON descriptor write handle:%u status:0x%x",
                 param->write.handle,
                 param->write.status);
        break;

    case ESP_GATTC_NOTIFY_EVT:
        handle_lookbon_notify(param->notify.handle,
                              param->notify.value,
                              param->notify.value_len,
                              param->notify.is_notify);
        break;

    case ESP_GATTC_DISCONNECT_EVT:
        ESP_LOGI(TAG, "LOOKBON disconnected reason:0x%x", param->disconnect.reason);
        s_connected = false;
        s_connecting = false;
        reset_lookbon_gatt_state();
        break;

    case ESP_GATTC_CLOSE_EVT:
        ESP_LOGI(TAG, "LOOKBON close status:0x%x reason:0x%x", param->close.status, param->close.reason);
        s_connected = false;
        s_connecting = false;
        reset_lookbon_gatt_state();
        break;

    default:
        break;
    }
}
#endif

static esp_err_t init_nvs(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    return ret;
}

static void scan_task(void *arg)
{
    (void)arg;

    char target_bda[18] = {0};
    bda_to_str(s_remote_bda, target_bda);

    while (1) {
#if CONFIG_BT_BLE_ENABLED && !CONFIG_BT_NIMBLE_ENABLED
        if (s_gattc_if == ESP_GATT_IF_NONE) {
            ESP_LOGI(TAG, "Waiting for LOOKBON GATTC registration");
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
#endif

        if (s_connected || s_connecting) {
            vTaskDelay(pdMS_TO_TICKS(REMOTE_RESCAN_DELAY_MS));
            continue;
        }

        size_t results_len = 0;
        esp_hid_scan_result_t *results = NULL;
        ESP_LOGI(TAG, "Scanning for remote %s", target_bda);

        esp_err_t ret = esp_hid_scan(REMOTE_SCAN_SECONDS, &results_len, &results);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Scan failed: %s", esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(REMOTE_RESCAN_DELAY_MS));
            continue;
        }

        esp_hid_scan_result_t *target = NULL;
        for (esp_hid_scan_result_t *r = results; r; r = r->next) {
            char bda_str[18] = {0};
            bda_to_str(r->bda, bda_str);
            ESP_LOGI(TAG, "Found %s %s RSSI:%d usage:%s name:%s",
                     r->transport == ESP_HID_TRANSPORT_BLE ? "BLE" : "BT",
                     bda_str,
                     r->rssi,
                     esp_hid_usage_str(r->usage),
                     r->name ? r->name : "");

            if (r->transport == ESP_HID_TRANSPORT_BLE && remote_bda_matches(r->bda)) {
                target = r;
                break;
            }
        }

        if (target) {
            ESP_LOGI(TAG, "Opening LOOKBON GATT remote %s", target_bda);
            s_connecting = true;
            reset_lookbon_gatt_state();
            esp_err_t open_ret = esp_ble_gattc_open(s_gattc_if,
                                                     target->bda,
                                                     target->ble.addr_type,
                                                     true);
            if (open_ret != ESP_OK) {
                s_connecting = false;
                ESP_LOGE(TAG, "LOOKBON GATT open start failed: %s", esp_err_to_name(open_ret));
            }
        } else {
            ESP_LOGW(TAG, "Remote %s not found. Keep it awake and press a key while scanning.", target_bda);
        }

        if (results) {
            esp_hid_scan_results_free(results);
        }

        vTaskDelay(pdMS_TO_TICKS(REMOTE_RESCAN_DELAY_MS));
    }
}

esp_err_t remote_hid_start(lv_display_t *disp)
{
#if !CONFIG_BT_BLE_ENABLED
    (void)disp;
    ESP_LOGE(TAG, "Bluetooth BLE is disabled. Enable CONFIG_BT_ENABLED and CONFIG_BT_BLE_ENABLED.");
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (!s_key_queue) {
        s_key_queue = xQueueCreate(REMOTE_KEY_QUEUE_LEN, sizeof(uint32_t));
        if (!s_key_queue) {
            return ESP_ERR_NO_MEM;
        }
    }

    register_lvgl_keypad(disp);

    ESP_RETURN_ON_ERROR(init_nvs(), TAG, "NVS init failed");
    ESP_RETURN_ON_ERROR(esp_hid_gap_init(HIDH_BLE_MODE), TAG, "HID GAP init failed");

#if !CONFIG_BT_NIMBLE_ENABLED
    ESP_RETURN_ON_ERROR(esp_ble_gattc_register_callback(lookbon_gattc_event_handler),
                        TAG, "GATTC callback registration failed");
    ESP_RETURN_ON_ERROR(esp_ble_gattc_app_register(LOOKBON_GATTC_APP_ID),
                        TAG, "GATTC app registration failed");
    const uint8_t *own_bda = esp_bt_dev_get_address();
    if (own_bda) {
        char own_bda_str[18] = {0};
        bda_to_str(own_bda, own_bda_str);
        ESP_LOGI(TAG, "Own BLE address: %s", own_bda_str);
    }
#endif

    BaseType_t task_ok = xTaskCreate(scan_task, "remote_hid",
                                     REMOTE_SCAN_TASK_STACK_SIZE,
                                     NULL, 3, NULL);
    if (task_ok != pdPASS) {
        ESP_LOGE(TAG, "Create remote_hid task failed, internal free:%u psram free:%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
#endif
}
