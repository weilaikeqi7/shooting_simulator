#pragma once

#include "esp_err.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

esp_err_t esp_timer_stop_blocking(esp_timer_handle_t timer, TickType_t ticks_to_wait);