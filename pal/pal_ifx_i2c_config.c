/*
 * pal_ifx_i2c_config.c
 *
 * Pico port of the Infineon NEW_PAL_TEMPLATE pal_ifx_i2c_config.c.
 *
 * Wires the IFX I2C protocol stack to the bench hardware:
 *   - I2C0 on GP20 (SDA) / GP21 (SCL) at 100 kHz
 *   - Trust M 7-bit I2C address 0x30
 *   - RESET pin on GP22 (active low)
 *
 * Note on reset type: the vendor's ifx_i2c_context_0 defaults to
 * IFX_I2C_COLD_RESET (0), which toggles both the VDD and the RST pin.
 * optiga_vdd_0 below carries a NULL hardware context because VDD is
 * hardwired to the Pico's 3.3 V (no power control), so the VDD toggle
 * is a no-op and the "cold" reset degrades to a safe warm reset.
 */

#include "ifx_i2c_config.h"
#include "hardware/i2c.h"
#include "pal_gpio.h"
#include "pal_i2c.h"

#include "pico_pal_gpio.h"

static pico_gpio_ctx_t g_reset_pin = {
    .gpio = 22u, /* Trust M RST (active low) */
};

/**
 * \brief PAL I2C configuration for OPTIGA.
 */
pal_i2c_t optiga_pal_i2c_context_0 = {
    .p_i2c_hw_config = (void *)i2c0, /* SDK I2C instance (i2c0 is a pointer macro) */
    .p_upper_layer_ctx = NULL,        /* filled by the stack */
    .upper_layer_event_handler = NULL, /* filled by the stack */
    .slave_address = 0x30,
};

/**
 * \brief PAL vdd pin configuration for OPTIGA.
 *
 * VDD is hardwired to 3.3 V: no power control, NULL context makes the
 * stack's VDD toggles no-ops.
 */
pal_gpio_t optiga_vdd_0 = {
    .p_gpio_hw = NULL,
};

/**
 * \brief PAL reset pin configuration for OPTIGA.
 */
pal_gpio_t optiga_reset_0 = {
    .p_gpio_hw = &g_reset_pin,
};
