/*
 * crypt — Phase 6, Step 1a: ECDSA core (keygen / sign / verify).
 *
 * Exercises the three new library wrappers end to end on the chip:
 *
 *   1. pico_optiga_ecc_gen_keypair()  — generate a NIST P-256 key pair into
 *      key object 0xE0F2. The private key stays on the chip; only the public
 *      key is returned to the host.
 *   2. pico_optiga_ecdsa_sign()       — sign a SHA-256 digest on the chip with
 *      the on-chip private key (DER-encoded signature comes back).
 *   3. pico_optiga_ecdsa_verify()     — verify that signature with the public
 *      key (must pass), then verify a corrupted copy (must fail).
 *
 * SAFETY — 0xE0F2 was confirmed by the Step 0 inventory to be a safe keygen
 * target: LcsO=creation, Change AC = LcsO<0x07 (already open), so keygen
 * needs no metadata (AC-tag) write. The keygen writes key material to the
 * slot's intended purpose and may advance the slot creation -> operational
 * (the normal "in service" state); it never writes an AC tag and never moves
 * toward termination.
 *
 * SHA-256 is computed on the Pico (a small, portable, self-contained
 * implementation below — no hardware dependency, host-testable). It is
 * verified against two NIST test vectors before any digest is trusted, so a
 * regression would abort the example rather than sign a wrong digest.
 */

#include <pico/stdlib.h>
#include <stdio.h>
#include <string.h>

#include "pico_optiga/optiga.h"

/* The confirmed-safe ECC keygen target (Step 0 inventory). */
#define KEY_OID      0xE0F2u
#define PUB_CAP      128u
#define SIG_CAP      128u

/* =====================================================================
 * SHA-256 (FIPS 180-4) — small, portable, self-contained.
 *
 * Used only to form the digest the chip signs/verifies. It is checked
 * against two NIST vectors in main() before use.
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

    printf("== OPTIGA Trust M — ECDSA keygen/sign/verify (Step 1a) ==\n");

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

    /* 1. Keygen a P-256 pair into 0xE0F2 (private stays on chip). */
    uint8_t pub[PUB_CAP];
    size_t pub_len = 0;
    rc = pico_optiga_ecc_gen_keypair(KEY_OID, PICO_OPTIGA_CURVE_P256,
                                     PICO_OPTIGA_USAGE_SIGN, pub, PUB_CAP,
                                     &pub_len);
    if (PICO_OPTIGA_OK != rc) {
        printf("keygen: FAIL — %s\n", pico_optiga_strerror(rc));
        pico_optiga_deinit();
        return 1;
    }
    printf("keygen 0x%04X: OK — public key (%u bytes):\n  ",
           (unsigned)KEY_OID, (unsigned)pub_len);
    print_hex(pub, pub_len);

    /* 2. Digest a fixed message on the Pico, sign it on the chip. */
    const char *msg = "pico-optiga Trust M — Phase 6 Step 1a ECDSA round-trip";
    uint8_t digest[32];
    sha256((const uint8_t *)msg, strlen(msg), digest);
    printf("SHA-256 of message (%u bytes):\n  ", (unsigned)strlen(msg));
    print_hex(digest, sizeof(digest));

    uint8_t sig[SIG_CAP];
    size_t sig_len = 0;
    rc = pico_optiga_ecdsa_sign(KEY_OID, digest, sizeof(digest), sig, SIG_CAP,
                                &sig_len);
    if (PICO_OPTIGA_OK != rc) {
        printf("sign: FAIL — %s\n", pico_optiga_strerror(rc));
        pico_optiga_deinit();
        return 1;
    }
    printf("sign: OK — DER signature (%u bytes):\n  ", (unsigned)sig_len);
    print_hex(sig, sig_len);

    /* 3. Verify the signature with the public key (must pass). */
    int good_ok = 0;
    rc = pico_optiga_ecdsa_verify(digest, sizeof(digest), sig, sig_len, pub,
                                  pub_len, PICO_OPTIGA_CURVE_P256);
    good_ok = (PICO_OPTIGA_OK == rc);
    printf("verify (good signature): %s\n",
           good_ok ? "PASS" : "FAIL (unexpected!)");

    /* 4. Negative test: corrupt one signature byte, verify must fail. */
    uint8_t badsig[SIG_CAP];
    memcpy(badsig, sig, sig_len);
    badsig[sig_len / 2u] ^= 0x01u;
    int bad_rejected = 1;
    rc = pico_optiga_ecdsa_verify(digest, sizeof(digest), badsig, sig_len, pub,
                                  pub_len, PICO_OPTIGA_CURVE_P256);
    bad_rejected = (PICO_OPTIGA_OK != rc);
    printf("verify (corrupted signature): %s\n",
           bad_rejected ? "rejected as expected" : "PASS (unexpected!)");

    int ok = good_ok && bad_rejected;
    printf("\n%s\n", ok ? "ECDSA round-trip: OK"
                        : "ECDSA round-trip: FAILED");

    pico_optiga_deinit();
    printf("done\n");
    return ok ? 0 : 1;
}
