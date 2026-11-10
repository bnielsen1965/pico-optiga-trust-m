/*
 * optiga.c — pico_optiga: synchronous wrapper over the Infineon
 * OPTIGA Trust M host library.
 *
 * The vendor API is asynchronous with a quirk: the util calls return
 * OPTIGA_LIB_SUCCESS to mean "accepted, in flight" (not "done") and
 * complete later in the registered callback. Every accepted call is
 * therefore followed by a pump loop that drives the stack's one-shot
 * chain (see pal/pal_os_event.c), bounded by a timeout. The instance
 * stays locked until the completion callback fires — calling the next
 * API too early fails with OPTIGA_UTIL_ERROR_INSTANCE_IN_USE (0x0305).
 *
 * Hardware (see PLAN.md, Phase 1 facts):
 *   I2C0: SDA = GP20, SCL = GP21, 100 kHz (the vendor stack negotiates
 *         up to 400 kHz during the application open)
 *   RST:  GP22, active low (VDD is hardwired to 3.3 V — warm resets only)
 */

#include "pico_optiga/optiga.h"

#include <stdbool.h>
#include <string.h>

#include "pico/critical_section.h"
#include "pico/stdlib.h"
#include "pico/time.h"

#include "hardware/gpio.h"
#include "hardware/i2c.h"

#include "optiga_util.h"
#include "optiga_crypt.h"
#include "pal_os_event.h"

#include "optiga_status.h"

/* ---------------------------------------------------------------------
 * Hardware configuration
 * ------------------------------------------------------------------- */

#define OPTIGA_I2C          (i2c0)
#define OPTIGA_I2C_SDA_PIN  20u
#define OPTIGA_I2C_SCL_PIN  21u
#define OPTIGA_I2C_HZ       100000u
#define OPTIGA_RST_PIN      22u
#define TRUSTM_I2C_ADDR     0x30u  /* factory default 7-bit address */

/* Warm reset (datasheet 5.1.4.2, Table 14): RST low 10 µs…2.5 ms (we
 * use 1 ms); first transmission only >= tSTARTUP (15-20 ms) after the
 * rising edge. */
#define OPTIGA_RST_PULSE_MS  1u
#define OPTIGA_RST_SETTLE_MS 25u

/* Raw registry access (datasheet Table 20/24): GUARD_TIME >= 50 µs
 * between consecutive accesses (we use 100 µs); the chip NAKs its
 * address while busy/idle, so retry on a tight cadence. */
#define OPTIGA_GUARD_TIME_US      100u
#define OPTIGA_REG_READ_RETRIES   5u
#define OPTIGA_REG_RETRY_DELAY_MS 50u

/* Timeout for one synchronous operation. The open (reset sequence +
 * IFX negotiation) completes in well under a second on the bench; 2 s
 * is a large margin for a dead or slow bus. */
#define OPTIGA_OP_TIMEOUT_MS 2000u

/* Data object IDs (SRM, "Common data objects with TAG's and AC's"). */
#define OID_GLOBAL_LIFECYCLE_STATE 0xE0C0u  /* 1 byte: LcsG */
#define OID_COPROCESSOR_UID        0xE0C2u  /* 27 bytes: unique ID + build info */

/* IFX I2C registry (datasheet Table 20). */
#define REG_I2C_STATE 0x82u  /* 4 bytes, RO: state + response data length */

/* ---------------------------------------------------------------------
 * State (one chip, one instance — static, no per-call allocation)
 * ------------------------------------------------------------------- */

static optiga_util_t *g_util = NULL;
static optiga_crypt_t *g_crypt = NULL;
static volatile optiga_lib_status_t g_status = OPTIGA_LIB_SUCCESS;
static bool g_inited = false;
static bool g_app_open = false;
static bool g_op_inflight = false;
static critical_section_t g_cs;

/* Completion callback shared by the util and crypt instances: both drive
 * the same optiga_cmd scheduler and one-shot event slot, and we run one
 * operation at a time (op_begin/op_end), so a single g_status is enough. */
static void op_callback(void *context, optiga_lib_status_t return_status) {
    (void)context;
    g_status = return_status;
}

/* ---------------------------------------------------------------------
 * Synchronous wait helpers
 * ------------------------------------------------------------------- */

/*
 * Pump the stack's one-shot chain until the completion callback has
 * fired, or the timeout expires. Returns PICO_OPTIGA_OK on completion,
 * PICO_OPTIGA_ETIMEOUT if the chip does not finish in time.
 *
 * The ~1 ms cadence matches the stack's own PL_POLLING_INVERVAL_US.
 * (pico-sdk 2.x has no time_reached(); a deadline is reached when
 * absolute_time_diff_us(deadline, now) >= 0, as in pal_os_event.c.)
 */
static int pump_until_complete(uint32_t timeout_ms) {
    absolute_time_t deadline = make_timeout_time_ms(timeout_ms);
    while (OPTIGA_LIB_BUSY == g_status) {
        if (absolute_time_diff_us(deadline, get_absolute_time()) >= 0) {
            return PICO_OPTIGA_ETIMEOUT;
        }
        pal_os_event_trigger_registered_callback();
        sleep_ms(1);
    }
    return PICO_OPTIGA_OK;
}

/*
 * Await the outcome of an asynchronous util call. `issued` is the
 * return value of the util API: OPTIGA_LIB_SUCCESS means "accepted, in
 * flight" (pump until the completion callback fires); anything else is
 * a synchronous error the callback will not supersede. Returns a
 * pico_optiga error code.
 */
static int await_result(optiga_lib_status_t issued) {
    if (OPTIGA_LIB_SUCCESS == issued) {
        int rc = pump_until_complete(OPTIGA_OP_TIMEOUT_MS);
        if (PICO_OPTIGA_OK != rc) {
            return rc;
        }
    }
    return pico_optiga_map_status(g_status);
}

/*
 * Re-entrancy guard: mark an operation in flight. Not thread-safe by
 * design (single core, no OS); the critical section is cheap insurance
 * for a future FreeRTOS port — it makes the check-and-set atomic, and
 * a task entering while the pump runs gets PICO_OPTIGA_EBUSY. The pump
 * itself is *not* held under the section (it blocks for up to the
 * timeout).
 */
static int op_begin(void) {
    /* Fast path without the lock: the section is only initialized once
     * pico_optiga_init() has run. */
    if (!g_inited) {
        return PICO_OPTIGA_ENOTINIT;
    }
    critical_section_enter_blocking(&g_cs);
    if (!g_inited) {  /* re-check under the lock */
        critical_section_exit(&g_cs);
        return PICO_OPTIGA_ENOTINIT;
    }
    if (g_op_inflight) {
        critical_section_exit(&g_cs);
        return PICO_OPTIGA_EBUSY;
    }
    g_op_inflight = true;
    critical_section_exit(&g_cs);
    return PICO_OPTIGA_OK;
}

static void op_end(void) {
    critical_section_enter_blocking(&g_cs);
    g_op_inflight = false;
    critical_section_exit(&g_cs);
}

/* ---------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------- */

int pico_optiga_init(void) {
    if (g_inited) {
        return PICO_OPTIGA_OK;
    }

    /* Assign the spin lock for the operation guard (pico-sdk 2.x). */
    critical_section_init(&g_cs);

    /* 1. RST pin: drive high *before* enabling the output driver — the
     *    RP2350 GPIO OUT register defaults to 0, so a plain
     *    gpio_set_dir(OUT) would emit a low spike on RST at boot. */
    gpio_init(OPTIGA_RST_PIN);
    gpio_put(OPTIGA_RST_PIN, 1);
    gpio_set_dir(OPTIGA_RST_PIN, GPIO_OUT);

    /* 2. Warm reset with the bus idle (SDA/SCL as pulled-up inputs):
     *    clears any busy state left by a previous run. The vendor open
     *    below performs its own reset sequence; this one guarantees a
     *    clean start for the I2C bring-up and for pico_optiga_probe(). */
    gpio_pull_up(OPTIGA_I2C_SDA_PIN);
    gpio_pull_up(OPTIGA_I2C_SCL_PIN);
    sleep_ms(1);
    gpio_put(OPTIGA_RST_PIN, 0);
    sleep_ms(OPTIGA_RST_PULSE_MS);
    gpio_put(OPTIGA_RST_PIN, 1);
    sleep_ms(OPTIGA_RST_SETTLE_MS);

    /* 3. I2C0 on GP20/GP21 @ 100 kHz (weak internal pull-ups as a
     *    safety net; the module carries external pull-ups). */
    i2c_init(OPTIGA_I2C, OPTIGA_I2C_HZ);
    gpio_set_function(OPTIGA_I2C_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(OPTIGA_I2C_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(OPTIGA_I2C_SDA_PIN);
    gpio_pull_up(OPTIGA_I2C_SCL_PIN);

    /* 4. The util and crypt instances (each heap-allocated once by the
     *    vendor; static for the lifetime of the process). Both share the
     *    one optiga_cmd scheduler and PAL event slot, so creating the
     *    crypt instance here has no bus side effect. */
    g_util = optiga_util_create(0, op_callback, NULL);
    if (NULL == g_util) {
        return PICO_OPTIGA_EIO;
    }
    g_crypt = optiga_crypt_create(0, op_callback, NULL);
    if (NULL == g_crypt) {
        (void)optiga_util_destroy(g_util);
        g_util = NULL;
        return PICO_OPTIGA_EIO;
    }

    g_inited = true;

    /* 5. Open the application: vendor reset sequence (2 ms RST + 12 ms
     *    startup) + IFX I2C negotiation. On failure the instance stays
     *    created — probe() and deinit() remain usable. */
    g_status = OPTIGA_LIB_BUSY;
    int rc = await_result(optiga_util_open_application(g_util, 0));
    g_app_open = (PICO_OPTIGA_OK == rc);
    return rc;
}

int pico_optiga_deinit(void) {
    if (!g_inited) {
        return PICO_OPTIGA_OK;
    }

    if (NULL != g_util) {
        /* Best-effort close; only attempted when the open completed,
         * so a timed-out open is not stacked with a close command. */
        if (g_app_open) {
            g_status = OPTIGA_LIB_BUSY;
            (void)await_result(optiga_util_close_application(g_util, 0));
        }
        (void)optiga_util_destroy(g_util);
        g_util = NULL;
    }
    if (NULL != g_crypt) {
        (void)optiga_crypt_destroy(g_crypt);
        g_crypt = NULL;
    }

    g_app_open = false;
    g_inited = false;
    i2c_deinit(OPTIGA_I2C);
    return PICO_OPTIGA_OK;
}

/* ---------------------------------------------------------------------
 * Diagnostics
 * ------------------------------------------------------------------- */

/*
 * Read an IFX I2C registry register: write the register address, then
 * read the data (datasheet Appendix A.2.1.1). Retries on NAK /
 * all-0xFF (chip not ready, transient bus state). Returns 0 on success.
 */
static int reg_read(uint8_t reg, uint8_t *buf, size_t len) {
    for (uint32_t attempt = 1; attempt <= OPTIGA_REG_READ_RETRIES; attempt++) {
        sleep_us(OPTIGA_GUARD_TIME_US);
        /* Pico SDK: the blocking calls return the number of bytes
         * transferred on success; negative = address NAK / timeout. */
        if (i2c_write_blocking(OPTIGA_I2C, TRUSTM_I2C_ADDR, &reg, 1, false) < 0) {
            sleep_ms(OPTIGA_REG_RETRY_DELAY_MS);
            continue;
        }
        sleep_us(OPTIGA_GUARD_TIME_US);
        if (i2c_read_blocking(OPTIGA_I2C, TRUSTM_I2C_ADDR, buf, len, false) < 0) {
            sleep_ms(OPTIGA_REG_RETRY_DELAY_MS);
            continue;
        }
        /* All-0xFF means the bus floated (NAK) — treat as a failed read. */
        int all_ff = 1;
        for (size_t i = 0; i < len; i++) {
            if (buf[i] != 0xFFu) {
                all_ff = 0;
                break;
            }
        }
        if (!all_ff) {
            return 0;
        }
        sleep_ms(OPTIGA_REG_RETRY_DELAY_MS);
    }
    return -1;
}

int pico_optiga_probe(void) {
    if (!g_inited) {
        return PICO_OPTIGA_ENOTINIT;
    }
    uint8_t state[4] = {0};
    if (0 != reg_read(REG_I2C_STATE, state, sizeof(state))) {
        return PICO_OPTIGA_ENODEV;
    }
    return PICO_OPTIGA_OK;
}

/* ---------------------------------------------------------------------
 * Identity
 * ------------------------------------------------------------------- */

static uint16_t read_le16(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t read_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Fill the decoded UID fields (SRM "Coprocessor UID OPTIGA Trust M
 * Family" table; multi-byte integers are little-endian). */
static void decode_uid(pico_optiga_info_t *info) {
    const uint8_t *u = info->uid;
    info->cim_id = u[0];
    info->platform_id = u[1];
    info->model_id = u[2];
    info->rom_code = read_le16(&u[3]);
    memcpy(info->chip_type, &u[5], sizeof(info->chip_type));
    memcpy(info->batch_number, &u[11], sizeof(info->batch_number));
    info->wafer_pos_x = read_le16(&u[17]);
    info->wafer_pos_y = read_le16(&u[19]);
    info->fw_identifier = read_le32(&u[21]);
    memcpy(info->esw_build, &u[25], sizeof(info->esw_build));
}

/* ---------------------------------------------------------------------
 * Data objects
 * ------------------------------------------------------------------- */

/* Core read; the caller holds the operation guard (op_begin/op_end). */
static int read_data_obj_impl(uint16_t oid, uint8_t *buf, size_t len,
                              size_t *out_len) {
    uint16_t length = (uint16_t)len;
    g_status = OPTIGA_LIB_BUSY;
    int rc = await_result(optiga_util_read_data(g_util, oid, 0, buf, &length));
    *out_len = (PICO_OPTIGA_OK == rc) ? (size_t)length : 0u;
    return rc;
}

int pico_optiga_read_data_obj(uint16_t oid, uint8_t *buf, size_t len,
                              size_t *out_len) {
    if ((NULL == buf) || (0 == len) || (NULL == out_len) ||
        (len > 0xFFFFu)) {
        return PICO_OPTIGA_EINVAL;
    }
    int rc = op_begin();
    if (PICO_OPTIGA_OK != rc) {
        return rc;
    }
    rc = read_data_obj_impl(oid, buf, len, out_len);
    op_end();
    return rc;
}

int pico_optiga_write_data_obj(uint16_t oid, pico_optiga_write_mode_t mode,
                               const uint8_t *buf, size_t len) {
    if ((NULL == buf) || (0 == len) || (len > 0xFFFFu) ||
        ((uint32_t)mode > (uint32_t)PICO_OPTIGA_ERASE_AND_WRITE)) {
        return PICO_OPTIGA_EINVAL;
    }
    uint8_t wtype = (PICO_OPTIGA_ERASE_AND_WRITE == mode)
                        ? OPTIGA_UTIL_ERASE_AND_WRITE
                        : OPTIGA_UTIL_WRITE_ONLY;
    int rc = op_begin();
    if (PICO_OPTIGA_OK != rc) {
        return rc;
    }
    g_status = OPTIGA_LIB_BUSY;
    rc = await_result(optiga_util_write_data(g_util, oid, wtype, 0, buf,
                                             (uint16_t)len));
    op_end();
    return rc;
}

/* Core metadata read; the caller holds the operation guard. */
static int read_metadata_impl(uint16_t oid, uint8_t *buf, size_t len,
                              size_t *out_len) {
    uint16_t length = (uint16_t)len;
    g_status = OPTIGA_LIB_BUSY;
    int rc = await_result(optiga_util_read_metadata(g_util, oid, buf, &length));
    *out_len = (PICO_OPTIGA_OK == rc) ? (size_t)length : 0u;
    return rc;
}

static int write_metadata_impl(uint16_t oid, const uint8_t *buf, size_t len) {
    g_status = OPTIGA_LIB_BUSY;
    return await_result(optiga_util_write_metadata(g_util, oid, buf,
                                                   (uint8_t)len));
}

int pico_optiga_read_metadata(uint16_t oid, uint8_t *buf, size_t len,
                              size_t *out_len) {
    if ((NULL == buf) || (0 == len) || (NULL == out_len) ||
        (len > 0xFFFFu)) {
        return PICO_OPTIGA_EINVAL;
    }
    int rc = op_begin();
    if (PICO_OPTIGA_OK != rc) {
        return rc;
    }
    rc = read_metadata_impl(oid, buf, len, out_len);
    op_end();
    return rc;
}

int pico_optiga_write_metadata(uint16_t oid, const uint8_t *buf, size_t len) {
    /* The vendor API takes a uint8_t length, so the metadata blob is capped at
     * 255 bytes. The write is a TLV delta: only the tags present in `buf` are
     * updated; the rest of the object's metadata is left untouched. */
    if ((NULL == buf) || (0u == len) || (len > 0xFFu)) {
        return PICO_OPTIGA_EINVAL;
    }
    int rc = op_begin();
    if (PICO_OPTIGA_OK != rc) {
        return rc;
    }
    rc = write_metadata_impl(oid, buf, len);
    op_end();
    return rc;
}

int pico_optiga_get_random(uint8_t *buf, size_t len) {
    /* The chip generates 8..256 bytes per call. */
    if ((NULL == buf) || (len < 8u) || (len > 256u)) {
        return PICO_OPTIGA_EINVAL;
    }
    int rc = op_begin();
    if (PICO_OPTIGA_OK != rc) {
        return rc;
    }
    g_status = OPTIGA_LIB_BUSY;
    rc = await_result(optiga_crypt_random(g_crypt, OPTIGA_RNG_TYPE_TRNG, buf,
                                          (uint16_t)len));
    op_end();
    return rc;
}

/* ---------------------------------------------------------------------
 * Crypto (ECC / ECDSA)
 * ------------------------------------------------------------------- */

/*
 * Keygen / sign / verify all run on g_crypt, which shares the same
 * optiga_cmd scheduler and one-shot event slot as g_util (see op_callback).
 * The private key is always kept on the chip (export_private_key = FALSE);
 * only the public key and signatures cross to the host.
 */
int pico_optiga_ecc_gen_keypair(uint16_t oid, uint8_t curve, uint8_t usage,
                                uint8_t *pub, size_t pub_cap,
                                size_t *out_pub_len) {
    if ((NULL == pub) || (0u == pub_cap) || (NULL == out_pub_len) ||
        (pub_cap > 0xFFFFu)) {
        return PICO_OPTIGA_EINVAL;
    }
    int rc = op_begin();
    if (PICO_OPTIGA_OK != rc) {
        return rc;
    }
    optiga_key_id_t key_id = (optiga_key_id_t)oid;
    uint16_t pub_len = (uint16_t)pub_cap;
    g_status = OPTIGA_LIB_BUSY;
    rc = await_result(optiga_crypt_ecc_generate_keypair(g_crypt,
                                                        (optiga_ecc_curve_t)curve,
                                                        usage, FALSE, &key_id,
                                                        pub, &pub_len));
    if (PICO_OPTIGA_OK == rc) {
        *out_pub_len = (size_t)pub_len;
    }
    op_end();
    return rc;
}

int pico_optiga_ecdsa_sign(uint16_t oid, const uint8_t *digest,
                           size_t digest_len, uint8_t *sig, size_t sig_cap,
                           size_t *out_sig_len) {
    if ((NULL == digest) || (0u == digest_len) || (digest_len > 0xFFu) ||
        (NULL == sig) || (0u == sig_cap) || (NULL == out_sig_len) ||
        (sig_cap > 0xFFFFu)) {
        return PICO_OPTIGA_EINVAL;
    }
    int rc = op_begin();
    if (PICO_OPTIGA_OK != rc) {
        return rc;
    }
    optiga_key_id_t key_id = (optiga_key_id_t)oid;
    uint16_t sig_len = (uint16_t)sig_cap;
    g_status = OPTIGA_LIB_BUSY;
    rc = await_result(optiga_crypt_ecdsa_sign(g_crypt, digest,
                                              (uint8_t)digest_len, key_id,
                                              sig, &sig_len));
    if (PICO_OPTIGA_OK == rc) {
        *out_sig_len = (size_t)sig_len;
    }
    op_end();
    return rc;
}

int pico_optiga_ecdsa_verify(const uint8_t *digest, size_t digest_len,
                             const uint8_t *sig, size_t sig_len,
                             const uint8_t *pub, size_t pub_len,
                             uint8_t curve) {
    if ((NULL == digest) || (0u == digest_len) || (digest_len > 0xFFu) ||
        (NULL == sig) || (0u == sig_len) || (sig_len > 0xFFFFu) ||
        (NULL == pub) || (0u == pub_len) || (pub_len > 0xFFFFu)) {
        return PICO_OPTIGA_EINVAL;
    }
    int rc = op_begin();
    if (PICO_OPTIGA_OK != rc) {
        return rc;
    }
    public_key_from_host_t pk;
    pk.public_key = (uint8_t *)pub;  /* verify only reads the key */
    pk.length = (uint16_t)pub_len;
    pk.key_type = curve;
    g_status = OPTIGA_LIB_BUSY;
    rc = await_result(optiga_crypt_ecdsa_verify(g_crypt, digest,
                                                (uint8_t)digest_len, sig,
                                                (uint16_t)sig_len,
                                                OPTIGA_CRYPT_HOST_DATA, &pk));
    op_end();
    return rc;
}

/* ---------------------------------------------------------------------
 * Crypto (KDF / agreement / hash / symmetric)
 *
 * All run on g_crypt (same scheduler + one-shot event slot as the rest).
 * Secrets/keys stay on the chip; only digests, derived keys, and shared
 * secrets cross to the host.
 * ------------------------------------------------------------------- */

int pico_optiga_sha256(const uint8_t *data, size_t len, uint8_t out[32]) {
    if ((NULL == data) || (0u == len) || (NULL == out)) {
        return PICO_OPTIGA_EINVAL;
    }
    int rc = op_begin();
    if (PICO_OPTIGA_OK != rc) {
        return rc;
    }
    hash_data_from_host_t hd;
    hd.buffer = data;
    hd.length = (uint32_t)len;
    g_status = OPTIGA_LIB_BUSY;
    rc = await_result(optiga_crypt_hash(g_crypt, OPTIGA_HASH_TYPE_SHA_256,
                                        OPTIGA_CRYPT_HOST_DATA, &hd, out));
    op_end();
    return rc;
}

int pico_optiga_ecdh(uint16_t oid, const uint8_t *pub, size_t pub_len,
                     uint8_t *out, size_t out_cap) {
    if ((NULL == pub) || (0u == pub_len) || (pub_len > 0xFFFFu) ||
        ((NULL != out) && (out_cap < 32u))) {
        return PICO_OPTIGA_EINVAL;
    }
    int rc = op_begin();
    if (PICO_OPTIGA_OK != rc) {
        return rc;
    }
    optiga_key_id_t key_id = (optiga_key_id_t)oid;
    public_key_from_host_t pk;
    pk.public_key = (uint8_t *)pub;  /* ECDH only reads the key */
    pk.length = (uint16_t)pub_len;
    pk.key_type = (uint8_t)OPTIGA_ECC_CURVE_NIST_P_256;
    g_status = OPTIGA_LIB_BUSY;
    rc = await_result(optiga_crypt_ecdh(g_crypt, key_id, &pk,
                                        (NULL != out) ? TRUE : FALSE, out));
    op_end();
    return rc;
}

int pico_optiga_hkdf(uint16_t secret, const uint8_t *salt, size_t salt_len,
                     const uint8_t *info, size_t info_len,
                     uint8_t *out, size_t out_len) {
    if ((NULL == out) || (0u == out_len) || (out_len > 32u) ||
        ((NULL != salt) && (salt_len > 0xFFFFu)) ||
        ((NULL != info) && (info_len > 0xFFFFu))) {
        return PICO_OPTIGA_EINVAL;
    }
    int rc = op_begin();
    if (PICO_OPTIGA_OK != rc) {
        return rc;
    }
    g_status = OPTIGA_LIB_BUSY;
    rc = await_result(optiga_crypt_hkdf(g_crypt, OPTIGA_HKDF_SHA_256, secret,
                                        salt, (uint16_t)salt_len,
                                        info, (uint16_t)info_len,
                                        (uint16_t)out_len, TRUE, out));
    op_end();
    return rc;
}

int pico_optiga_symmetric_generate_key(uint16_t oid, uint8_t key_type,
                                       uint8_t usage, int export_key,
                                       uint8_t *out, size_t out_len) {
    if ((export_key && ((NULL == out) || (0u == out_len))) ||
        (out_len > 0xFFFFu)) {
        return PICO_OPTIGA_EINVAL;
    }
    int rc = op_begin();
    if (PICO_OPTIGA_OK != rc) {
        return rc;
    }
    /* When export_key is FALSE the vendor API returns the generated key's OID
     * through this pointer; when TRUE it receives the exported key bytes. */
    optiga_key_id_t key_id = (optiga_key_id_t)oid;
    g_status = OPTIGA_LIB_BUSY;
    rc = await_result(optiga_crypt_symmetric_generate_key(
                          g_crypt, (optiga_symmetric_key_type_t)key_type, usage,
                          (export_key ? TRUE : FALSE),
                          export_key ? (void *)out : (void *)&key_id));
    op_end();
    return rc;
}

int pico_optiga_symmetric_encrypt(pico_optiga_sym_mode_t mode, uint16_t key_oid,
                                  const uint8_t *plain, size_t plain_len,
                                  const uint8_t *iv, size_t iv_len,
                                  const uint8_t *aad, size_t aad_len,
                                  uint8_t *out, size_t out_cap, size_t *out_len) {
    if ((NULL == plain) || (0u == plain_len) || (NULL == out) ||
        (0u == out_cap) || (NULL == out_len)) {
        return PICO_OPTIGA_EINVAL;
    }
    int rc = op_begin();
    if (PICO_OPTIGA_OK != rc) {
        return rc;
    }
    optiga_key_id_t key_id = (optiga_key_id_t)key_oid;
    uint32_t out_len_u32 = (uint32_t)out_cap;
    g_status = OPTIGA_LIB_BUSY;
    rc = await_result(optiga_crypt_symmetric_encrypt(
                          g_crypt, (optiga_symmetric_encryption_mode_t)mode,
                          key_id, plain, (uint32_t)plain_len,
                          iv, (uint16_t)iv_len, aad, (uint16_t)aad_len,
                          out, &out_len_u32));
    if (PICO_OPTIGA_OK == rc) {
        *out_len = (size_t)out_len_u32;
    }
    op_end();
    return rc;
}

int pico_optiga_symmetric_decrypt(pico_optiga_sym_mode_t mode, uint16_t key_oid,
                                  const uint8_t *cipher, size_t cipher_len,
                                  const uint8_t *iv, size_t iv_len,
                                  const uint8_t *aad, size_t aad_len,
                                  uint8_t *out, size_t out_cap, size_t *out_len) {
    if ((NULL == cipher) || (0u == cipher_len) || (NULL == out) ||
        (0u == out_cap) || (NULL == out_len)) {
        return PICO_OPTIGA_EINVAL;
    }
    int rc = op_begin();
    if (PICO_OPTIGA_OK != rc) {
        return rc;
    }
    optiga_key_id_t key_id = (optiga_key_id_t)key_oid;
    uint32_t out_len_u32 = (uint32_t)out_cap;
    g_status = OPTIGA_LIB_BUSY;
    rc = await_result(optiga_crypt_symmetric_decrypt(
                          g_crypt, (optiga_symmetric_encryption_mode_t)mode,
                          key_id, cipher, (uint32_t)cipher_len,
                          iv, (uint16_t)iv_len, aad, (uint16_t)aad_len,
                          out, &out_len_u32));
    if (PICO_OPTIGA_OK == rc) {
        *out_len = (size_t)out_len_u32;
    }
    op_end();
    return rc;
}

/* Up-counter (e.g. the FIDO2 sync counter). Runs on g_util; a monotonic NVM
 * increment — each call is a write, bounded by endurance. */
int pico_optiga_counter_incr(uint16_t oid, uint8_t amount) {
    if (0u == amount) {
        return PICO_OPTIGA_EINVAL;
    }
    int rc = op_begin();
    if (PICO_OPTIGA_OK != rc) {
        return rc;
    }
    g_status = OPTIGA_LIB_BUSY;
    rc = await_result(optiga_util_update_count(g_util, oid, amount));
    op_end();
    return rc;
}

int pico_optiga_get_info(pico_optiga_info_t *info) {
    if (NULL == info) {
        return PICO_OPTIGA_EINVAL;
    }
    int rc = op_begin();
    if (PICO_OPTIGA_OK != rc) {
        return rc;
    }

    memset(info, 0, sizeof(*info));
    size_t n = 0;
    rc = read_data_obj_impl(OID_GLOBAL_LIFECYCLE_STATE, &info->lifecycle,
                            1u, &n);
    if (PICO_OPTIGA_OK == rc) {
        rc = read_data_obj_impl(OID_COPROCESSOR_UID, info->uid,
                                PICO_OPTIGA_UID_LEN, &n);
    }
    if (PICO_OPTIGA_OK == rc) {
        decode_uid(info);
    }

    op_end();
    return rc;
}

/* ---------------------------------------------------------------------
 * Error strings
 * ------------------------------------------------------------------- */

const char *pico_optiga_strerror(int err) {
    switch (err) {
    case PICO_OPTIGA_OK:
        return "success";
    case PICO_OPTIGA_EINVAL:
        return "invalid argument";
    case PICO_OPTIGA_ENOTINIT:
        return "not initialized (call pico_optiga_init first)";
    case PICO_OPTIGA_EBUSY:
        return "another operation is already in flight";
    case PICO_OPTIGA_ETIMEOUT:
        return "timeout waiting for the chip";
    case PICO_OPTIGA_ENODEV:
        return "no chip on the bus (check power and wiring)";
    case PICO_OPTIGA_EIO:
        return "I2C / protocol error";
    case PICO_OPTIGA_EDEVICE:
        return "OPTIGA device error (chip code in the SRM)";
    case PICO_OPTIGA_EHANDSHAKE:
        return "handshake error: chip is paired to a shielded connection "
               "(0x0107); unlock with Infineon tooling";
    default:
        return "unknown error";
    }
}
