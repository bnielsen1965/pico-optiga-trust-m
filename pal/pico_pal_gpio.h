/*
 * pico_pal_gpio.h
 *
 * Pico-specific PAL GPIO context shared by pal_gpio.c and
 * pal_ifx_i2c_config.c. The Infineon PAL passes GPIO contexts as an
 * opaque `void *p_gpio_hw`; on the Pico that is just a GPIO number.
 */
#ifndef PICO_PAL_GPIO_H
#define PICO_PAL_GPIO_H

#include <stdint.h>

/** Pico PAL GPIO context: a GPIO number. */
typedef struct pico_gpio_ctx {
    uint gpio;
} pico_gpio_ctx_t;

#endif /* PICO_PAL_GPIO_H */
