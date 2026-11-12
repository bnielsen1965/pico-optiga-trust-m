// fido_optiga.c - the OPTIGA Trust M FIDO2 key backend (see fido_optiga.h).
//
// Single-core, no-OS: every hook runs on the card task (core 0). The littlefs
// counter and pub-key cache go through pico_lfs (flash_safe_execute), which is
// safe from core 0. The chip calls go through the pico_optiga module (a global
// instance opened by pico_optiga_init in main).

#include "fido_optiga.h"

#include <string.h>

#include "pico/stdlib.h"
#include "pico_littlefs.h"

#include "pico_optiga/optiga.h"
#include "fido_der.h"

// The OPTIGA's ECC public-key prefix (03 42 00 04) that precedes X||Y.
#define FIDO_OPTIGA_PUB_HEADER 4u
// A P-256 public key as the keygen returns it: 4-byte header + X(32) + Y(32).
#define FIDO_OPTIGA_PUB_FULL   68u

// --- the littlefs sync counter (/f2_cnt, 4-byte big-endian) -----------------
// Resettable: RESET and a console wipe zero it. A missing/corrupt file reads
// as 0. The card tick is the only caller, so no locking is needed.

static uint32_t cnt_read(void)
{
    uint8_t b[4];
    if (4 != pico_lfs_read_file(FIDO_OPTIGA_CNT_PATH, b, sizeof b)) {
        return 0;
    }
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
           ((uint32_t)b[2] << 8) | (uint32_t)b[3];
}

static int cnt_write(uint32_t v)
{
    const uint8_t b[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16),
                           (uint8_t)(v >> 8), (uint8_t)v };
    return pico_lfs_write_file(FIDO_OPTIGA_CNT_PATH, b, sizeof b);
}

// --- fido_ctap_key_ops_t hooks ----------------------------------------------

// Key-once: the single P-256 keypair lives in 0xE0F3 and is generated exactly
// once. The CTAP layer calls genkey on every MakeCredential (mc_create), so
// this must NOT re-key: if the public-key cache exists, return it and leave
// the chip alone — re-keying would change the public key and invalidate every
// existing credential. Only when the cache is absent (first boot) does it
// generate the keypair and cache the public key (the chip has no
// get-public-key command, so the littlefs cache is the only copy of the public
// half). pub = X(32)||Y(32).
static int be_genkey(void *dev, uint8_t slot, uint8_t pub[64])
{
    (void)dev;
    (void)slot;
    if (64 == pico_lfs_read_file(FIDO_OPTIGA_PUB_PATH, pub, 64)) {
        return 0;   // already keyed: return the cached public key
    }
    uint8_t full[FIDO_OPTIGA_PUB_FULL];
    size_t full_len = 0;
    int rc = pico_optiga_ecc_gen_keypair(FIDO_OPTIGA_KEY_OID,
                                         PICO_OPTIGA_CURVE_P256,
                                         PICO_OPTIGA_USAGE_SIGN,
                                         full, sizeof(full), &full_len);
    if (PICO_OPTIGA_OK != rc) {
        return -1;
    }
    if (full_len < FIDO_OPTIGA_PUB_FULL) {
        return -1;
    }
    memcpy(pub, full + FIDO_OPTIGA_PUB_HEADER, 64);
    // Cache the public key so the key stays stable across future calls.
    return pico_lfs_write_file(FIDO_OPTIGA_PUB_PATH, pub, 64);
}

// The public key of 0xE0F3, from the littlefs cache (the chip has no
// get-public-key command). This is where the assertion authData's COSE_Key
// and the MakeCredential publicKey come from.
static int be_get_pubkey(void *dev, uint8_t slot, uint8_t pub[64])
{
    (void)dev;
    (void)slot;
    return (64 == pico_lfs_read_file(FIDO_OPTIGA_PUB_PATH, pub, 64)) ? 0 : -1;
}

// Sign a 32-byte digest with 0xE0F3; sig = 64-byte r||s (DER converted to raw).
static int be_sign(void *dev, uint8_t slot, const uint8_t digest[32], uint8_t sig[64])
{
    (void)dev;
    (void)slot;
    uint8_t der[128];
    size_t der_len = 0;
    int rc = pico_optiga_ecdsa_sign(FIDO_OPTIGA_KEY_OID, digest, 32u,
                                    der, sizeof(der), &der_len);
    if (PICO_OPTIGA_OK != rc) {
        return -1;
    }
    return fido_der_to_raw_rs(der, der_len, sig) ? 0 : -1;
}

static uint32_t be_counter(void *dev)
{
    (void)dev;
    return cnt_read();
}

static int be_counter_inc(void *dev, uint32_t *new_value)
{
    (void)dev;
    const uint32_t v = cnt_read() + 1u;    // wraps at 2^32 (the WebAuthn counter)
    *new_value = v;
    return cnt_write(v);
}

static int be_counter_zero(void *dev)
{
    (void)dev;
    return cnt_write(0);
}

static int be_rand(void *dev, uint8_t *buf, uint32_t len)
{
    (void)dev;
    return (PICO_OPTIGA_OK == pico_optiga_get_random(buf, (size_t)len)) ? 0 : -1;
}

static uint32_t be_clock(void *dev)
{
    (void)dev;
    // Uptime in ms (the CTAP RESET 10-s window, 2.3 §6.6). to_ms_since_boot
    // CONVERTS a timestamp to ms-since-boot, so the argument must be the
    // current absolute time — to_ms_since_boot(0) is always 0 and would make
    // the window check a no-op. Matches the reference's osal_time_millis().
    return to_ms_since_boot(get_absolute_time());
}

// --- public API --------------------------------------------------------------

void fido_optiga_install_key_ops(fido_ctap_key_ops_t *keys)
{
    keys->genkey       = be_genkey;
    keys->get_pubkey   = be_get_pubkey;
    keys->sign         = be_sign;
    keys->counter      = be_counter;
    keys->counter_inc  = be_counter_inc;
    keys->counter_zero = be_counter_zero;
    keys->rand         = be_rand;
    keys->clock        = be_clock;
    keys->dev          = NULL;
}

int fido_optiga_provision(void)
{
    // be_genkey is key-once: it generates 0xE0F3 and caches the public key
    // only when the cache is absent, and is a safe no-op when already
    // provisioned (it never re-keys, so it cannot invalidate credentials).
    uint8_t pub[64];
    return be_genkey(NULL, 0u, pub);
}

bool fido_optiga_is_provisioned(void)
{
    uint8_t pub[64];
    return (64 == pico_lfs_read_file(FIDO_OPTIGA_PUB_PATH, pub, sizeof(pub)));
}
