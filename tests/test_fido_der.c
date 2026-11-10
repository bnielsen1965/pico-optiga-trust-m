/*
 * test_fido_der.c - host known-answer + round-trip tests for the DER ECDSA
 * signature <-> raw r||s conversion (examples/fido2/fido_der.c).
 *
 * The conversion is the one genuinely new bit of crypto plumbing for the
 * OPTIGA FIDO2 backend: the chip signs in DER, CTAP2 wants raw 64-byte r||s.
 * Vectors are hand-constructed and cross-checked; round-trips cover the
 * leading-zero-strip and 0x00 high-bit-pad edges.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "fido_der.h"

static int s_failures = 0;

static void check(bool ok, const char *what)
{
    printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) {
        s_failures++;
    }
}

int main(void)
{
    printf("test_fido_der: DER ECDSA sig <-> raw r||s\n\n");

    // --- known answer: r = 1, s = p-1 (0xFF..FE) ---------------------------
    // DER: 30 26 02 01 01 02 21 00 FF..FE   (r is 1 byte; s needs the 0x00 pad)
    static const uint8_t der[] = {
        0x30, 0x26,
        0x02, 0x01, 0x01,
        0x02, 0x21, 0x00,
        0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
        0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFE,
    };
    uint8_t raw[FIDO_DER_RS_LEN];
    const bool ok = fido_der_to_raw_rs(der, sizeof(der), raw);
    check(ok, "known-answer DER parses");
    check((0 == raw[0]) && (1 == raw[31]), "known-answer r = 1 (32-byte LE-aligned)");
    bool s_ok = true;
    for (int i = 0; i < 31; i++) {
        if (0xFF != raw[32 + i]) {
            s_ok = false;
        }
    }
    check(s_ok && (0xFE == raw[63]), "known-answer s = p-1 (0x00 pad stripped)");

    // --- round trips --------------------------------------------------------
    // A full-strength pair: both r and s have the high bit set (force the pad
    // on encode, force the strip on decode).
    uint8_t rt[FIDO_DER_RS_LEN];
    for (int i = 0; i < 64; i++) {
        rt[i] = (uint8_t)(0xA5 + i);
    }
    rt[0] = 0x81;          // r high bit set
    rt[32] = 0x82;         // s high bit set
    uint8_t der_rt[80];
    size_t der_rt_len = fido_raw_rs_to_der(rt, der_rt, sizeof(der_rt));
    uint8_t back[FIDO_DER_RS_LEN];
    check((der_rt_len > 0) && fido_der_to_raw_rs(der_rt, der_rt_len, back)
          && (0 == memcmp(rt, back, 64)),
          "round-trip: high-bit r and s (pad + strip)");

    // Small values: r and s fit in one byte (max leading-zero strip).
    uint8_t small[FIDO_DER_RS_LEN];
    memset(small, 0, sizeof(small));
    small[31] = 0x2A;      // r = 0x2A
    small[63] = 0x3C;      // s = 0x3C
    der_rt_len = fido_raw_rs_to_der(small, der_rt, sizeof(der_rt));
    check((der_rt_len > 0) && fido_der_to_raw_rs(der_rt, der_rt_len, back)
          && (0 == memcmp(small, back, 64)),
          "round-trip: single-byte r and s");

    // --- malformed inputs are rejected --------------------------------------
    uint8_t bad[FIDO_DER_RS_LEN];
    memset(bad, 0, sizeof(bad));
    check(!fido_der_to_raw_rs(der, 4, bad), "too-short DER rejected");
    uint8_t notseq[40];
    memcpy(notseq, der, sizeof(der));
    notseq[0] = 0x31;      // not a SEQUENCE
    check(!fido_der_to_raw_rs(notseq, sizeof(notseq), bad), "non-SEQUENCE rejected");
    uint8_t longr[64];
    memcpy(longr, der, sizeof(der));
    longr[3] = 0x22;       // r length 34 (> 33) — the r length byte is index 3
    check(!fido_der_to_raw_rs(longr, sizeof(longr), bad), "oversized r rejected");

    printf("\n%s (%d failure%s)\n",
           (0 == s_failures) ? "ALL PASS" : "FAILURES",
           s_failures, (1 == s_failures) ? "" : "s");
    return (0 == s_failures) ? 0 : 1;
}
