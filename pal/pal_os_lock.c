/*
 * pal_os_lock.c
 *
 * Pico port of the Infineon NEW_PAL_TEMPLATE pal_os_lock.c.
 *
 * Single-core, non-preemptive superloop: nothing can interrupt the
 * lock logic, so the template's trivial counter is correct as-is.
 */

#include "pal_os_lock.h"

void pal_os_lock_create(pal_os_lock_t *p_lock, uint8_t lock_type) {
    p_lock->type = lock_type;
    p_lock->lock = 0;
}

void pal_os_lock_destroy(pal_os_lock_t *p_lock) {
    (void)p_lock;
}

pal_status_t pal_os_lock_acquire(pal_os_lock_t *p_lock) {
    pal_status_t return_status = PAL_STATUS_FAILURE;

    if (!(p_lock->lock)) {
        p_lock->lock++;
        if (1 != p_lock->lock) {
            p_lock->lock--;
        }
        return_status = PAL_STATUS_SUCCESS;
    }
    return return_status;
}

void pal_os_lock_release(pal_os_lock_t *p_lock) {
    if (0 != p_lock->lock) {
        p_lock->lock--;
    }
}

void pal_os_lock_enter_critical_section() {
    // No preemption on this target: nothing to do.
}

void pal_os_lock_exit_critical_section() {
    // No preemption on this target: nothing to do.
}
