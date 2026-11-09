/*
 * pal_os_timer.c
 *
 * Pico port of the Infineon NEW_PAL_TEMPLATE pal_os_timer.c.
 *
 * Real-time from the Pico SDK's 64-bit µs timer. The microsecond
 * counter must return a unique value per invocation (used by the
 * optiga command scheduler); to_us_since_boot() satisfies that.
 */

#include "pico/stdlib.h"
#include "pico/time.h"
#include "pal_os_timer.h"

uint32_t pal_os_timer_get_time_in_microseconds(void) {
    return (uint32_t)to_us_since_boot(get_absolute_time());
}

uint32_t pal_os_timer_get_time_in_milliseconds(void) {
    return (uint32_t)to_ms_since_boot(get_absolute_time());
}

void pal_os_timer_delay_in_milliseconds(uint16_t milliseconds) {
    sleep_ms(milliseconds);
}

pal_status_t pal_timer_init(void) {
    return PAL_STATUS_SUCCESS;
}

pal_status_t pal_timer_deinit(void) {
    return PAL_STATUS_SUCCESS;
}
