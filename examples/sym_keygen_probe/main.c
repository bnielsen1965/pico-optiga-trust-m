/*
 * sym_keygen_probe — SAFE probe: can we keygen a symmetric key into 0xE200?
 *
 * Background. The Vault wants an on-chip AES-256 master in the symmetric slot
 * 0xE200 (the analog of the ATECC608's slot-7 master). The open question is
 * whether the chip will accept a keygen into 0xE200, whose Change AC reads
 * NEV in the metadata inventory. The vendor's own example writes the Change
 * AC -> ALW *before* keygen, which we cannot do on a NEV slot (writing an AC
 * is forbidden by our safety model). But the chip's gen_symkey command may set
 * up what it needs on its own, in which case no metadata write is required.
 *
 * This probe answers that directly:
 *   1. reads + prints the raw 0xE200 metadata (is the key-algo tag 0xE0
 *      already present, as the vendor's example checks for?);
 *   2. attempts pico_optiga_symmetric_generate_key(0xE200, AES-256,
 *      ENCRYPTION, no export);
 *   3. on success, re-reads the metadata (the used size should now be > 0) and
 *      derives a test key with pico_optiga_hkdf(0xE200, ...) to confirm the
 *      full master -> HKDF path the Vault would use.
 *
 * SAFETY. A symmetric keygen does not advance any life-cycle state and cannot
 * brick the chip. If it succeeds, 0xE200 now holds an AES-256 key — exactly
 * what the Vault wants as its master. If it fails, nothing changed. (Running
 * it twice re-keys the slot, which is harmless for a probe.)
 *
 * This is a diagnostic, not a littlefs app: it links only the standard deps.
 */

#include <pico/stdlib.h>
#include <stdio.h>
#include <string.h>

#include "pico_optiga/optiga.h"

#define MASTER_OID 0xE200u  /* the only symmetric key slot on this chip */

static void print_hex(const uint8_t *buf, size_t len) {
    for (size_t i = 0; i < len; i++) {
        printf("%02X", (unsigned)buf[i]);
        if ((i + 1u) % 16u == 0u) {
            printf("\n    ");
        } else if (i + 1u < len) {
            printf(" ");
        }
    }
    printf("\n");
}

/* Read 0xE200 metadata and summarize it: the raw bytes, whether the key-algo
 * tag (0xE0) the vendor's example checks for is present, and whether the
 * used-size tag (0xC5) is non-empty (a key is stored). Returns the read rc. */
static int probe_slot(const char *label) {
    uint8_t meta[256];
    size_t n = 0;
    int rc = pico_optiga_read_metadata(MASTER_OID, meta, sizeof(meta), &n);
    if (PICO_OPTIGA_OK != rc) {
        printf("%s: metadata read FAIL — %s\n", label,
               pico_optiga_strerror(rc));
        return rc;
    }
    printf("%s: 0x%04X metadata (%u bytes):\n    ", label, (unsigned)MASTER_OID,
           (unsigned)n);
    print_hex(meta, n);

    int have_algo = 0;
    int used_nonzero = 0;
    for (size_t i = 0; i < n; i++) {
        if (meta[i] == 0xE0u) {
            have_algo = 1;
        }
        /* 0xC5 is the used-size tag; a non-zero following byte means a key is
         * present. (Coarse — good enough for a before/after comparison.) */
        if ((meta[i] == 0xC5u) && (i + 2u < n) && (0u != meta[i + 2u])) {
            used_nonzero = 1;
        }
    }
    printf("    key-algo tag 0xE0 present: %s | used-size non-empty: %s\n",
           have_algo ? "yes" : "no", used_nonzero ? "yes" : "no");
    return PICO_OPTIGA_OK;
}

int main(void) {
    stdio_init_all();
    printf("== symmetric keygen probe on 0x%04X (SAFE) ==\n\n",
           (unsigned)MASTER_OID);

    int rc = pico_optiga_init();
    if (PICO_OPTIGA_OK != rc) {
        printf("init: FAIL — %s\n", pico_optiga_strerror(rc));
        pico_optiga_deinit();
        return 1;
    }
    printf("init: OK\n\n");

    /* 1. Slot state before keygen. */
    probe_slot("before");
    printf("\n");

    /* 2. Attempt the keygen (AES-256, ENCRYPTION usage, key stays on chip). */
    printf("attempting symmetric keygen (AES-256, ENCRYPTION, no export)...\n");
    rc = pico_optiga_symmetric_generate_key(
        MASTER_OID, PICO_OPTIGA_SYM_AES_256, PICO_OPTIGA_USAGE_ENCRYPTION, 0,
        NULL, 0);
    if (PICO_OPTIGA_OK != rc) {
        printf("keygen: FAIL — %s\n", pico_optiga_strerror(rc));
        printf("\n=> 0x%04X is NOT keygen-able as-is (Change AC NEV blocks it).\n"
               "   Fall back to the ECC-master design (0xE0F2).\n",
               (unsigned)MASTER_OID);
        pico_optiga_deinit();
        return 1;
    }
    printf("keygen: OK — 0x%04X now holds an on-chip AES-256 key.\n\n",
           (unsigned)MASTER_OID);

    /* 3. Confirm the slot now holds a key, and that master -> HKDF works. */
    probe_slot("after");
    printf("\n");

    uint8_t salt[16] = {0};
    uint8_t info[8] = {0};
    uint8_t dk[32];
    rc = pico_optiga_hkdf(MASTER_OID, salt, sizeof(salt), info, sizeof(info),
                          dk, sizeof(dk));
    if (PICO_OPTIGA_OK != rc) {
        printf("hkdf from 0x%04X: FAIL — %s\n", (unsigned)MASTER_OID,
               pico_optiga_strerror(rc));
    } else {
        printf("hkdf from 0x%04X: OK — derived 32-byte key:\n    ",
               (unsigned)MASTER_OID);
        print_hex(dk, sizeof(dk));
        printf("\n=> 0x%04X works as the Vault master (keygen + HKDF both OK).\n",
               (unsigned)MASTER_OID);
    }

    pico_optiga_deinit();
    printf("\ndone\n");
    return 0;
}
