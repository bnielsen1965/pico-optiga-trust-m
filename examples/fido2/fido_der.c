// fido_der.c - DER ECDSA signature <-> raw r||s (see fido_der.h).
//
// P-256 signatures: r and s are 256-bit integers, so each is 32 bytes, or 33
// bytes in DER when the high bit is set (a leading 0x00 is prepended to keep
// the INTEGER positive). The raw CTAP2 form is always exactly 32 bytes each.

#include "fido_der.h"

#include <string.h>

// Right-align a little-endian-ish big integer of length `len` (1..33) into a
// 32-byte field: the low 32 bytes are kept, anything above is dropped (that is
// the DER 0x00 high-byte pad), and a short value is zero-padded on the left.
static void align32(const uint8_t *src, size_t len, uint8_t out[32])
{
    memset(out, 0, 32);
    if (len > 32) {
        src++;                    // 33 bytes = a 0x00 high-byte pad + 32: drop the pad
        len = 32;
    }
    memcpy(out + (32 - len), src, len);
}

bool fido_der_to_raw_rs(const uint8_t *der, size_t der_len, uint8_t raw[FIDO_DER_RS_LEN])
{
    size_t i = 0;
    if (der_len < 8u || der[i++] != 0x30u) {
        return false;             // SEQUENCE
    }
    // SEQUENCE length: short form (P-256 sigs are < 128 bytes) or long form.
    if (der[i] & 0x80u) {
        const size_t n = der[i] & 0x7fu;
        if ((n < 1) || (n > 2) || ((i + 1u + n) > der_len)) {
            return false;
        }
        i += 1u + n;
    } else {
        i += 1u;
    }
    // INTEGER r
    if (((i + 2u) > der_len) || (der[i++] != 0x02u)) {
        return false;
    }
    const size_t rlen = der[i++];
    if ((rlen < 1u) || (rlen > 33u) || ((i + rlen) > der_len)) {
        return false;
    }
    const uint8_t *rp = der + i;
    i += rlen;
    // INTEGER s
    if (((i + 2u) > der_len) || (der[i++] != 0x02u)) {
        return false;
    }
    const size_t slen = der[i++];
    if ((slen < 1u) || (slen > 33u) || ((i + slen) > der_len)) {
        return false;
    }
    const uint8_t *sp = der + i;
    i += slen;

    align32(rp, rlen, raw);              // r -> raw[0..31]
    align32(sp, slen, raw + 32);         // s -> raw[32..63]
    return true;
}

// Encode one integer (the low 32 bytes of a 32-byte field) as a DER INTEGER:
// strip leading zero bytes, and prepend a 0x00 if the new high bit is set.
// Returns the number of bytes written (2 + content).
static size_t encode_integer(const uint8_t v[32], uint8_t *out)
{
    size_t k = 0;
    while ((k < 31u) && (v[k] == 0u)) {
        k++;                            // strip leading zeros (keep >= 1 byte)
    }
    const size_t content = 32u - k;
    const bool need_pad = (v[k] & 0x80u) != 0u;
    out[0] = 0x02u;
    out[1] = (uint8_t)(content + (need_pad ? 1u : 0u));
    size_t o = 2u;
    if (need_pad) {
        out[o++] = 0x00u;
    }
    memcpy(out + o, v + k, content);
    o += content;
    return o;
}

size_t fido_raw_rs_to_der(const uint8_t raw[FIDO_DER_RS_LEN], uint8_t *out, size_t out_cap)
{
    if (NULL == out) {
        return 0;
    }
    uint8_t rb[64], sb[64];
    const size_t rlen = encode_integer(raw, rb);
    const size_t slen = encode_integer(raw + 32, sb);
    const size_t body = rlen + slen;
    if ((body + 2u) > out_cap) {
        return 0;                       // not enough room
    }
    out[0] = 0x30u;
    out[1] = (uint8_t)body;             // P-256 body is always < 128
    memcpy(out + 2, rb, rlen);
    memcpy(out + 2 + rlen, sb, slen);
    return 2u + body;
}
