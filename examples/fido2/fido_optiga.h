/*
 * fido_optiga.h - the OPTIGA Trust M backend for the FIDO2 CTAP layer.
 *
 * Implements fido_ctap_key_ops_t (fido_ctap.h) for the one-key, non-resident
 * model we agreed on:
 *
 *   - ONE P-256 key on 0xE0F3, shared by every credential AND the attestation.
 *     The private key is generated on-chip and never leaves; only the 64-byte
 *     public key is cached in littlefs (the OPTIGA has no get-public-key
 *     command — the public key exists only at keygen time).
 *   - A littlefs sync counter (/f2_cnt, 4-byte big-endian) — resettable, so
 *     RESET zeroes it cleanly.
 *   - The chip TRNG for random bytes; pico-sdk for the clock.
 *
 * The CTAP layer (fido_ctap.c) is chip-agnostic: it calls these hooks and
 * never names a slot, so "slot" arguments are ignored (everything is 0xE0F3).
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "fido_ctap.h"

// The single FIDO2 key slot (0xE0F2 is the Vault master; 0xE0F3 is FIDO2's).
#define FIDO_OPTIGA_KEY_OID    0xE0F3u
// littlefs objects on the shared volume (flash @ 0x80000):
#define FIDO_OPTIGA_PUB_PATH   "/f2_idkey.pub"   // 64-byte cached public key
#define FIDO_OPTIGA_CNT_PATH   "/f2_cnt"         // 4-byte big-endian counter

// Install the OPTIGA key-ops into *keys (the CTAP context's backend). `dev`
// is set to NULL — the module is a process global, so the hooks ignore it.
void fido_optiga_install_key_ops(fido_ctap_key_ops_t *keys);

// One-time master keygen: generate the P-256 keypair into 0xE0F3 (USAGE_SIGN)
// and cache its 64-byte public key in littlefs. Returns 0 on success. This is
// the app's `provision` — the CTAP layer never calls it per-credential.
int fido_optiga_provision(void);

// True if the master is provisioned (0xE0F3 keyed AND the pub-key cache is a
// well-formed 64 bytes). Read-only.
bool fido_optiga_is_provisioned(void);
