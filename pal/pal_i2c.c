/*
 * pal_i2c.c
 *
 * Pico port of the Infineon NEW_PAL_TEMPLATE pal_i2c.c.
 *
 * Blocking I2C via the Pico SDK. The IFX I2C protocol is a polling-based
 * protocol: the stack calls pal_i2c_write/read and learns the outcome
 * from the upper-layer event callback, so the blocking result is mapped
 * to an event here (as the template prescribes for blocking drivers).
 *
 * Bench findings from Phase 1 (see PLAN.md, "Facts established in
 * Phase 1") that this file must honor:
 *
 *  - i2c_write_blocking()/i2c_read_blocking() return the byte count on
 *    success (not 0), 0 when the address was ACKed but the data NAKed,
 *    -1 (PICO_ERROR_GENERIC) on address NAK, -3 (PICO_ERROR_TIMEOUT)
 *    on timeout.
 *  - The addr parameter is the 7-bit address UNSHIFTED (the driver
 *    drives the hw target register directly) and the last parameter is
 *    *nostop* (false = issue STOP) in SDK 2.x.
 *  - An address NAK from the Trust M means the chip is busy or in idle
 *    state, not that it is absent. The IFX physical layer treats
 *    PAL_I2C_EVENT_BUSY as "retry after PL_POLLING_INVERVAL_US (1 ms)",
 *    which lands inside the chip's ~5 ms ACK-ready window, so failures
 *    are reported as BUSY and let the stack retry.
 *  - The IFX protocol is STOP-based: guard time is measured from the
 *    STOP condition to the next START, so writes terminate with a STOP.
 */

#include <stdio.h>

#include "hardware/i2c.h"
#include "hardware/gpio.h"
#include "pal_i2c.h"

// --- Bench trace ---------------------------------------------------------
// Optional (PICO_OPTIGA_PAL_TRACE=ON): prints every I2C transaction so
// the IFX negotiation can be followed on the serial console: which
// register is written/read, with what data, and what the Pico SDK driver
// reported (rc >= 0: bytes transferred, 0: addr ACKed/data NAKed,
// < 0: addr NAK/timeout). Off by default: a data-object read is several
// transactions, which is too noisy for normal operation.
#ifdef PICO_OPTIGA_PAL_TRACE
static void trace_i2c(const char *op, const pal_i2c_t *p_i2c_context,
                      const uint8_t *p_data, uint16_t length, int rc) {
    printf("[I2C] %s @0x%02X %uB rc=%d", op, p_i2c_context->slave_address,
           (unsigned)length, rc);
    if (rc > 0) {
        for (uint16_t i = 0; i < length; i++) {
            printf(" %02X", p_data[i]);
        }
    }
    printf("\n");
}
#endif  // PICO_OPTIGA_PAL_TRACE
// --- end bench trace -----------------------------------------------------

#define TRUSTM_I2C             i2c0
#define TRUSTM_I2C_SDA         20u
#define TRUSTM_I2C_SCL         21u
#define TRUSTM_I2C_INIT_HZ     100000u
#define PAL_I2C_MASTER_MAX_BITRATE (400U) /* kHz */

/* Per-character I2C timeout (microseconds).
 *
 * The Pico SDK's i2c_read_blocking()/i2c_write_blocking() pass a NULL
 * timeout check to the driver, so their two busy-wait loops
 * (wait-for-TX-FIFO-space and wait-for-RX-data) are UNBOUNDED: if the
 * Trust M stalls the bus mid-transfer (clock-stretch overrun, a NAK after
 * the address byte, SDA/SCL held) the core spins forever and the whole
 * superloop hangs. We observed exactly this on the 0xE0F3 sign's final
 * DATA-register read.
 *
 * Use the *_timeout_per_char_us() variants so a stalled byte maps to
 * PICO_ERROR_TIMEOUT (rc < 0), which this file already reports as
 * PAL_I2C_EVENT_BUSY and lets the IFX physical layer retry on its 1 ms
 * polling cadence. 10 ms/char is far above the chip's worst-case
 * clock-stretch per byte, so legitimate transfers are unaffected.
 */
#define PAL_I2C_TIMEOUT_PER_CHAR_US (10000U)

static volatile uint32_t g_entry_count = 0;
static pal_i2c_t *gp_pal_i2c_current_ctx;

static pal_status_t pal_i2c_acquire(const void *p_i2c_context) {
    // Single-core, non-preemptive: a simple entry count is sufficient.
    (void)p_i2c_context;

    if (0 == g_entry_count) {
        g_entry_count++;
        if (1 == g_entry_count) {
            return PAL_STATUS_SUCCESS;
        }
    }
    return PAL_STATUS_FAILURE;
}

static void pal_i2c_release(const void *p_i2c_context) {
    (void)p_i2c_context;
    g_entry_count = 0;
}

/* Pre-transfer bus recovery.
 *
 * The Pico SDK's i2c_read_blocking_internal() contains an UNBOUNDED
 *   while (!i2c_get_write_available(i2c)) tight_loop_contents();
 * loop (no timeout_check) that waits for TX-FIFO space. If the I2C core's
 * TX FIFO gets stuck (e.g. after an aborted transfer, a clock-stretch
 * overrun, or a NAK mid-burst) this loop spins forever and the whole
 * superloop hangs. We observed exactly this on the 0xE0F3 sign's final
 * DATA-register read (80-byte read entered, never returned).
 *
 * Before every transfer we check whether the TX FIFO is drained. If it is
 * stuck (no write available), we re-initialize the I2C core and re-pin the
 * GPIOs so the next transfer starts from a clean state. In the normal case
 * the TX FIFO is empty and this is a single register read (no cost).
 */
static void pal_i2c_bus_recovery(i2c_inst_t *i2c) {
    if (!i2c_get_write_available(i2c)) {
        // TX FIFO is stuck full: reset the I2C core and re-pin the GPIOs.
        i2c_init(i2c, TRUSTM_I2C_INIT_HZ);
        gpio_set_function(TRUSTM_I2C_SDA, GPIO_FUNC_I2C);
        gpio_set_function(TRUSTM_I2C_SCL, GPIO_FUNC_I2C);
        gpio_pull_up(TRUSTM_I2C_SDA);
        gpio_pull_up(TRUSTM_I2C_SCL);
    }
}

void invoke_upper_layer_callback(const pal_i2c_t *p_pal_i2c_ctx, optiga_lib_status_t event) {
    upper_layer_callback_t upper_layer_handler;

    upper_layer_handler = (upper_layer_callback_t)p_pal_i2c_ctx->upper_layer_event_handler;
    upper_layer_handler(p_pal_i2c_ctx->p_upper_layer_ctx, event);

    // Release I2C bus
    pal_i2c_release((void *)p_pal_i2c_ctx);
}

pal_status_t pal_i2c_init(const pal_i2c_t *p_i2c_context) {
    i2c_inst_t *i2c = (i2c_inst_t *)p_i2c_context->p_i2c_hw_config;

    i2c_init(i2c, TRUSTM_I2C_INIT_HZ);
    gpio_set_function(TRUSTM_I2C_SDA, GPIO_FUNC_I2C);
    gpio_set_function(TRUSTM_I2C_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(TRUSTM_I2C_SDA);
    gpio_pull_up(TRUSTM_I2C_SCL);
    return PAL_STATUS_SUCCESS;
}

pal_status_t pal_i2c_deinit(const pal_i2c_t *p_i2c_context) {
    (void)p_i2c_context;
    return PAL_STATUS_SUCCESS;
}

pal_status_t pal_i2c_write(const pal_i2c_t *p_i2c_context, uint8_t *p_data, uint16_t length) {
    pal_status_t status = PAL_STATUS_FAILURE;

    // Acquire the I2C bus before read/write
    if (PAL_STATUS_SUCCESS == pal_i2c_acquire(p_i2c_context)) {
        gp_pal_i2c_current_ctx = (pal_i2c_t *)p_i2c_context;
        i2c_inst_t *i2c = (i2c_inst_t *)p_i2c_context->p_i2c_hw_config;

        // Blocking write; the transaction ends with a STOP (the IFX
        // protocol is STOP-based, see file header).
        //
        // Pico SDK 2.x API notes (both bit us in bench testing):
        //   - addr is the 7-bit address, UNSHIFTED: the driver writes it
        //     straight into the hw target register and the controller
        //     appends the R/W bit. Passing (addr << 1) puts 2*addr on
        //     the wire and the chip NAKs every transaction.
        //   - the last parameter is *nostop*: false = issue a STOP after
        //     the transfer (1.x called this "ack" with opposite
        //     polarity).
        // Recover from a stuck I2C core before the write (see
        // pal_i2c_bus_recovery).
        pal_i2c_bus_recovery(i2c);

        int rc = i2c_write_timeout_per_char_us(
            i2c, (uint8_t)p_i2c_context->slave_address, p_data, length, false,
            PAL_I2C_TIMEOUT_PER_CHAR_US);

#ifdef PICO_OPTIGA_PAL_TRACE
        trace_i2c("W", p_i2c_context, p_data, length, rc);
#endif
        if (rc >= 0) {
            // rc > 0: fully acknowledged; rc == 0: address ACKed but the
            // data byte NAKed. Either way the chip answered its address,
            // so the transaction reached the device.
            invoke_upper_layer_callback(gp_pal_i2c_current_ctx, PAL_I2C_EVENT_SUCCESS);
            status = PAL_STATUS_SUCCESS;
        } else {
            // Address NAK / timeout: the chip is busy or idle (it does
            // not acknowledge its address in those states, datasheet
            // A.2.1). Report BUSY so the IFX physical layer retries on
            // its 1 ms polling cadence instead of aborting.
            ((upper_layer_callback_t)(p_i2c_context->upper_layer_event_handler)
            )(p_i2c_context->p_upper_layer_ctx, PAL_I2C_EVENT_BUSY);

            // Release I2C Bus
            pal_i2c_release((void *)p_i2c_context);
            status = PAL_STATUS_I2C_BUSY;
        }
    } else {
        status = PAL_STATUS_I2C_BUSY;
        ((upper_layer_callback_t)(p_i2c_context->upper_layer_event_handler)
        )(p_i2c_context->p_upper_layer_ctx, PAL_I2C_EVENT_BUSY);
    }
    return status;
}

pal_status_t pal_i2c_read(const pal_i2c_t *p_i2c_context, uint8_t *p_data, uint16_t length) {
    pal_status_t status = PAL_STATUS_FAILURE;

    // Acquire the I2C bus before read/write
    if (PAL_STATUS_SUCCESS == pal_i2c_acquire(p_i2c_context)) {
        gp_pal_i2c_current_ctx = (pal_i2c_t *)p_i2c_context;
        i2c_inst_t *i2c = (i2c_inst_t *)p_i2c_context->p_i2c_hw_config;

        // Recover from a stuck I2C core before the read (see
        // pal_i2c_bus_recovery). Without this the read path's unbounded
        // TX-FIFO-space wait can hang the superloop forever.
        pal_i2c_bus_recovery(i2c);

        // Blocking read (master NAKs the last byte, then STOP). 7-bit
        // address unshifted, last arg nostop=false (see pal_i2c_write).
        int rc = i2c_read_timeout_per_char_us(
            i2c, (uint8_t)p_i2c_context->slave_address, p_data, length, false,
            PAL_I2C_TIMEOUT_PER_CHAR_US);

#ifdef PICO_OPTIGA_PAL_TRACE
        trace_i2c("R", p_i2c_context, p_data, length, rc);
#endif
        if (rc >= 0) {
            invoke_upper_layer_callback(gp_pal_i2c_current_ctx, PAL_I2C_EVENT_SUCCESS);
            status = PAL_STATUS_SUCCESS;
        } else {
            // Same BUSY mapping as pal_i2c_write().
            ((upper_layer_callback_t)(p_i2c_context->upper_layer_event_handler)
            )(p_i2c_context->p_upper_layer_ctx, PAL_I2C_EVENT_BUSY);

            // Release I2C Bus
            pal_i2c_release((void *)p_i2c_context);
            status = PAL_STATUS_I2C_BUSY;
        }
    } else {
        status = PAL_STATUS_I2C_BUSY;
        ((upper_layer_callback_t)(p_i2c_context->upper_layer_event_handler)
        )(p_i2c_context->p_upper_layer_ctx, PAL_I2C_EVENT_BUSY);
    }
    return status;
}

pal_status_t pal_i2c_set_bitrate(const pal_i2c_t *p_i2c_context, uint16_t bitrate) {
    pal_status_t return_status = PAL_STATUS_FAILURE;
    optiga_lib_status_t event = PAL_I2C_EVENT_ERROR;

    // Acquire the I2C bus before setting the bitrate
    if (PAL_STATUS_SUCCESS == pal_i2c_acquire(p_i2c_context)) {
        // If the user provided bitrate is greater than the I2C master
        // hardware maximum supported value, set the I2C master to its
        // maximum supported value.
        if (bitrate > PAL_I2C_MASTER_MAX_BITRATE) {
            bitrate = PAL_I2C_MASTER_MAX_BITRATE;
        }
        i2c_set_baudrate((i2c_inst_t *)p_i2c_context->p_i2c_hw_config,
                         (uint32_t)bitrate * 1000u);
#ifdef PICO_OPTIGA_PAL_TRACE
        printf("[I2C] br %u kHz\n", (unsigned)bitrate);
#endif
        return_status = PAL_STATUS_SUCCESS;
        event = PAL_I2C_EVENT_SUCCESS;
    } else {
        return_status = PAL_STATUS_I2C_BUSY;
        event = PAL_I2C_EVENT_BUSY;
    }
    if (0 != p_i2c_context->upper_layer_event_handler) {
        ((upper_layer_callback_t)(p_i2c_context->upper_layer_event_handler)
        )(p_i2c_context->p_upper_layer_ctx, event);
    }
    // Release I2C Bus if its acquired
    if (PAL_STATUS_I2C_BUSY != return_status) {
        pal_i2c_release((void *)p_i2c_context);
    }
    return return_status;
}
