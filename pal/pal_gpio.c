/*
 * pal_gpio.c
 *
 * Pico port of the Infineon NEW_PAL_TEMPLATE pal_gpio.c.
 *
 * The only pin the stack ever drives on this board is Trust M RST
 * (GP22, active low); optiga_vdd_0 carries a NULL hardware context, so
 * VDD calls are no-ops.
 *
 * Safe bring-up (Phase 1 finding, see PLAN.md): the RP2350 GPIO OUT
 * register defaults to 0, so a plain gpio_set_dir(GPIO_OUT) emits a
 * low spike on the pin. The driver is therefore armed high *before*
 * the output is enabled.
 */

#include "hardware/gpio.h"
#include "pal_gpio.h"

#include "pico_pal_gpio.h"

// The stack may call set_high/low before pal_gpio_init; remember which
// pins we have brought up (the SDK has no gpio_is_init() in 2.x).
static bool g_gpio_brought_up[30];

static void pico_gpio_ensure_output(const pico_gpio_ctx_t *ctx) {
    if (!g_gpio_brought_up[ctx->gpio]) {
        gpio_init(ctx->gpio);
        // Arm high while still an input, then enable the driver: no
        // low spike on RST.
        gpio_put(ctx->gpio, 1);
        gpio_set_dir(ctx->gpio, GPIO_OUT);
        g_gpio_brought_up[ctx->gpio] = true;
    }
}

pal_status_t pal_gpio_init(const pal_gpio_t *p_gpio_context) {
    if ((p_gpio_context != NULL) && (p_gpio_context->p_gpio_hw != NULL)) {
        pico_gpio_ensure_output((const pico_gpio_ctx_t *)p_gpio_context->p_gpio_hw);
    }
    return PAL_STATUS_SUCCESS;
}

pal_status_t pal_gpio_deinit(const pal_gpio_t *p_gpio_context) {
    // Keep the pin as output-high: RST idles high, and a floating RST
    // during boot is what the Phase 1 debugging ruled out.
    (void)p_gpio_context;
    return PAL_STATUS_SUCCESS;
}

void pal_gpio_set_high(const pal_gpio_t *p_gpio_context) {
    if ((p_gpio_context != NULL) && (p_gpio_context->p_gpio_hw != NULL)) {
        const pico_gpio_ctx_t *ctx = (const pico_gpio_ctx_t *)p_gpio_context->p_gpio_hw;
        pico_gpio_ensure_output(ctx);
        gpio_put(ctx->gpio, 1);
    }
}

void pal_gpio_set_low(const pal_gpio_t *p_gpio_context) {
    if ((p_gpio_context != NULL) && (p_gpio_context->p_gpio_hw != NULL)) {
        const pico_gpio_ctx_t *ctx = (const pico_gpio_ctx_t *)p_gpio_context->p_gpio_hw;
        pico_gpio_ensure_output(ctx);
        gpio_put(ctx->gpio, 0);
    }
}
