/*
 * pico_optiga — stable, synchronous C API for the OPTIGA Trust M
 * (Infineon SLS32AIA) on the Raspberry Pi Pico II.
 *
 * Wraps the Infineon OPTIGA Trust M host library (third_party/) and its
 * Pico PAL (./pal) so applications never touch the IFX I2C protocol.
 *
 * Threading: single-core, no OS — NOT thread-safe by design. One chip,
 * one instance, one operation at a time; a second call started while
 * another is in flight returns PICO_OPTIGA_EBUSY.
 *
 * Memory: static contexts only (one optiga_util_t instance, created in
 * pico_optiga_init). No per-call heap allocation.
 *
 * Error handling: flat int codes — 0 (PICO_OPTIGA_OK) on success,
 * negative PICO_OPTIGA_E* on failure. See pico_optiga_strerror().
 */

#ifndef PICO_OPTIGA_OPTIGA_H
#define PICO_OPTIGA_OPTIGA_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------
 * Error codes
 * ------------------------------------------------------------------- */

#define PICO_OPTIGA_OK         (0)    /* success                        */
#define PICO_OPTIGA_EINVAL     (-1)   /* invalid argument               */
#define PICO_OPTIGA_ENOTINIT   (-2)   /* pico_optiga_init() not called  */
#define PICO_OPTIGA_EBUSY      (-3)   /* another operation in flight    */
#define PICO_OPTIGA_ETIMEOUT   (-4)   /* chip did not answer in time    */
#define PICO_OPTIGA_ENODEV     (-5)   /* no chip on the bus (probe)     */
#define PICO_OPTIGA_EIO        (-6)   /* I2C / protocol error           */
#define PICO_OPTIGA_EDEVICE    (-7)   /* OPTIGA device error (0x80xx)   */
#define PICO_OPTIGA_EHANDSHAKE (-8)   /* 0x0107: chip paired to a shielded
                                         connection; unlock required    */

/* Human-readable description of an error code (never NULL). */
const char *pico_optiga_strerror(int err);

/* ---------------------------------------------------------------------
 * Identity
 * ------------------------------------------------------------------- */

/* Coprocessor UID length in bytes (data object 0xE0C2). */
#define PICO_OPTIGA_UID_LEN 27u

/* Chip identity, as read from the common data objects:
 *
 *   0xE0C0  Global Life Cycle Status (1 byte)
 *   0xE0C2  Coprocessor UID (27 bytes)
 *
 * Both have Read AC = ALW, so this works on factory-fresh and
 * pre-paired chips alike. The decoded fields follow the SRM table
 * "Coprocessor UID OPTIGA Trust M Family" (multi-byte integers are
 * little-endian; ESW build is BCD). */
typedef struct pico_optiga_info {
    /* Raw values */
    uint8_t lifecycle;                          /* LcsG; 0x07 = operational */
    uint8_t uid[PICO_OPTIGA_UID_LEN];           /* Coprocessor UID, raw     */

    /* Decoded UID fields */
    uint8_t  cim_id;                            /* CIM identifier          */
    uint8_t  platform_id;                       /* platform identifier     */
    uint8_t  model_id;                          /* model identifier        */
    uint16_t rom_code;                          /* ID of ROM mask          */
    uint8_t  chip_type[6];                      /* chip type               */
    uint8_t  batch_number[6];                   /* batch number            */
    uint16_t wafer_pos_x;                       /* wafer X position        */
    uint16_t wafer_pos_y;                       /* wafer Y position        */
    uint32_t fw_identifier;                     /* firmware identifier     */
    uint8_t  esw_build[2];                      /* ESW build number, BCD   */
} pico_optiga_info_t;

/* ---------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------- */

/*
 * Bring up the chip and open the IFX application:
 *
 *   1. RST pin (GP22) safe bring-up + warm reset (1 ms pulse,
 *      25 ms tSTARTUP settle);
 *   2. I2C0 on GP20 (SDA) / GP21 (SCL) at 100 kHz; the vendor stack
 *      negotiates up to 400 kHz during the open;
 *   3. create the optiga_util instance (static, one per chip);
 *   4. open the application (vendor reset sequence + IFX negotiation).
 *
 * Idempotent: a second call while initialized returns PICO_OPTIGA_OK.
 * On failure the hardware is left in a state where pico_optiga_probe()
 * and pico_optiga_deinit() still work.
 */
int pico_optiga_init(void);

/*
 * Close the application (best effort) and tear down: destroys the
 * optiga_util instance and deinits I2C. Safe to call without a prior
 * init, and after a failed init.
 */
int pico_optiga_deinit(void);

/* ---------------------------------------------------------------------
 * Diagnostics
 * ------------------------------------------------------------------- */

/*
 * Raw I2C probe (no protocol stack): read the IFX I2C registry
 * I2C_STATE register (0x82) from address 0x30 with the datasheet
 * guard-time and retry rules. Returns PICO_OPTIGA_OK if the chip
 * answers with non-0xFF data, PICO_OPTIGA_ENODEV if not.
 *
 * Works after pico_optiga_init() regardless of whether the
 * application open succeeded.
 */
int pico_optiga_probe(void);

/*
 * Read the chip identity (0xE0C0 + 0xE0C2) and decode the UID fields
 * into *info. The application must be open (a successful init).
 */
int pico_optiga_get_info(pico_optiga_info_t *info);

/* ---------------------------------------------------------------------
 * Data objects
 * ------------------------------------------------------------------- */

/*
 * Read `len` bytes from data object `oid` (offset 0) into `buf`.
 * *out_len receives the number of bytes actually read (0 on failure).
 * `buf` must fit at least `len` bytes. The application must be open.
 */
int pico_optiga_read_data_obj(uint16_t oid, uint8_t *buf, size_t len,
                              size_t *out_len);

/*
 * Write modes for pico_optiga_write_data_obj(), mirroring the vendor's
 * OPTIGA_UTIL_WRITE_ONLY / OPTIGA_UTIL_ERASE_AND_WRITE.
 *
 * Both write only the *data payload* — never the object's metadata or
 * life cycle. The difference is what happens to the bytes we don't write:
 */
typedef enum {
    /* Overwrite `len` bytes at offset 0, leaving the rest of the object
     * untouched. One NVM program cycle (cheapest). */
    PICO_OPTIGA_WRITE_ONLY = 0,
    /* Erase the whole object first, then write `len` bytes at offset 0;
     * the remainder is left blank. Costs an extra NVM erase cycle, but
     * leaves the object's contents fully defined. */
    PICO_OPTIGA_ERASE_AND_WRITE = 1,
} pico_optiga_write_mode_t;

/*
 * Write `len` bytes from `buf` into data object `oid` at offset 0.
 * `mode` selects WRITE_ONLY (overwrite in place) or ERASE_AND_WRITE
 * (erase the object, then write). The application must be open.
 *
 * This changes only the object's *data* — never its metadata (access
 * conditions) or life cycle. It is safe and repeatable, bounded only by
 * NVM endurance (~1e5 cycles), so keep the write count low.
 */
int pico_optiga_write_data_obj(uint16_t oid, pico_optiga_write_mode_t mode,
                               const uint8_t *buf, size_t len);

/*
 * Read the TLV metadata of data object `oid` into `buf` (at most `len`
 * bytes). *out_len receives the number of bytes read (0 on failure).
 *
 * The metadata describes the object's access conditions (Read/Change/
 * Execute), its life cycle state (LcsO), and its max/used size. This is
 * a pure read — it never modifies the chip. The application must be open.
 *
 * NOTE: writing metadata is a different, far more dangerous operation
 * (it can lock an object's access conditions or advance its life cycle
 * irreversibly). This library deliberately exposes no metadata-write API.
 */
int pico_optiga_read_metadata(uint16_t oid, uint8_t *buf, size_t len,
                              size_t *out_len);

/* ---------------------------------------------------------------------
 * Random
 * ------------------------------------------------------------------- */

/*
 * Generate `len` bytes of true random from the chip's hardware TRNG into
 * `buf`. `len` must be 8..256 bytes (the chip's supported range). This is
 * a pure read — it never modifies the chip. The application must be open.
 *
 * Uses the true (hardware) random number generator, so the output is
 * suitable for keys, nonces, and other secrets.
 */
int pico_optiga_get_random(uint8_t *buf, size_t len);

/* ---------------------------------------------------------------------
 * Crypto (ECC / ECDSA)
 * ------------------------------------------------------------------- */

/* ECC curves for pico_optiga_ecc_gen_keypair() / pico_optiga_ecdsa_verify().
 * The value is the vendor's optiga_ecc_curve_t; this project needs P-256. */
#define PICO_OPTIGA_CURVE_P256  0x03u  /* NIST P-256 (secp256r1) */

/* Key usage for pico_optiga_ecc_gen_keypair(). The value is the vendor's
 * optiga_key_usage_t. */
#define PICO_OPTIGA_USAGE_SIGN  0x10u  /* private key may sign (ECDSA) */

/*
 * Generate an ECC key pair on the chip into key object `oid` (e.g. 0xE0F2).
 * `curve` is the curve (PICO_OPTIGA_CURVE_P256); `usage` the key usage
 * (PICO_OPTIGA_USAGE_SIGN). The PRIVATE key stays on the chip (never
 * exported); the PUBLIC key is written into `pub` (at most `pub_cap` bytes)
 * and its actual length returned in *out_pub_len.
 *
 * This writes key material to a slot's intended purpose and may advance the
 * slot's life cycle creation -> operational (the normal "in service" state).
 * It does NOT write metadata/access conditions and never moves toward
 * termination. `oid` must be a key object whose Change AC is already open
 * (see the metadata inventory). The application must be open.
 */
int pico_optiga_ecc_gen_keypair(uint16_t oid, uint8_t curve, uint8_t usage,
                                uint8_t *pub, size_t pub_cap, size_t *out_pub_len);

/*
 * Sign a message digest on the chip with the private key in `oid`. `digest`
 * (`digest_len` bytes, e.g. a 32-byte SHA-256) is signed and the
 * DER-encoded ECDSA signature is written into `sig` (at most `sig_cap`
 * bytes); its length is returned in *out_sig_len. The private key never
 * leaves the chip. The application must be open.
 */
int pico_optiga_ecdsa_sign(uint16_t oid, const uint8_t *digest,
                           size_t digest_len, uint8_t *sig, size_t sig_cap,
                           size_t *out_sig_len);

/*
 * Verify a DER-encoded ECDSA `sig` (`sig_len` bytes) over `digest`
 * (`digest_len` bytes) using the host-provided ECC `pub` (`pub_len` bytes —
 * the same key a keygen returned). `curve` is the curve
 * (PICO_OPTIGA_CURVE_P256). Returns PICO_OPTIGA_OK if the signature is valid,
 * a PICO_OPTIGA_E* code if it is not. The application must be open.
 */
int pico_optiga_ecdsa_verify(const uint8_t *digest, size_t digest_len,
                             const uint8_t *sig, size_t sig_len,
                             const uint8_t *pub, size_t pub_len,
                             uint8_t curve);

#ifdef __cplusplus
}
#endif

#endif /* PICO_OPTIGA_OPTIGA_H */
