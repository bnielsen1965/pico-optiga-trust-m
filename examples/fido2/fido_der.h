/*
 * fido_der.h - DER ECDSA signature -> raw r||s (the CTAP2 wire form).
 *
 * The OPTIGA's ecdsa_sign returns a DER-encoded signature
 * (SEQUENCE { INTEGER r, INTEGER s }); CTAP2 / WebAuthn want the raw
 * 64-byte form r||s, each a 32-byte big-endian integer. This converts
 * between the two. Pure C, no chip, host-tested (tests/test_fido_der.c).
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FIDO_DER_RS_LEN 64

// Decode a DER ECDSA signature into raw r||s. Returns false if the DER is
// malformed for P-256 (bad tags, an r/s longer than 33 bytes, truncation).
bool fido_der_to_raw_rs(const uint8_t *der, size_t der_len, uint8_t raw[FIDO_DER_RS_LEN]);

// Encode a raw 64-byte r||s into a DER ECDSA signature. Writes the DER into
// out (at most out_cap bytes); returns the length written, 0 on failure (out_cap
// too small, or an r/s with a non-zero high bit that would need > 1 pad byte).
size_t fido_raw_rs_to_der(const uint8_t raw[FIDO_DER_RS_LEN], uint8_t *out, size_t out_cap);
