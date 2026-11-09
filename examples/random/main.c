/*
 * random — Phase 5, Step 3: pull true random from the chip's hardware TRNG.
 *
 * Uses pico_optiga_get_random(), which wraps the host library's
 * optiga_crypt_random() with the true (hardware) RNG type. This is a pure
 * read — it never writes to the chip, so there is no NVM/lifecycle risk.
 *
 * To show the output is a live RNG (not a constant), we fetch two 32-byte
 * blocks and confirm they differ. 32 bytes is a common size (e.g. an AES-256
 * key or a P-256 scalar), and is well within the chip's 8..256 byte range.
 */

#include <pico/stdlib.h>
#include <stdio.h>
#include <string.h>

#include "pico_optiga/optiga.h"

#define FETCH_LEN 32u

static void print_hex(const uint8_t *buf, size_t len) {
    for (size_t i = 0; i < len; i++) {
        printf("%02X", (unsigned)buf[i]);
        if ((i + 1u) % 16u == 0u) {
            printf("\n  ");
        } else if (i + 1u < len) {
            printf(" ");
        }
    }
    printf("\n");
}

int main(void) {
    stdio_init_all();

    printf("== OPTIGA Trust M — true random (Step 3) ==\n");

    int rc = pico_optiga_init();
    if (PICO_OPTIGA_OK != rc) {
        printf("init: FAIL — %s\n", pico_optiga_strerror(rc));
        pico_optiga_deinit();
        return 1;
    }
    printf("init: OK\n\n");

    uint8_t a[FETCH_LEN];
    uint8_t b[FETCH_LEN];

    rc = pico_optiga_get_random(a, FETCH_LEN);
    if (PICO_OPTIGA_OK != rc) {
        printf("random #1: FAIL — %s\n", pico_optiga_strerror(rc));
        pico_optiga_deinit();
        return 1;
    }
    printf("Fetch #1 (%u bytes):\n  ", (unsigned)FETCH_LEN);
    print_hex(a, FETCH_LEN);

    rc = pico_optiga_get_random(b, FETCH_LEN);
    if (PICO_OPTIGA_OK != rc) {
        printf("random #2: FAIL — %s\n", pico_optiga_strerror(rc));
        pico_optiga_deinit();
        return 1;
    }
    printf("Fetch #2 (%u bytes):\n  ", (unsigned)FETCH_LEN);
    print_hex(b, FETCH_LEN);

    printf("\nTwo %u-byte fetches %s.\n", (unsigned)FETCH_LEN,
           (0 == memcmp(a, b, FETCH_LEN)) ? "are IDENTICAL (unexpected!)"
                                          : "differ — live TRNG confirmed");

    pico_optiga_deinit();
    printf("done\n");
    return 0;
}
