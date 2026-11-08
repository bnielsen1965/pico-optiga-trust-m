/*
 * i2c_probe — Phase 1: raw I2C probe of the OPTIGA Trust M.
 *
 * No Infineon library involved. Talks plain I2C to the chip's IFX I2C
 * registry (datasheet Table 20) to verify:
 *   - wiring / pull-ups (bus scan)
 *   - the chip answers at its factory address 0x30
 *   - the chip is alive and reports sane I2C settings
 *
 * Register access pattern (datasheet Appendix A.2.1.1):
 *   1. write 1 byte  = register address
 *   2. read N bytes  = register data
 *
 * Protocol constraints (datasheet Table 24 / Appendix A.2.1):
 *   - GUARD_TIME: >= 50 µs between consecutive accesses to the chip,
 *     measured at the master from STOP to the next START; the chip NAKs
 *     the read address if the read follows the register write too soon.
 *   - The chip does not ACK its address while busy (or in idle state);
 *     the host must retry, up to ~100 ms in the extreme case.
 *
 * Bus:  I2C0, SDA = GP20, SCL = GP21, 100 kHz
 * RST:  GP22 (active low) — warm reset before probing (datasheet 5.1.4.2)
 * Chip: OPTIGA Trust M @ 0x30 (7-bit), I2C-ready 15 ms after power-on
 * Stdio: UART0 (CP210x bridge), 115200 8N1
 *
 * Note: a raw registry read cannot reveal whether the chip was paired to a
 * shielded connection — that shows up later as a 0x107 handshake error in
 * Phase 3.
 */
#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/i2c.h"

#define PROBE_I2C       (i2c0)
#define PROBE_SDA_PIN   20u
#define PROBE_SCL_PIN   21u
#define PROBE_RST_PIN   22u /* Trust M RST, active low */
#define PROBE_FREQ_HZ   100000u
#define TRUSTM_ADDR     0x30u

/* IFX I2C registry (datasheet Table 20) */
#define REG_I2C_STATE    0x82u /* 4 bytes, RO: state + response data length */
#define REG_MAX_SCL_FREQ 0x84u /* 4 bytes, RO: max SCL frequency in kHz      */
#define REG_I2C_MODE     0x89u /* 2 bytes, RW: current I2C mode              */

/* I2C_STATE feature bits (datasheet Table 23) */
#define ST_BUSY          (1u << 31)
#define ST_RESP_RDY      (1u << 30)
#define ST_SOFT_RESET    (1u << 27)
#define ST_CONT_READ     (1u << 26)
#define ST_REP_START     (1u << 25)
#define ST_CLK_STRETCH   (1u << 24)
#define ST_PRESENT_LAYER (1u << 23)

/* I2C_MODE encodings (datasheet Table 22) */
#define MODE_SM          0x01u
#define MODE_FM          0x02u
#define MODE_SM_FM       0x03u
#define MODE_FM_PLUS     0x04u

#define REG_READ_RETRIES 5u
#define REG_READ_RETRY_DELAY_MS 50u

/* Datasheet Table 24: GUARD_TIME — minimum time that must elapse at the
 * master between two consecutive accesses to the same device (measured from
 * the STOP condition until the next START). The reference IFX I2C stack
 * enforces exactly 50 µs (PL_GUARD_TIME_INTERVAL_US); we use 100 µs for
 * margin. Without it the chip NAKs the read address. */
#define TRUSTM_GUARD_TIME_US 100u

/* Warm-reset timing (datasheet 5.1.4.2, Table 14):
 *   - RST must be low for t1 = 10 µs to be detected, and must be
 *     released within 2.5 ms of the falling edge -> pulse 1 ms
 *   - first I2C transmission only >= tSTARTUP after RST rises:
 *     15 ms (20 ms if an NVM program was pending) -> settle 25 ms */
#define TRUSTM_RST_PULSE_MS  1u
#define TRUSTM_RST_SETTLE_MS 25u

/* Settle before the first probe. tSTARTUP (15-20 ms) is already covered
 * by the reset settle; this is margin. Keep this SHORT: long silence
 * after reset lets the chip settle into its idle state, in which it
 * will not acknowledge its address — the scan's retry loop below is
 * what wakes it. */
#define SCAN_SETTLE_MS 50u

/* The chip does not acknowledge its address while busy or in idle state
 * (datasheet A.2.1): "the host must retry or repeat the transaction
 * until it is successful or timed out for 100 milliseconds (extreme
 * case)". Empirically its ACK-ready window opens a few ms after bus
 * activity and closes again after ~20 ms of silence, so the retry
 * cadence must be tight (5 ms) — one re-probe after a long pause walks
 * right out of the window. */
#define SCAN_RETRY_INTERVAL_MS 5u
#define SCAN_RETRY_BUDGET_MS   100u

static const char *mode_name(uint8_t mode)
{
    switch (mode) {
    case MODE_SM:    return "SM (100 kHz)";
    case MODE_FM:    return "FM (400 kHz)";
    case MODE_SM_FM: return "SM & FM (fab default)";
    case MODE_FM_PLUS: return "FM+ (1 MHz)";
    default:         return "invalid";
    }
}

/*
 * Read an IFX I2C register: write the register address, then read the data.
 * Retries on NAK / all-0xFF (chip not ready, transient bus state).
 * Returns 0 on success, -1 on failure.
 */
static int reg_read(uint8_t reg, uint8_t *buf, size_t len)
{
    for (uint32_t attempt = 1; attempt <= REG_READ_RETRIES; attempt++) {
        /* Guard time since the previous access to the chip (covers the gap
         * between consecutive reg_read calls and the scan -> first read). */
        sleep_us(TRUSTM_GUARD_TIME_US);
        /* Pico SDK: i2c_write_blocking / i2c_read_blocking return the number
         * of bytes transferred on success, and a negative value on failure
         * (address NAK / timeout). A return of 0 means the address was
         * acknowledged but the first data byte was NAKed. */
        if (i2c_write_blocking(PROBE_I2C, TRUSTM_ADDR, &reg, 1, false) < 0) {
            sleep_ms(REG_READ_RETRY_DELAY_MS);
            continue;
        }
        /* The chip needs GUARD_TIME after the register-address write before
         * it will service the read; back-to-back write+read gets NAKed. */
        sleep_us(TRUSTM_GUARD_TIME_US);
        if (i2c_read_blocking(PROBE_I2C, TRUSTM_ADDR, buf, len, false) < 0) {
            sleep_ms(REG_READ_RETRY_DELAY_MS);
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
        sleep_ms(REG_READ_RETRY_DELAY_MS);
    }
    return -1;
}

static void print_hex(const char *label, const uint8_t *buf, size_t len)
{
    printf("  %-22s", label);
    for (size_t i = 0; i < len; i++) {
        printf("%02X ", buf[i]);
    }
    printf("\n");
}

/*
 * Warm-reset the chip (datasheet 5.1.4.2): VCC stays powered, RST is
 * pulsed low. This re-initializes the I2C interface and clears any busy
 * state left by a previous run, so every probe session starts from a
 * known-good state.
 *
 * Requirement: SDA and SCL must be high (bus idle) while RST is
 * asserted — call this before i2c_init(), with the I2C pins as plain
 * pulled-up inputs.
 */
/*
 * Bring up the RST pin safely, as early as possible in boot:
 *   1. the RP2350 GPIO OUT register defaults to 0 after reset, so the
 *      output value must be set HIGH *before* enabling the output
 *      driver — otherwise gpio_set_dir(OUT) emits a short low spike
 *      on RST;
 *   2. holding RST deterministically high from the start of boot means
 *      the chip never sees a floating reset line while the Pico starts
 *      up (the module side may carry no pull-up).
 */
static void rst_pin_bringup(void)
{
    gpio_init(PROBE_RST_PIN);
    gpio_put(PROBE_RST_PIN, 1);       /* output value high first... */
    gpio_set_dir(PROBE_RST_PIN, GPIO_OUT); /* ...then enable driver: no low spike */
}

static void trustm_reset(void)
{
    /* RST is already held high by rst_pin_bringup(). */
    sleep_ms(1);                /* let the line settle high */

    gpio_put(PROBE_RST_PIN, 0); /* assert reset */
    sleep_ms(TRUSTM_RST_PULSE_MS);
    gpio_put(PROBE_RST_PIN, 1); /* deassert (rising edge starts tSTARTUP) */

    sleep_ms(TRUSTM_RST_SETTLE_MS);
    printf("Chip warm-reset (RST GP%d low %u ms, tSTARTUP settle %u ms)\n",
           PROBE_RST_PIN, TRUSTM_RST_PULSE_MS, TRUSTM_RST_SETTLE_MS);
}

static void bus_scan(void)
{
    printf("I2C bus scan @ %u Hz:\n", PROBE_FREQ_HZ);
    int found = 0;
    uint32_t waited_ms = 0;

    /* Datasheet A.2.1: repeat the transaction until it succeeds, up to
     * a 100 ms budget. The tight cadence keeps the chip's ACK-ready
     * window from closing between attempts. */
    for (;;) {
        uint8_t dummy = 0;
        /* Success returns 1 (bytes written); 0 means the address was
         * acknowledged but the dummy data byte was NAKed — in either
         * case something on the bus answered the address, so count it
         * as present. Only a negative return (address NAK / timeout)
         * means no device. */
        if (i2c_write_blocking(PROBE_I2C, TRUSTM_ADDR, &dummy, 1, false) >= 0) {
            printf("  device at 0x%02X  (Trust M) — answered after %u ms\n",
                   TRUSTM_ADDR, (unsigned)waited_ms);
            found = 1;
            break;
        }
        if (waited_ms + SCAN_RETRY_INTERVAL_MS > SCAN_RETRY_BUDGET_MS) {
            break;
        }
        sleep_ms(SCAN_RETRY_INTERVAL_MS);
        waited_ms += SCAN_RETRY_INTERVAL_MS;
    }

    if (!found) {
        printf("  (no device answered within %u ms)\n", SCAN_RETRY_BUDGET_MS);
    }
}

int main(void)
{
    /* Hold RST high before *anything* else, so the chip never sees a
     * floating or glitchy reset line during Pico boot. */
    rst_pin_bringup();

    stdio_init_all(); /* pico-sdk 2.x replacement for pico_sdk_init() */

    /* Warm-reset the chip before touching the bus. The datasheet requires
     * SDA/SCL high while RST is asserted, so the I2C pins stay plain
     * pulled-up inputs until after the reset; i2c_init() comes later. */
    gpio_pull_up(PROBE_SDA_PIN);
    gpio_pull_up(PROBE_SCL_PIN);
    trustm_reset();

    i2c_init(PROBE_I2C, PROBE_FREQ_HZ);
    gpio_set_function(PROBE_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(PROBE_SCL_PIN, GPIO_FUNC_I2C);
    /* Weak internal pull-ups as a safety net; the Trust M module carries
     * external pull-ups, these just sit in parallel. */
    gpio_pull_up(PROBE_SDA_PIN);
    gpio_pull_up(PROBE_SCL_PIN);

    printf("== OPTIGA Trust M raw I2C probe ==\n");
    printf("Bus: I2C0 (SDA GP%d, SCL GP%d) @ %u kHz\n",
           PROBE_SDA_PIN, PROBE_SCL_PIN, PROBE_FREQ_HZ / 1000);
    printf("Expected chip address: 0x%02X\n\n", TRUSTM_ADDR);

    /* The warm reset already cleared any busy state and guaranteed
     * tSTARTUP; this is extra margin before the first probe. */
    sleep_ms(SCAN_SETTLE_MS);

    bus_scan();
    printf("\n");

    uint8_t state[4] = {0};
    uint8_t max_freq[4] = {0};
    uint8_t mode[2] = {0};

    int rc_state = reg_read(REG_I2C_STATE, state, sizeof(state));
    if (rc_state != 0) {
        printf("FAIL: no valid I2C_STATE from 0x%02X after %u attempts.\n",
               TRUSTM_ADDR, REG_READ_RETRIES);
        printf("Check: power (3.3 V on VDD), SDA/SCL wiring, pull-ups, and\n");
        printf("that GP20/GP21 are not driving something else.\n");
        return 1;
    }

    reg_read(REG_MAX_SCL_FREQ, max_freq, sizeof(max_freq));
    reg_read(REG_I2C_MODE, mode, sizeof(mode));

    print_hex("I2C_STATE (0x82):", state, sizeof(state));
    print_hex("MAX_SCL_FREQU (0x84):", max_freq, sizeof(max_freq));
    print_hex("I2C_MODE (0x89):", mode, sizeof(mode));
    printf("\n");

    uint32_t state_word = ((uint32_t)state[0] << 24) |
                          ((uint32_t)state[1] << 16) |
                          ((uint32_t)state[2] << 8) |
                          (uint32_t)state[3];
    uint16_t resp_len = (uint16_t)(state_word & 0xFFFFu);
    uint32_t max_khz = ((uint32_t)max_freq[0] << 24) |
                       ((uint32_t)max_freq[1] << 16) |
                       ((uint32_t)max_freq[2] << 8) |
                       (uint32_t)max_freq[3];
    uint8_t mode_val = mode[1] & 0x07u;

    printf("Decoded:\n");
    printf("  state byte   : 0x%02X  (BUSY=%u RESP_RDY=%u SOFT_RESET=%u "
           "CONT_READ=%u REP_START=%u CLK_STRETCH=%u)\n",
           state[0],
           !!(state_word & ST_BUSY),
           !!(state_word & ST_RESP_RDY),
           !!(state_word & ST_SOFT_RESET),
           !!(state_word & ST_CONT_READ),
           !!(state_word & ST_REP_START),
           !!(state_word & ST_CLK_STRETCH));
    printf("  resp. length : %u bytes\n", resp_len);
    printf("  max SCL      : %u kHz\n", (unsigned)max_khz);
    printf("  I2C mode     : 0x%02X -> %s\n", mode_val, mode_name(mode_val));

    if (state_word & ST_BUSY) {
        printf("\nChip reports BUSY — it is executing a command; re-probe in a moment.\n");
    }

    printf("\nPASS: OPTIGA Trust M is alive at 0x%02X.\n", TRUSTM_ADDR);
    printf("Next: Phase 2 (Pico PAL) + Phase 3 (optiga_util handshake).\n");
    return 0;
}
