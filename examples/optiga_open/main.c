/*
 * optiga_open — Phase 2 exit test
 *
 * Drives the full Infineon OPTIGA Trust M host library through the Pico
 * PAL: creates an optiga_util instance, opens the application on the
 * chip, closes it again.
 *
 * What happens inside optiga_util_open_application():
 *   1. the IFX stack's own reset state machine runs on GP22
 *      (RST low 2 s, high, 12 s startup — vendor constants,
 *      ifx_i2c_config.h);
 *   2. frame size and bitrate are negotiated over I2C (starting at
 *      100 kHz);
 *   3. the application is opened on the chip.
 *
 * The stack is event-driven. On this bare-metal port the application
 * pumps the one-shot chain from its wait loop (see pal/pal_os_event.c):
 *
 *     while (OPTIGA_LIB_BUSY == status) {
 *         pal_os_event_trigger_registered_callback();
 *         sleep_ms(1);
 *     }
 *
 * The open completes in well under a second: the reset sequence is a
 * 1 ms RST-low pulse + 25 ms startup (datasheet tSTARTUP = 15 ms, 20 ms
 * if NVM programming was pending; see the PICO PATCH in
 * ifx_i2c_config.h), then the IFX negotiation runs at 100 kHz.
 */

#include <pico/stdlib.h>
#include <stdio.h>

#include "hardware/gpio.h"
#include "optiga_util.h"
#include "pal_os_event.h"

#define TRUSTM_RST_GPIO 22u

static void rst_pin_bringup(void) {
    // Drive high before enabling the driver: the RP2350 GPIO OUT
    // register defaults to 0, so a plain gpio_set_dir(GPIO_OUT) would
    // emit a low spike on RST during boot.
    gpio_init(TRUSTM_RST_GPIO);
    gpio_put(TRUSTM_RST_GPIO, 1);
    gpio_set_dir(TRUSTM_RST_GPIO, GPIO_OUT);
}

static volatile optiga_lib_status_t lib_status;

static void util_callback(void *context, optiga_lib_status_t return_status) {
    (void)context;
    lib_status = return_status;
}

/* Pump the stack's one-shot chain until the completion callback has
 * fired. The ~1 ms cadence matches the stack's own polling interval. */
static void pump_until_complete(void) {
    while (OPTIGA_LIB_BUSY == lib_status) {
        pal_os_event_trigger_registered_callback();
        sleep_ms(1);
    }
}

int main(void) {
    rst_pin_bringup();
    stdio_init_all();

    printf("== OPTIGA Trust M via Infineon host library ==\n");
    printf("Creating optiga_util instance...\n");

    optiga_util_t *util = optiga_util_create(0, util_callback, NULL);
    if (NULL == util) {
        printf("FAIL: optiga_util_create returned NULL\n");
        return 1;
    }

    optiga_lib_status_t status;

    printf("Opening application (1 ms RST + 25 ms startup, then IFX negotiation)...\n");
    lib_status = OPTIGA_LIB_BUSY;
    status = optiga_util_open_application(util, 0);
    if (OPTIGA_LIB_SUCCESS == status) {
        pump_until_complete();
    }
    printf("open_application:  %s (0x%04X)\n",
           (OPTIGA_LIB_SUCCESS == lib_status) ? "OK" : "FAIL", (unsigned)lib_status);

    if (OPTIGA_LIB_SUCCESS == lib_status) {
        printf("Closing application...\n");
        lib_status = OPTIGA_LIB_BUSY;
        status = optiga_util_close_application(util, 0);
        if (OPTIGA_LIB_SUCCESS == status) {
            pump_until_complete();
        }
        printf("close_application: %s (0x%04X)\n",
               (OPTIGA_LIB_SUCCESS == lib_status) ? "OK" : "FAIL", (unsigned)lib_status);
    }

    // lint --e{534} the return value is not checked
    optiga_util_destroy(util);
    printf("done\n");
    return 0;
}
