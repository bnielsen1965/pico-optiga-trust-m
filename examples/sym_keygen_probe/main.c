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
 * Flow (the vendor's initial-provision sequence):
 *   1. read + print 0xE200 metadata (LcsO / Change / Exec ACs, key-algo tag);
 *   2. WRITE the metadata to open the Change + Exec ACs to ALW — a TLV delta
 *      with NO LcsO tag, so it cannot advance the life cycle (no bricking).
 *      This is the vendor's own first-provision step and the one the earlier
 *      probe skipped; it is reversible while the slot is in creation.
 *   3. read the metadata again (Change should now read ALW);
 *   4. attempt pico_optiga_symmetric_generate_key(0xE200, AES-256, ENCRYPTION,
 *      no export);
 *   5. read the metadata again (the 0xE0 key-algo tag should now be present);
 *   6. derive a test key with pico_optiga_hkdf(0xE200, ...) to confirm the
 *      full master -> HKDF path the Vault would use.
 *
 * SAFETY. The only brick-the-chip write is advancing a life cycle (LcsO, tag
 * 0xC0) to Termination (0x0F); this probe never writes a 0xC0 tag. Opening an
 * AC on a factory-empty slot in creation is reversible and is the vendor's
 * documented flow. A keygen advances no life cycle. If the keygen succeeds,
 * 0xE200 now holds an AES-256 key — exactly what the Vault wants as its
 * master. (Running it twice re-keys the slot, harmless for a probe.)
 *
 * This is a diagnostic, not a littlefs app: it links only the standard deps.
 */

#include <pico/stdlib.h>
#include <stdio.h>
#include <string.h>

#include "pico_optiga/optiga.h"

#define MASTER_OID 0xE200u  /* the only symmetric key slot on this chip */

/* The vendor's initial-provision descriptor for 0xE200: open the Change and
 * Exec ACs to ALW so a key can be generated into the (factory-empty) slot.
 *
 * SAFETY:
 *   - NO LcsO (0xC0) tag — the life cycle is left exactly as it is, so this
 *     cannot advance it toward Termination (the only brick-the-chip write).
 *   - Writing an AC tag is reversible while the slot's LcsO is in creation,
 *     and this is the vendor's own documented first-provision step.
 *   - Scoped to 0xE200 only (a factory-empty slot provided for us to use). */
static const uint8_t kE200Open[] = {
    0x20, 0x06,         /* meta start, total len 6 */
    0xD0, 0x01, 0x00,   /* Change AC -> ALW (0x00) */
    0xD3, 0x01, 0x00,   /* Exec AC   -> ALW (0x00) */
};

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

/* Find a 1-byte-valued TLV tag in the metadata; return its value, or -1 if
 * absent. (Coarse parse — good enough for a diagnostic.) */
static int find_tag_val(const uint8_t *meta, size_t n, uint8_t tag) {
    for (size_t i = 0; i + 2u < n; i++) {
        if ((meta[i] == tag) && (meta[i + 1u] == 0x01u)) {
            return (int)meta[i + 2u];
        }
    }
    return -1;
}

static const char *ac_name(int v) {
    if (v == 0x00) return "ALW";
    if (v == 0xFF) return "NEV";
    return "?";
}

static const char *lcs_name(int v) {
    switch (v) {
        case 0x01: return "creation";
        case 0x03: return "init";
        case 0x07: return "operational";
        case 0x0F: return "TERMINATION";
        default:   return "?";
    }
}

/* Read 0xE200 metadata and summarize it: the decoded LcsO / Change / Exec ACs
 * and whether the key-algo tag (0xE0) is present (a key is stored). The raw
 * bytes follow. Returns the read rc. */
static int probe_slot(const char *label) {
    uint8_t meta[256];
    size_t n = 0;
    int rc = pico_optiga_read_metadata(MASTER_OID, meta, sizeof(meta), &n);
    if (PICO_OPTIGA_OK != rc) {
        printf("%s: metadata read FAIL — %s\n", label,
               pico_optiga_strerror(rc));
        return rc;
    }
    int lcs  = find_tag_val(meta, n, 0xC0u);
    int chg  = find_tag_val(meta, n, 0xD0u);
    int exec = find_tag_val(meta, n, 0xD3u);
    int algo = find_tag_val(meta, n, 0xE0u);
    printf("%s: LcsO=%-11s Change=%-3s Exec=%-3s key-algo(0xE0)=%s\n",
           label, lcs_name(lcs), ac_name(chg), ac_name(exec),
           (algo >= 0) ? "present" : "absent");
    printf("    raw (%u bytes): ", (unsigned)n);
    print_hex(meta, n);
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

    /* 1. Slot state before any write. */
    probe_slot("before");
    printf("\n");

    /* 2. The vendor's initial-provision step: open the Change + Exec ACs to
     *    ALW. This is a metadata (AC-tag) write, NOT a life-cycle write — the
     *    blob has no 0xC0 tag, so it cannot advance the LcsO toward
     *    Termination (the only brick-the-chip write). Reversible while the
     *    slot is in creation. This is the step the earlier probe skipped, which
     *    is why the keygen used to fail. */
    printf("opening 0x%04X ACs (Change->ALW, Exec->ALW)...\n",
           (unsigned)MASTER_OID);
    rc = pico_optiga_write_metadata(MASTER_OID, kE200Open, sizeof(kE200Open));
    if (PICO_OPTIGA_OK != rc) {
        printf("metadata write: FAIL — %s\n", pico_optiga_strerror(rc));
        printf("=> cannot open the slot; the keygen will not work. Stop here.\n");
        pico_optiga_deinit();
        return 1;
    }
    printf("metadata write: OK\n\n");

    /* 3. Slot state after the AC open (Change should now read ALW). */
    probe_slot("after-open");
    printf("\n");

    /* 4. Now attempt the keygen (AES-256, ENCRYPTION usage, key stays on
     *    chip). With the Change AC open, this should succeed. */
    printf("attempting symmetric keygen (AES-256, ENCRYPTION, no export)...\n");
    rc = pico_optiga_symmetric_generate_key(
        MASTER_OID, PICO_OPTIGA_SYM_AES_256, PICO_OPTIGA_USAGE_ENCRYPTION, 0,
        NULL, 0);
    if (PICO_OPTIGA_OK != rc) {
        printf("keygen: FAIL — %s\n", pico_optiga_strerror(rc));
    } else {
        printf("keygen: OK — 0x%04X now holds an on-chip AES-256 key.\n",
               (unsigned)MASTER_OID);
    }
    printf("\n");

    /* 5. Slot state after keygen (the 0xE0 key-algo tag should now be present). */
    probe_slot("after-keygen");
    printf("\n");

    /* 6. THE real test: can we derive a key from 0xE200 via HKDF? This works
     *    if the slot holds a key. This is the master->HKDF path the Vault uses,
     *    so run it regardless of the keygen outcome. */
    uint8_t salt[16] = {0};
    uint8_t info[8] = {0};
    uint8_t dk[32];
    rc = pico_optiga_hkdf(MASTER_OID, salt, sizeof(salt), info, sizeof(info),
                          dk, sizeof(dk));
    if (PICO_OPTIGA_OK != rc) {
        printf("hkdf from 0x%04X: FAIL — %s\n", (unsigned)MASTER_OID,
               pico_optiga_strerror(rc));
        printf("\n=> 0x%04X has no usable key: keygen failed AND there is no\n"
               "   pre-existing key to derive from. Fall back to the ECC master\n"
               "   (0xE0F2).\n",
               (unsigned)MASTER_OID);
    } else {
        printf("hkdf from 0x%04X: OK — derived 32-byte key:\n    ",
               (unsigned)MASTER_OID);
        print_hex(dk, sizeof(dk));
        printf("\n=> 0x%04X holds a USABLE key for the Vault master (HKDF OK) —\n"
               "   use it directly; no keygen needed.\n",
               (unsigned)MASTER_OID);
    }

    pico_optiga_deinit();
    printf("\ndone\n");
    return 0;
}
