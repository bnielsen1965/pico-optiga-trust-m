/*
 * crypt — Phase 6, Step 1a + 1b: on-chip crypto tour.
 *
 * Exercises the pico_optiga crypto wrappers end to end on the chip:
 *
 *   Part 1 (Step 1a) — ECDSA core:
 *     1. ecc_gen_keypair  generate a NIST P-256 pair into 0xE0F2 (private
 *                         stays on the chip, public key returned).
 *     2. ecdsa_sign       sign a SHA-256 digest on the chip (DER signature).
 *     3. ecdsa_verify     verify it (must pass) and a corrupted copy
 *                         (must be rejected).
 *
 *   Part 2 (Step 1b) — the remaining primitives:
 *     4. sha256           chip SHA-256 of the same message; must match the
 *                         Pico's (cross-checks the offload).
 *     5. ecdh             shared secret of 0xE0F2 and a fixed peer public key
 *                         (exported to the host).
 *     6. ecdh + hkdf      ECDH into the chip session, then HKDF-SHA256 derive
 *                         a 32-byte key from that session secret (the real
 *                         ECDH->HKDF flow, no secret written to NVM).
 *     7. counter          increment the on-chip up-counter 0xE120 and read it
 *                         back (the FIDO2 sync counter).
 *
 * Not exercised here: symmetric (AES) encrypt/decrypt — it needs a symmetric
 * key provisioned at a confirmed-open slot (0xE200 is not yet inventoried).
 * See PLAN.md, Phase 6.
 *
 * SAFETY — writes in this example:
 *   - keygen into 0xE0F2 (Step 0 confirmed it a safe target: creation,
 *     Change AC already open — no AC-tag write; may advance creation->
 *     operational, never toward termination).
 *   - counter init (count=0, threshold=10) + one increment of 0xE120
 *     (Step 0 confirmed it an open counter; these are data-payload writes to
 *     the counter, not metadata/AC). The init resets it each flash — fine for
 *     a demo; a real FIDO2 app would init once and only increment.
 * Every other operation is a stateless computation.
 *
 * SHA-256 is computed on the Pico (a small, portable, self-contained
 * implementation below — no hardware dependency, host-testable). It is
 * verified against two NIST test vectors before any digest is trusted.
 */

#include <pico/stdlib.h>
#include <stdio.h>
#include <string.h>

#include "pico_optiga/optiga.h"

/* Confirmed-safe targets (Step 0 inventory). */
#define KEY_OID      0xE0F2u  /* ECC keygen / sign / ecdh */
#define COUNTER_OID  0xE120u  /* up-counter (FIDO2 sync counter) */
#define PUB_CAP      128u
#define SIG_CAP      128u

/* Fixed peer public key for the ECDH demo (a valid P-256 point, in the same
 * 68-byte format a keygen returns: 03 42 00 04 || X(32) || Y(32)). */
static const uint8_t kPeerPub[68] = {
    0x03, 0x42, 0x00, 0x04,
    0x94, 0x89, 0x2F, 0x09, 0xEA, 0x4E, 0xCA, 0xBC, 0x6A, 0x4E, 0xF2, 0x06,
    0x36, 0x26, 0xE0, 0x5D, 0xE0, 0xD5, 0xF9, 0x77, 0xEA, 0xC3, 0xB2, 0x70,
    0xAC, 0xE2, 0x19, 0x00, 0xF5, 0xDB, 0x56, 0xE7, 0x37, 0xBB, 0xBE, 0x46,
    0xE4, 0x49, 0x76, 0x38, 0x25, 0xB5, 0xF8, 0x94, 0x74, 0x9E, 0x1A, 0xB6,
    0x5A, 0xF1, 0x29, 0xD7, 0x3A, 0xB6, 0x9B, 0x80, 0xAC, 0xC5, 0xE1, 0xC3,
    0x10, 0xF2, 0x16, 0xC6};

/* Arbitrary HKDF salt / info (demo only). */
static const uint8_t kSalt[32] = {
    0x61, 0xC7, 0xDE, 0xF9, 0x0F, 0xD5, 0xCD, 0x7A, 0x8B, 0x7A, 0x36, 0x41,
    0x04, 0xE0, 0x0D, 0x82, 0x38, 0x46, 0xBF, 0xB7, 0x70, 0xEE, 0xBF, 0x8F,
    0x40, 0x25, 0x2E, 0x0A, 0x21, 0x42, 0xAF, 0x9C};
static const uint8_t kInfo[16] = {
    0x8D, 0xE4, 0x3F, 0xFF, 0x65, 0x2D, 0xA0, 0xA7,
    0xF0, 0x4E, 0x8F, 0x22, 0x84, 0xA4, 0x28, 0x3B};

/* Counter object layout (8 bytes, big-endian): [count: 4][threshold: 4].
 * Must be initialized (write_data) before update_count works — see the
 * vendor's counter example. threshold=10 is plenty for a single increment. */
static const uint8_t kCounterInit[8] = {
    0x00, 0x00, 0x00, 0x00,  /* initial count = 0 */
    0x00, 0x00, 0x00, 0x0A,  /* threshold = 10 */
};

/* =====================================================================
 * SHA-256 (FIPS 180-4) — small, portable, self-contained.
 *
 * Used to form the digest the chip signs/verifies, and to cross-check the
 * chip's own SHA-256. Checked against two NIST vectors in main() before use.
 * =================================================================== */

typedef struct {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t  buf[64];
    uint32_t buflen;
} sha256_ctx;

static const uint32_t sha256_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static uint32_t rotr32(uint32_t x, uint32_t n) {
    return (x >> n) | (x << (32u - n));
}

static void sha256_init(sha256_ctx *c) {
    c->state[0] = 0x6a09e667; c->state[1] = 0xbb67ae85;
    c->state[2] = 0x3c6ef372; c->state[3] = 0xa54ff53a;
    c->state[4] = 0x510e527f; c->state[5] = 0x9b05688c;
    c->state[6] = 0x1f83d9ab; c->state[7] = 0x5be0cd19;
    c->bitlen = 0;
    c->buflen = 0;
}

static void sha256_transform(sha256_ctx *c, const uint8_t *p) {
    uint32_t w[64];
    for (uint32_t i = 0; i < 16; i++) {
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
    }
    for (uint32_t i = 16; i < 64; i++) {
        uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^
                      (w[i - 15] >> 3);
        uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^
                      (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = c->state[0], b = c->state[1], cc = c->state[2], d = c->state[3];
    uint32_t e = c->state[4], f = c->state[5], g = c->state[6], h = c->state[7];
    for (uint32_t i = 0; i < 64; i++) {
        uint32_t S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + sha256_k[i] + w[i];
        uint32_t S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        uint32_t mj = (a & b) ^ (a & cc) ^ (b & cc);
        uint32_t t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->state[0] += a; c->state[1] += b; c->state[2] += cc; c->state[3] += d;
    c->state[4] += e; c->state[5] += f; c->state[6] += g; c->state[7] += h;
}

static void sha256_update(sha256_ctx *c, const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        c->buf[c->buflen++] = data[i];
        if (c->buflen == 64u) {
            sha256_transform(c, c->buf);
            c->bitlen += 512u;
            c->buflen = 0;
        }
    }
}

static void sha256_final(sha256_ctx *c, uint8_t out[32]) {
    uint64_t bits = c->bitlen + (uint64_t)c->buflen * 8u;
    c->buf[c->buflen++] = 0x80;
    if (c->buflen > 56u) {
        while (c->buflen < 64u) {
            c->buf[c->buflen++] = 0;
        }
        sha256_transform(c, c->buf);
        c->buflen = 0;
    }
    while (c->buflen < 56u) {
        c->buf[c->buflen++] = 0;
    }
    for (int i = 7; i >= 0; i--) {
        c->buf[c->buflen++] = (uint8_t)(bits >> (i * 8));
    }
    sha256_transform(c, c->buf);
    for (int i = 0; i < 8; i++) {
        out[i * 4]     = (uint8_t)(c->state[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(c->state[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(c->state[i] >> 8);
        out[i * 4 + 3] = (uint8_t)(c->state[i]);
    }
}

static void sha256(const uint8_t *data, size_t len, uint8_t out[32]) {
    sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, data, len);
    sha256_final(&c, out);
}

/* Self-check the SHA-256 implementation against two NIST vectors.
 * Returns 1 on success, 0 on any mismatch. */
static int sha256_self_check(void) {
    uint8_t d[32];
    const uint8_t empty[32] = {
        0xe3, 0xb0, 0xc4, 0x42, 0x98, 0xfc, 0x1c, 0x14, 0x9a, 0xfb, 0xf4,
        0xc8, 0x99, 0x6f, 0xb9, 0x24, 0x27, 0xae, 0x41, 0xe4, 0x64, 0x9b,
        0x93, 0x4c, 0xa4, 0x95, 0x99, 0x1b, 0x78, 0x52, 0xb8, 0x55};
    const uint8_t abc[32] = {
        0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40,
        0xde, 0x5d, 0xae, 0x22, 0x23, 0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17,
        0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad};

    sha256((const uint8_t *)"", 0u, d);
    if (0 != memcmp(d, empty, 32u)) {
        return 0;
    }
    sha256((const uint8_t *)"abc", 3u, d);
    if (0 != memcmp(d, abc, 32u)) {
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------- */

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

    printf("== OPTIGA Trust M — on-chip crypto tour (Phase 6) ==\n");

    /* Trust the local SHA-256 before it forms any digest. */
    if (!sha256_self_check()) {
        printf("SHA-256 self-check: FAIL (NIST vectors) — aborting\n");
        return 1;
    }
    printf("SHA-256 self-check: OK (NIST vectors \"\" and \"abc\")\n\n");

    int rc = pico_optiga_init();
    if (PICO_OPTIGA_OK != rc) {
        printf("init: FAIL — %s\n", pico_optiga_strerror(rc));
        pico_optiga_deinit();
        return 1;
    }
    printf("init: OK\n\n");

    int all_ok = 1;

    /* ---- Part 1: ECDSA keygen / sign / verify ---- */
    printf("--- ECDSA (keygen 0x%04X, sign, verify) ---\n", (unsigned)KEY_OID);
    uint8_t pub[PUB_CAP];
    size_t pub_len = 0;
    /* SIGN (for ECDSA) | KEY_AGREEMENT (for ECDH) — usage is fixed at
     * keygen time, so both must be set up front. */
    rc = pico_optiga_ecc_gen_keypair(KEY_OID, PICO_OPTIGA_CURVE_P256,
                                     PICO_OPTIGA_USAGE_SIGN |
                                         PICO_OPTIGA_USAGE_KEY_AGREEMENT,
                                     pub, PUB_CAP, &pub_len);
    if (PICO_OPTIGA_OK != rc) {
        printf("keygen: FAIL — %s\n", pico_optiga_strerror(rc));
        pico_optiga_deinit();
        return 1;
    }
    printf("keygen: OK — public key (%u bytes)\n", (unsigned)pub_len);

    const char *msg = "pico-optiga Trust M — Phase 6 ECDSA round-trip";
    uint8_t digest[32];
    sha256((const uint8_t *)msg, strlen(msg), digest);

    uint8_t sig[SIG_CAP];
    size_t sig_len = 0;
    rc = pico_optiga_ecdsa_sign(KEY_OID, digest, sizeof(digest), sig, SIG_CAP,
                                &sig_len);
    if (PICO_OPTIGA_OK != rc) {
        printf("sign: FAIL — %s\n", pico_optiga_strerror(rc));
        all_ok = 0;
    } else {
        printf("sign: OK — DER signature (%u bytes)\n", (unsigned)sig_len);
        rc = pico_optiga_ecdsa_verify(digest, sizeof(digest), sig, sig_len, pub,
                                      pub_len, PICO_OPTIGA_CURVE_P256);
        int good = (PICO_OPTIGA_OK == rc);
        printf("verify (good): %s\n", good ? "PASS" : "FAIL (unexpected!)");
        all_ok = all_ok && good;

        uint8_t bad[SIG_CAP];
        memcpy(bad, sig, sig_len);
        bad[sig_len / 2u] ^= 0x01u;
        rc = pico_optiga_ecdsa_verify(digest, sizeof(digest), bad, sig_len, pub,
                                      pub_len, PICO_OPTIGA_CURVE_P256);
        int rejected = (PICO_OPTIGA_OK != rc);
        printf("verify (corrupted): %s\n",
               rejected ? "rejected as expected" : "ACCEPTED (bad!)");
        all_ok = all_ok && rejected;
    }
    printf("\n");

    /* ---- Part 2a: chip SHA-256 cross-check ---- */
    printf("--- SHA-256 offload (chip vs Pico) ---\n");
    uint8_t chip_digest[32];
    rc = pico_optiga_sha256((const uint8_t *)msg, strlen(msg), chip_digest);
    if (PICO_OPTIGA_OK != rc) {
        printf("chip sha256: FAIL — %s\n", pico_optiga_strerror(rc));
        all_ok = 0;
    } else {
        int match = (0 == memcmp(chip_digest, digest, 32u));
        printf("chip sha256 == Pico sha256: %s\n",
               match ? "MATCH" : "MISMATCH (bad!)");
        all_ok = all_ok && match;
    }
    printf("\n");

    /* ---- Part 2b: ECDH + HKDF ---- */
    printf("--- ECDH + HKDF ---\n");
    uint8_t ssec[32];
    rc = pico_optiga_ecdh(KEY_OID, kPeerPub, sizeof(kPeerPub), ssec,
                          sizeof(ssec));
    if (PICO_OPTIGA_OK != rc) {
        printf("ecdh (export): FAIL — %s\n", pico_optiga_strerror(rc));
        all_ok = 0;
    } else {
        printf("ecdh: OK — shared secret (32 bytes):\n  ");
        print_hex(ssec, sizeof(ssec));
    }

    /* ECDH into the chip session, then HKDF from that session secret. */
    rc = pico_optiga_ecdh(KEY_OID, kPeerPub, sizeof(kPeerPub), NULL, 0u);
    if (PICO_OPTIGA_OK != rc) {
        printf("ecdh (session): FAIL — %s\n", pico_optiga_strerror(rc));
        all_ok = 0;
    } else {
        uint8_t dk[32];
        rc = pico_optiga_hkdf(PICO_OPTIGA_KEY_SESSION, kSalt, sizeof(kSalt),
                              kInfo, sizeof(kInfo), dk, sizeof(dk));
        if (PICO_OPTIGA_OK != rc) {
            printf("hkdf: FAIL — %s\n", pico_optiga_strerror(rc));
            all_ok = 0;
        } else {
            printf("hkdf: OK — derived key (32 bytes):\n  ");
            print_hex(dk, sizeof(dk));
        }
    }
    printf("\n");

    /* ---- Part 2c: counter ---- */
    printf("--- Counter (0x%04X) ---\n", (unsigned)COUNTER_OID);
    /* Initialize the counter (count=0, threshold=10) — required before
     * update_count works. A data-payload write to a confirmed-open slot. */
    rc = pico_optiga_write_data_obj(COUNTER_OID, PICO_OPTIGA_ERASE_AND_WRITE,
                                    kCounterInit, sizeof(kCounterInit));
    if (PICO_OPTIGA_OK != rc) {
        printf("counter init: FAIL — %s\n", pico_optiga_strerror(rc));
        all_ok = 0;
    } else {
        printf("counter init: OK\n");
        uint8_t after[8];
        size_t n = 0;
        if (PICO_OPTIGA_OK == pico_optiga_read_data_obj(COUNTER_OID, after,
                                                        sizeof(after), &n)) {
            printf("after init:    ");
            print_hex(after, n);
        }
        rc = pico_optiga_counter_incr(COUNTER_OID, 1u);
        if (PICO_OPTIGA_OK != rc) {
            printf("counter incr: FAIL — %s\n", pico_optiga_strerror(rc));
            all_ok = 0;
        } else {
            n = 0;
            if (PICO_OPTIGA_OK == pico_optiga_read_data_obj(COUNTER_OID, after,
                                                            sizeof(after), &n)) {
                printf("after (inc 1): ");
                print_hex(after, n);
            }
            printf("counter incr: OK\n");
        }
    }
    printf("\n");

    printf("%s\n", all_ok ? "crypto tour: OK" : "crypto tour: FAILED");

    pico_optiga_deinit();
    printf("done\n");
    return all_ok ? 0 : 1;
}
