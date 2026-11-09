/*
 * data_objects — Phase 5, Step 2: write a marker to a confirmed-open slot
 * and prove it persists across a reset.
 *
 * Target: 0xF1D1 — an Arbitrary Data Type 3 slot. Step 1's read-only
 * inventory confirmed it is safe to use as scratch:
 *
 *     0xF1D1  creation  140 B  Change=LcsO<0x07  Read=ALW
 *
 * i.e. it is in the creation life cycle (so LcsO<0x07 allows writes), its
 * Change access condition is open, and it reads back freely. It is a plain
 * data slot (no data-type tag, not a key/cert/secret/counter).
 *
 * What this example does (and what it deliberately does NOT do):
 *
 *   DOES: write a 16-byte marker to the *data* of 0xF1D1, read it back,
 *         reset the chip, and (after the reset) read it again to prove the
 *         write landed in tamper-resistant NVM that survives power/reset.
 *
 *   DOES NOT:
 *     - touch 0xF1D0 — Step 1 showed that is the locked Authorization
 *       Reference (LcsO=operational, Read=NEV, shielded Change AC).
 *     - write any life-cycle state (0xE0C0 / 0xF1C0 / any LcsO). Advancing
 *       a life cycle toward termination (0x0F) is irreversible.
 *     - write any metadata / access-condition tag. Setting a Change AC to
 *       NEV would freeze a slot forever. pico_optiga exposes no
 *       metadata-write API, so this cannot happen through it.
 *
 * A data write changes neither the object's LcsO nor its metadata — it is
 * safe and repeatable, bounded only by NVM endurance (~1e5 cycles). We do a
 * handful of writes, not a loop.
 *
 * Flow:
 *   boot 1: 0xF1D1 has no marker  -> write it -> read back (match) -> reset
 *   boot 2: 0xF1D1 has the marker -> "PERSISTENCE CONFIRMED" -> done
 */

#include <hardware/watchdog.h>
#include <pico/stdlib.h>
#include <stdio.h>
#include <string.h>

#include "pico_optiga/optiga.h"

/* The confirmed-open scratch slot (from Step 1). Do NOT change this to
 * 0xF1D0 (locked AUTHREF) or any lifecycle/metadata object. */
#define TARGET_OID 0xF1D1u

/* 16-byte marker. The "POP5" magic makes detection unambiguous. */
static const uint8_t kMarker[16] = {
    0x50, 0x4F, 0x50, 0x35, /* "POP5" */
    0x00, 0x01, 0x02, 0x03,
    0x04, 0x05, 0x06, 0x07,
    0xDE, 0xAD, 0xBE, 0xEF,
};
#define MARKER_LEN (sizeof(kMarker))

static void print_hex(const char *label, const uint8_t *buf, size_t len) {
    printf("  %-9s", label);
    for (size_t i = 0; i < len; i++) {
        printf("%02X ", (unsigned)buf[i]);
    }
    printf("\n");
}

/* Read up to MARKER_LEN bytes from TARGET_OID; return bytes read. */
static size_t read_target(uint8_t *buf) {
    size_t n = 0;
    if (PICO_OPTIGA_OK !=
        pico_optiga_read_data_obj(TARGET_OID, buf, MARKER_LEN, &n)) {
        return 0;
    }
    return n;
}

int main(void) {
    stdio_init_all();

    printf("== OPTIGA Trust M data objects — persistence demo (Step 2) ==\n");
    printf("Target slot: 0x%04X (Arbitrary Data Type 3, open per Step 1)\n\n",
           (unsigned)TARGET_OID);

    int rc = pico_optiga_init();
    if (PICO_OPTIGA_OK != rc) {
        printf("init: FAIL — %s\n", pico_optiga_strerror(rc));
        pico_optiga_deinit();
        return 1;
    }
    printf("init: OK\n\n");

    uint8_t buf[MARKER_LEN];
    size_t n = read_target(buf);
    if (n == MARKER_LEN && 0 == memcmp(buf, kMarker, MARKER_LEN)) {
        /* Marker already present — this is boot 2, after the reset. */
        print_hex("data:", buf, n);
        printf("  data MATCHES marker\n");
        printf("  (reboot was %s)\n\n",
               watchdog_enable_caused_reboot() ? "watchdog-triggered" : "not watchdog");
        printf("PERSISTENCE CONFIRMED: the marker written before the reset\n");
        printf("is still in NVM. The write is durable across power/reset.\n\n");
        pico_optiga_deinit();
        printf("done\n");
        return 0;
    }

    /* Boot 1: no marker yet. Show what's there, then write. */
    printf("Current contents of 0x%04X (%u bytes):\n", (unsigned)TARGET_OID,
           (unsigned)n);
    if (n > 0u) {
        print_hex("  data:", buf, n);
    } else {
        printf("  (read returned 0 bytes)\n");
    }
    printf("  -> no marker present; writing one now\n\n");

    print_hex("marker:", kMarker, MARKER_LEN);
    rc = pico_optiga_write_data_obj(TARGET_OID, PICO_OPTIGA_ERASE_AND_WRITE,
                                    kMarker, MARKER_LEN);
    if (PICO_OPTIGA_OK != rc) {
        printf("write: FAIL — %s\n", pico_optiga_strerror(rc));
        pico_optiga_deinit();
        return 1;
    }
    printf("write: OK (ERASE_AND_WRITE)\n\n");

    /* Read back to confirm the write landed. */
    n = read_target(buf);
    print_hex("readback:", buf, n);
    if (n != MARKER_LEN || 0 != memcmp(buf, kMarker, MARKER_LEN)) {
        printf("readback does NOT match marker — aborting, no reset.\n");
        pico_optiga_deinit();
        return 1;
    }
    printf("  readback MATCHES marker\n\n");

    printf("To prove persistence, resetting the chip now (watchdog)...\n");
    fflush(stdout);
    /* Arm the watchdog to reset the board in 300 ms. We never call
     * watchdog_update(), so it fires and reboots; the sleep keeps us out
     * of the way (it will not return). */
    watchdog_enable(300u, false);
    sleep_ms(1000u);
    /* not reached */
    return 0;
}
