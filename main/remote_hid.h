#pragma once

#include "esp_err.h"
#include "lvgl.h"

typedef enum {
    REMOTE_KEY_LOOKBON_JOY_UP = 0x1001,
    REMOTE_KEY_LOOKBON_JOY_DOWN,
    REMOTE_KEY_LOOKBON_JOY_LEFT,
    REMOTE_KEY_LOOKBON_JOY_RIGHT,
    REMOTE_KEY_LOOKBON_BTN_O,
    REMOTE_KEY_LOOKBON_BTN_A,
    REMOTE_KEY_LOOKBON_BTN_B,
    REMOTE_KEY_LOOKBON_BTN_C,
    REMOTE_KEY_LOOKBON_BTN_D,
    REMOTE_KEY_LOOKBON_BTN_BACK,
} remote_key_t;

esp_err_t remote_hid_start(lv_display_t *disp);

