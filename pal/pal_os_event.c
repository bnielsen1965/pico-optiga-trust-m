/*
 * pal_os_event.c
 *
 * Pico port of the Infineon NEW_PAL_TEMPLATE pal_os_event.c.
 *
 * The Infineon stack is event-driven: every asynchronous step is a
 * one-shot timer (50 us guard time, 1 ms busy polling, 2 s + 12 s
 * reset sequence). On an OS (the Linux/Zephyr PALs) the expiry runs the
 * callback in a thread. The Pico runs bare-metal superloop code with no
 * threads and no OS scheduler, so this port implements the one-shot as
 * a *software deadline*:
 *
 *   - pal_os_event_register_callback_oneshot() arms a
 *     {callback, args, deadline} slot (no hardware timer, no ISR);
 *   - the application's wait loop calls
 *     pal_os_event_trigger_registered_callback() — the "pump" — and the
 *     callback fires in thread context once its deadline has been
 *     reached.
 *
 * A single slot is the intended design: the stack's state machines are
 * chains of one-shots with at most one pending at a time (optiga_cmd
 * and ifx_i2c share the same event object).
 *
 * Pump cadence: applications poll at ~1 ms, which matches the stack's
 * own PL_POLLING_INVERVAL_US (1 ms). The 50 us guard time stretches to
 * at most ~1 ms in practice — fine, the datasheet specifies a minimum,
 * not a target.
 *
 * Example pump loop (see examples/optiga_open/main.c):
 *
 *     while (OPTIGA_LIB_BUSY == status) {
 *         pal_os_event_trigger_registered_callback();
 *         sleep_ms(1);
 *     }
 */

#include "pico/time.h"
#include "pal_os_event.h"

static pal_os_event_t pal_os_event_0 = {0};

static struct {
    bool_t armed;
    register_callback callback;
    void *args;
    absolute_time_t deadline;
} g_oneshot;

pal_os_event_t *pal_os_event_create(register_callback callback, void *callback_args) {
    if ((NULL != callback) && (NULL != callback_args)) {
        pal_os_event_0.callback_ctx = callback_args;
        pal_os_event_0.callback_registered = callback;
        return &pal_os_event_0;
    }
    return NULL;
}

void pal_os_event_destroy(pal_os_event_t *p_pal_os_event) {
    (void)p_pal_os_event;
    g_oneshot.armed = FALSE;
}

void pal_os_event_register_callback_oneshot(
    pal_os_event_t *p_pal_os_event,
    register_callback callback,
    void *callback_args,
    uint32_t time_us
) {
    (void)p_pal_os_event; /* single shared slot, see file header */
    g_oneshot.armed = TRUE;
    g_oneshot.callback = callback;
    g_oneshot.args = callback_args;
    g_oneshot.deadline = make_timeout_time_us((uint64_t)time_us);
}

void pal_os_event_start(
    pal_os_event_t *p_pal_os_event,
    register_callback callback,
    void *callback_args
) {
    if ((NULL != p_pal_os_event) && (FALSE == p_pal_os_event->is_event_triggered)) {
        p_pal_os_event->is_event_triggered = TRUE;
        // Arm for the next pump: deadline = now.
        pal_os_event_register_callback_oneshot(p_pal_os_event, callback, callback_args, 0);
    }
}

void pal_os_event_stop(pal_os_event_t *p_pal_os_event) {
    if (NULL != p_pal_os_event) {
        p_pal_os_event->is_event_triggered = FALSE;
    }
    g_oneshot.armed = FALSE;
}

void pal_os_event_trigger_registered_callback(void) {
    // Pico SDK 2.x has no time_reached(): a deadline is reached when the
    // signed difference deadline -> now is <= 0.
    if (g_oneshot.armed
        && (absolute_time_diff_us(g_oneshot.deadline, get_absolute_time()) <= 0)) {
        // Disarm before invoking: the callback re-arms the slot for the
        // next step of the state machine.
        g_oneshot.armed = FALSE;
        register_callback callback = g_oneshot.callback;
        void *args = g_oneshot.args;
        callback(args);
    }
}
