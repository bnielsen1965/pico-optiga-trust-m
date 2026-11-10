/*
 * test_vault_kdf.c - host tests for the vault's entry-key parameterization.
 *
 * The real entry key is derived on the chip:
 *     session  = ECDH(0xE0F2, kPeerPub)
 *     entrykey = HKDF(session, salt || PIN, info = name)
 * which cannot run on the host. This test stands in for that KDF with a
 * deterministic host function of the SAME three inputs -- SHA-256(salt ||
 * PIN || name) -- which has the one property the vault depends on: it is a
 * fixed 32-byte function of (salt, PIN, name). The GCM (test_vault_gcm.c,
 * NIST-verified) is the vehicle here, not the subject.
 *
 * What is verified (the vault's add/get glue, from examples/vault/main.c):
 *   - the entry record is exactly 96 bytes (mirrors main.c's vault_entry_t);
 *   - the entry key is DETERMINISTIC in (salt, PIN, name) -- the same inputs
 *     always give the same key, so `get` can re-derive what `add` used;
 *   - each of salt / PIN / name is BOUND into the key: changing any one
 *     changes the key, so the GCM tag no longer verifies;
 *   - a wrong PIN is rejected (the GCM tag fails);
 *   - a swapped name is rejected (the name is bound twice: HKDF info + AAD);
 *   - the record's magic / length / CRC-32 integrity checks reject corruption.
 *
 * The chip-specific parts (the ECDH, the HKDF, the on-chip master) are
 * bench-verified on the hardware (Phase 6 crypt tour + the working vault);
 * this test pins down the host-side logic that surrounds them.
 */

#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "vault_gcm.h"
#include "pico_optiga/sha256.h"

// --- vault geometry (MUST mirror examples/vault/main.c) ----------------------

#define VAULT_SALT_LEN   16
#define VAULT_NONCE_LEN  12
#define VAULT_NAME_MAX   16
#define VAULT_PIN_MAX    24
#define VAULT_PASS_MAX   40
#define VAULT_MAGIC      0x31554156u     // 'V''A''U''1'

typedef struct {
    uint32_t magic;
    uint8_t  salt[VAULT_SALT_LEN];
    uint8_t  nonce[VAULT_NONCE_LEN];
    uint32_t pt_len;
    uint8_t  ct[VAULT_PASS_MAX];
    uint8_t  tag[VAULT_GCM_TAG_LEN];
    uint32_t crc32;                       // over all preceding bytes
} vault_entry_t;

// The record format is a wire/flash contract with main.c; a size drift here is
// a real bug, so fail the build if the two ever diverge.
_Static_assert(sizeof(vault_entry_t) == 96, "vault_entry_t must be 96 bytes");

static int s_failures = 0;

static void check(bool ok, const char *what)
{
    printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) {
        s_failures++;
    }
}

// Standard reflected CRC-32 (same polynomial as main.c / fs_audit).
static uint32_t crc32(const uint8_t *buf, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int)(crc & 1u)));
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

// Stand-in for the chip's HKDF(session, salt||PIN, info=name): a deterministic
// 32-byte function of the same three inputs. SHA-256 over salt || PIN || name.
static void test_derive_key(const uint8_t salt[VAULT_SALT_LEN],
                            const char *pin, size_t pin_len,
                            const char *name,
                            uint8_t key[VAULT_GCM_KEY_LEN])
{
    pico_sha256_ctx ctx;
    pico_sha256_init(&ctx);
    pico_sha256_update(&ctx, salt, VAULT_SALT_LEN);
    pico_sha256_update(&ctx, (const uint8_t *)pin, pin_len);
    pico_sha256_update(&ctx, (const uint8_t *)name, strlen(name));
    pico_sha256_final(&ctx, key);
}

// The vault's `add` glue (minus the chip RNG + littlefs write): derive the key,
// GCM-encrypt the password binding the name as AAD, then seal the record CRC.
static void test_add(const uint8_t salt[VAULT_SALT_LEN],
                     const uint8_t nonce[VAULT_NONCE_LEN],
                     const char *name, const char *pin, size_t pin_len,
                     const char *pass, size_t pass_len,
                     vault_entry_t *e)
{
    memset(e, 0, sizeof(*e));
    e->magic  = VAULT_MAGIC;
    memcpy(e->salt, salt, VAULT_SALT_LEN);
    memcpy(e->nonce, nonce, VAULT_NONCE_LEN);
    e->pt_len = (uint32_t)pass_len;

    uint8_t key[VAULT_GCM_KEY_LEN];
    test_derive_key(e->salt, pin, pin_len, name, key);
    (void)vault_gcm_encrypt(key, e->nonce, (const uint8_t *)name, strlen(name),
                            (const uint8_t *)pass, pass_len,
                            e->ct, sizeof(e->ct), e->tag);
    e->crc32 = crc32((const uint8_t *)e, offsetof(vault_entry_t, crc32));
}

// The vault's `get` glue (minus the littlefs read): integrity checks in the
// same order main.c uses, then re-derive the key and GCM-decrypt.
static bool test_get(const vault_entry_t *e, const char *name,
                     const char *pin, size_t pin_len,
                     char *pass_out, size_t pass_cap)
{
    if (VAULT_MAGIC != e->magic) {
        return false;
    }
    if (e->pt_len > VAULT_PASS_MAX) {
        return false;
    }
    if (crc32((const uint8_t *)e, offsetof(vault_entry_t, crc32)) != e->crc32) {
        return false;
    }
    uint8_t key[VAULT_GCM_KEY_LEN];
    test_derive_key(e->salt, pin, pin_len, name, key);
    return vault_gcm_decrypt(key, e->nonce, (const uint8_t *)name, strlen(name),
                             e->ct, e->pt_len, e->tag,
                             (uint8_t *)pass_out, pass_cap);
}

int main(void)
{
    printf("test_vault_kdf: vault entry-key parameterization\n\n");

    uint8_t saltA[VAULT_SALT_LEN], saltB[VAULT_SALT_LEN];
    uint8_t nonce[VAULT_NONCE_LEN];
    for (int i = 0; i < VAULT_SALT_LEN; i++) {
        saltA[i] = (uint8_t)(0xA0 + i);
        saltB[i] = (uint8_t)(0xB0 + i);
        if (i < VAULT_NONCE_LEN) {
            nonce[i] = (uint8_t)(0x0F + i);
        }
    }
    const char *name = "github";
    const char *pin  = "1234";
    const char *pass = "secret-password";

    // --- determinism: same (salt, PIN, name) -> identical key ---------------
    uint8_t k1[VAULT_GCM_KEY_LEN], k2[VAULT_GCM_KEY_LEN];
    test_derive_key(saltA, pin, strlen(pin), name, k1);
    test_derive_key(saltA, pin, strlen(pin), name, k2);
    check(0 == memcmp(k1, k2, sizeof(k1)),
          "deterministic: same (salt,PIN,name) -> same entry key");

    // --- each input is bound into the key -----------------------------------
    test_derive_key(saltA, "9999", 4, name, k2);
    check(0 != memcmp(k1, k2, sizeof(k1)), "PIN is bound: different PIN -> different key");
    test_derive_key(saltB, pin, strlen(pin), name, k2);
    check(0 != memcmp(k1, k2, sizeof(k1)), "salt is bound: different salt -> different key");
    test_derive_key(saltA, pin, strlen(pin), "gitlab", k2);
    check(0 != memcmp(k1, k2, sizeof(k1)), "name is bound: different name -> different key");

    // --- round-trip: add then get recovers the password ---------------------
    vault_entry_t e;
    test_add(saltA, nonce, name, pin, strlen(pin), pass, strlen(pass), &e);
    char out[VAULT_PASS_MAX + 1];
    bool got = test_get(&e, name, pin, strlen(pin), out, sizeof(out));
    check(got && (0 == strcmp(out, pass)), "round-trip: add -> get recovers the password");

    // --- wrong PIN is rejected ----------------------------------------------
    got = test_get(&e, name, "9999", 4, out, sizeof(out));
    check(!got, "wrong PIN rejected (GCM tag fails)");

    // --- swapped name is rejected (name bound as key info AND AAD) ----------
    got = test_get(&e, "gitlab", pin, strlen(pin), out, sizeof(out));
    check(!got, "swapped name rejected (name bound as HKDF info + GCM AAD)");

    // --- salt binding isolates the key (same everything, salt differs) ------
    vault_entry_t eA, eB;
    test_add(saltA, nonce, name, pin, strlen(pin), pass, strlen(pass), &eA);
    test_add(saltB, nonce, name, pin, strlen(pin), pass, strlen(pass), &eB);
    check(0 != memcmp(eA.ct, eB.ct, 16),
          "salt changes the ciphertext (same PIN/name/pass, different salt)");

    // --- integrity checks reject corruption ---------------------------------
    vault_entry_t c = e;
    c.ct[0] ^= 0x01u;                 // flip a byte inside the CRC-covered region
    check(!test_get(&c, name, pin, strlen(pin), out, sizeof(out)),
          "corrupt ciphertext rejected (CRC-32 mismatch)");

    c = e;
    c.magic = 0u;
    check(!test_get(&c, name, pin, strlen(pin), out, sizeof(out)),
          "bad magic rejected");

    c = e;
    c.pt_len = VAULT_PASS_MAX + 1u;
    check(!test_get(&c, name, pin, strlen(pin), out, sizeof(out)),
          "oversized pt_len rejected");

    printf("\n%s (%d failure%s)\n",
           (0 == s_failures) ? "ALL PASS" : "FAILURES",
           s_failures, (1 == s_failures) ? "" : "s");
    return (0 == s_failures) ? 0 : 1;
}
