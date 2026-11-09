/*
 * sha256.h — SHA-256 (FIPS 180-4): small, portable, self-contained.
 *
 * Header-only (static inline) so it can be used from the RP2350 examples and
 * the native host tests with no build-system change. It is the Pico-side
 * SHA-256 used to form the digest the OPTIGA signs/verifies and to
 * cross-check the chip's own SHA-256. It is deliberately a portable software
 * implementation (not the RP2350 hardware SHA-256 block) so it is identical
 * on host and target and unit-testable in tests/test_sha256.c.
 *
 * Verified against the NIST vector set (empty, "abc", 56-byte, and
 * 1,000,000 x 'a') in tests/test_sha256.c.
 */

#ifndef PICO_OPTIGA_SHA256_H
#define PICO_OPTIGA_SHA256_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t  buf[64];
    uint32_t buflen;
} pico_sha256_ctx;

static const uint32_t pico_sha256_k[64] = {
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

static inline uint32_t pico_sha256_rotr(uint32_t x, uint32_t n) {
    return (x >> n) | (x << (32u - n));
}

static inline void pico_sha256_init(pico_sha256_ctx *c) {
    c->state[0] = 0x6a09e667; c->state[1] = 0xbb67ae85;
    c->state[2] = 0x3c6ef372; c->state[3] = 0xa54ff53a;
    c->state[4] = 0x510e527f; c->state[5] = 0x9b05688c;
    c->state[6] = 0x1f83d9ab; c->state[7] = 0x5be0cd19;
    c->bitlen = 0;
    c->buflen = 0;
}

static inline void pico_sha256_transform(pico_sha256_ctx *c, const uint8_t *p) {
    uint32_t w[64];
    for (uint32_t i = 0; i < 16; i++) {
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
    }
    for (uint32_t i = 16; i < 64; i++) {
        uint32_t s0 = pico_sha256_rotr(w[i - 15], 7) ^
                      pico_sha256_rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = pico_sha256_rotr(w[i - 2], 17) ^
                      pico_sha256_rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = c->state[0], b = c->state[1], cc = c->state[2], d = c->state[3];
    uint32_t e = c->state[4], f = c->state[5], g = c->state[6], h = c->state[7];
    for (uint32_t i = 0; i < 64; i++) {
        uint32_t S1 = pico_sha256_rotr(e, 6) ^ pico_sha256_rotr(e, 11) ^
                      pico_sha256_rotr(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + pico_sha256_k[i] + w[i];
        uint32_t S0 = pico_sha256_rotr(a, 2) ^ pico_sha256_rotr(a, 13) ^
                      pico_sha256_rotr(a, 22);
        uint32_t mj = (a & b) ^ (a & cc) ^ (b & cc);
        uint32_t t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->state[0] += a; c->state[1] += b; c->state[2] += cc; c->state[3] += d;
    c->state[4] += e; c->state[5] += f; c->state[6] += g; c->state[7] += h;
}

static inline void pico_sha256_update(pico_sha256_ctx *c, const uint8_t *data,
                                      size_t len) {
    for (size_t i = 0; i < len; i++) {
        c->buf[c->buflen++] = data[i];
        if (c->buflen == 64u) {
            pico_sha256_transform(c, c->buf);
            c->bitlen += 512u;
            c->buflen = 0;
        }
    }
}

static inline void pico_sha256_final(pico_sha256_ctx *c, uint8_t out[32]) {
    uint64_t bits = c->bitlen + (uint64_t)c->buflen * 8u;
    c->buf[c->buflen++] = 0x80;
    if (c->buflen > 56u) {
        while (c->buflen < 64u) {
            c->buf[c->buflen++] = 0;
        }
        pico_sha256_transform(c, c->buf);
        c->buflen = 0;
    }
    while (c->buflen < 56u) {
        c->buf[c->buflen++] = 0;
    }
    for (int i = 7; i >= 0; i--) {
        c->buf[c->buflen++] = (uint8_t)(bits >> (i * 8));
    }
    pico_sha256_transform(c, c->buf);
    for (int i = 0; i < 8; i++) {
        out[i * 4]     = (uint8_t)(c->state[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(c->state[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(c->state[i] >> 8);
        out[i * 4 + 3] = (uint8_t)(c->state[i]);
    }
}

/*
 * One-shot SHA-256: hash `data` (`len` bytes) into `out` (32 bytes).
 */
static inline void pico_sha256(const uint8_t *data, size_t len,
                               uint8_t out[32]) {
    pico_sha256_ctx c;
    pico_sha256_init(&c);
    pico_sha256_update(&c, data, len);
    pico_sha256_final(&c, out);
}

#ifdef __cplusplus
}
#endif

#endif /* PICO_OPTIGA_SHA256_H */
