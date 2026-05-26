#include "esp_timer_stop_blocking_compat.h"

esp_err_t __attribute__((weak)) esp_timer_stop_blocking(esp_timer_handle_t timer, TickType_t ticks_to_wait)
{
    (void)ticks_to_wait;
    return esp_timer_stop(timer);
}