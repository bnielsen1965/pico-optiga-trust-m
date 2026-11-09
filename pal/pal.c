/*
 * pal.c
 *
 * Pico port of the Infineon NEW_PAL_TEMPLATE pal.c.
 *
 * Nothing to initialize at the PAL level: the I2C master is set up in
 * pal_i2c_init() and the RST pin is brought up on first use
 * (pal_gpio.c). The template's no-ops are used unchanged.
 */

#include "pal.h"

pal_status_t pal_init(void) {
    return PAL_STATUS_SUCCESS;
}

pal_status_t pal_deinit(void) {
    return PAL_STATUS_SUCCESS;
}
