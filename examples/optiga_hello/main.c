/*
 * optiga_hello — Phase 3: first library-level communication
 *
 * Extends optiga_open (the Phase 2 exit test) with data-object reads:
 * after opening the application on the chip it reads the identity
 * objects and prints them:
 *
 *   0xE0C0  Global Life Cycle Status (LcsG), 1 byte
 *           (0x07 = operational)
 *   0xE0C2  Coprocessor UID, 27 bytes — the chip's unique ID plus
 *           build and firmware information. Field layout per the
 *           SRM table "Coprocessor UID OPTIGA Trust M Family".
 *
 * Both objects have Read AC = ALW (always allowed), so they work on
 * factory-fresh and pre-paired chips alike.
 *
 * The Infineon API is asynchronous with a quirk: the util calls
 * return OPTIGA_LIB_SUCCESS to mean "accepted, in flight" (not
 * "done") and complete later in the registered callback. Every
 * accepted call is therefore followed by a pump loop that drives the
 * stack's one-shot chain (see pal/pal_os_event.c). The instance
 * stays locked until that completion callback has fired — calling
 * the next API too early fails with OPTIGA_UTIL_ERROR_INSTANCE_IN_USE
 * (0x0305).
 */

#include <pico/stdlib.h>
#include <stdio.h>

#include "hardware/gpio.h"
#include "optiga_util.h"
#include "pal_os_event.h"

/* Data object IDs (SRM, "Common data objects with TAG's and AC's"). */
#define OID_GLOBAL_LIFECYCLE_STATE 0xE0C0u  /* 1 byte: LcsG */
#define OID_COPROCESSOR_UID 0xE0C2u         /* 27 bytes: unique ID + build info */

/* Coprocessor UID field offsets (SRM, "Coprocessor UID OPTIGA Trust M
 * Family" table). Multi-byte integers are little-endian. */
#define UID_CIM_ID 0u            /* uint8: CIM identifier */
#define UID_PLATFORM_ID 1u       /* uint8: platform identifier */
#define UID_MODEL_ID 2u          /* uint8: model identifier */
#define UID_ROM_CODE 3u          /* uint16: ID of ROM mask */
#define UID_CHIP_TYPE 5u         /* uint8[6]: chip type */
#define UID_BATCH_NUMBER 11u     /* uint8[6]: batch number */
#define UID_CHIP_POS_X 17u       /* uint16: wafer X position */
#define UID_CHIP_POS_Y 19u       /* uint16: wafer Y position */
#define UID_FW_IDENTIFIER 21u    /* uint32: firmware identifier */
#define UID_ESW_BUILD 25u        /* uint8[2]: ESW build number, BCD */

#define UID_LEN 27u

#define TRUSTM_RST_GPIO 22u

static void rst_pin_bringup(void) {
    // Drive high before enabling the driver: the RP2350 GPIO OUT
    // register defaults to 0, so a plain gpio_set_dir(GPIO_OUT) would
    // emit a low spike on RST during boot.
    gpio_init(TRUSTM_RST_GPIO);
    gpio_put(TRUSTM_RST_GPIO, 1);
    gpio_set_dir(TRUSTM_RST_GPIO, GPIO_OUT);
}

static volatile optiga_lib_status_t lib_status;

static void util_callback(void *context, optiga_lib_status_t return_status) {
    (void)context;
    lib_status = return_status;
}

/* Pump the stack's one-shot chain until the completion callback has
 * fired. The ~1 ms cadence matches the stack's own polling interval. */
static void pump_until_complete(void) {
    while (OPTIGA_LIB_BUSY == lib_status) {
        pal_os_event_trigger_registered_callback();
        sleep_ms(1);
    }
}

/* Synchronous wrapper around the asynchronous optiga_util_read_data():
 * issues the read, pumps until the callback fires, reports the result.
 * Returns the number of bytes read (0 on failure). */
static uint16_t read_data_object(optiga_util_t *util, uint16_t oid,
                                 const char *name, uint8_t *buffer,
                                 uint16_t length) {
    optiga_lib_status_t status;

    lib_status = OPTIGA_LIB_BUSY;
    status = optiga_util_read_data(util, oid, 0, buffer, &length);
    if (OPTIGA_LIB_SUCCESS == status) {
        // "Accepted, in flight" — pump until the completion
        // callback fires (it also unlocks the instance).
        pump_until_complete();
    } else {
        // Synchronous error: the callback will not fire.
        lib_status = status;
    }

    if (OPTIGA_LIB_SUCCESS == lib_status) {
        printf("%s [0x%04X]: %u bytes\n", name, (unsigned)oid,
               (unsigned)length);
    } else {
        printf("%s [0x%04X]: FAIL (0x%04X)\n", name, (unsigned)oid,
               (unsigned)lib_status);
        length = 0;
    }
    return length;
}

static void print_hex(const char *label, const uint8_t *data, uint16_t len) {
    printf("  %-16s", label);
    for (uint16_t i = 0; i < len; i++) {
        printf("%02X ", data[i]);
    }
    printf("\n");
}

static uint16_t read_le16(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t read_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int main(void) {
    rst_pin_bringup();
    stdio_init_all();

    printf("== OPTIGA Trust M hello (via Infineon host library) ==\n");
    printf("Creating optiga_util instance...\n");

    optiga_util_t *util = optiga_util_create(0, util_callback, NULL);
    if (NULL == util) {
        printf("FAIL: optiga_util_create returned NULL\n");
        return 1;
    }

    optiga_lib_status_t status;

    printf("Opening application (2 ms RST + 12 ms startup, then IFX "
           "negotiation)...\n");
    lib_status = OPTIGA_LIB_BUSY;
    status = optiga_util_open_application(util, 0);
    if (OPTIGA_LIB_SUCCESS == status) {
        pump_until_complete();
    } else {
        lib_status = status;
    }
    printf("open_application:  %s (0x%04X)\n",
           (OPTIGA_LIB_SUCCESS == lib_status) ? "OK" : "FAIL",
           (unsigned)lib_status);

    if (OPTIGA_LIB_SUCCESS == lib_status) {
        uint8_t lcsg;
        if (1u == read_data_object(util, OID_GLOBAL_LIFECYCLE_STATE,
                                   "Global LcsG", &lcsg, sizeof(lcsg))) {
            printf("  lifecycle: 0x%02X%s\n", (unsigned)lcsg,
                   (0x07u == lcsg) ? " (operational)" : "");
        }

        uint8_t uid[UID_LEN];
        if (UID_LEN == read_data_object(util, OID_COPROCESSOR_UID,
                                        "Coprocessor UID", uid,
                                        sizeof(uid))) {
            printf("  CIM identifier:   0x%02X\n",
                   (unsigned)uid[UID_CIM_ID]);
            printf("  platform:         0x%02X\n",
                   (unsigned)uid[UID_PLATFORM_ID]);
            printf("  model:            0x%02X\n",
                   (unsigned)uid[UID_MODEL_ID]);
            printf("  ROM code:         0x%04X\n",
                   (unsigned)read_le16(&uid[UID_ROM_CODE]));
            print_hex("chip type:", &uid[UID_CHIP_TYPE], 6u);
            print_hex("batch number:", &uid[UID_BATCH_NUMBER], 6u);
            printf("  wafer position:   %u / %u\n",
                   (unsigned)read_le16(&uid[UID_CHIP_POS_X]),
                   (unsigned)read_le16(&uid[UID_CHIP_POS_Y]));
            printf("  firmware id:      0x%08lX\n",
                   (unsigned long)read_le32(&uid[UID_FW_IDENTIFIER]));
            printf("  ESW build (BCD):  %02X %02X\n",
                   (unsigned)uid[UID_ESW_BUILD],
                   (unsigned)uid[UID_ESW_BUILD + 1]);
            print_hex("full UID:", uid, (uint16_t)sizeof(uid));
        }
    }

    printf("Closing application...\n");
    lib_status = OPTIGA_LIB_BUSY;
    status = optiga_util_close_application(util, 0);
    if (OPTIGA_LIB_SUCCESS == status) {
        pump_until_complete();
    } else {
        lib_status = status;
    }
    printf("close_application: %s (0x%04X)\n",
           (OPTIGA_LIB_SUCCESS == lib_status) ? "OK" : "FAIL",
           (unsigned)lib_status);

    // lint --e{534} the return value is not checked
    optiga_util_destroy(util);
    printf("done\n");
    return 0;
}
