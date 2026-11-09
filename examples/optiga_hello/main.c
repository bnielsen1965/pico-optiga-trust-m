/*
 * optiga_hello — Phase 4: first application on the pico_optiga library.
 *
 * The whole IFX protocol (reset sequence, I2C bring-up, IFX negotiation,
 * data-object reads) is hidden inside pico_optiga; the application is a
 * few synchronous calls:
 *
 *     pico_optiga_init()
 *     pico_optiga_get_info(&info)
 *     pico_optiga_deinit()
 *
 * Identity comes from the common data objects:
 *
 *   0xE0C0  Global Life Cycle Status (LcsG), 1 byte (0x07 = operational)
 *   0xE0C2  Coprocessor UID, 27 bytes — unique ID plus build and
 *           firmware information (decoded per the SRM table).
 *
 * Both objects have Read AC = ALW, so they work on factory-fresh and
 * pre-paired chips alike. (A pre-paired chip that rejects plain IFX
 * shows up as PICO_OPTIGA_EHANDSHAKE from init.)
 */

#include <pico/stdlib.h>
#include <stdio.h>

#include "pico_optiga/optiga.h"

static void print_hex(const char *label, const uint8_t *data, size_t len) {
    printf("  %-16s", label);
    for (size_t i = 0; i < len; i++) {
        printf("%02X ", data[i]);
    }
    printf("\n");
}

int main(void) {
    stdio_init_all();

    printf("== OPTIGA Trust M hello (via pico_optiga) ==\n");

    int rc = pico_optiga_init();
    if (PICO_OPTIGA_OK != rc) {
        printf("init: FAIL — %s\n", pico_optiga_strerror(rc));
        if (PICO_OPTIGA_OK == pico_optiga_probe()) {
            printf("  (probe: chip IS on the bus — the failure is in the "
                   "IFX open, not the wiring)\n");
        }
        pico_optiga_deinit();
        return 1;
    }
    printf("init: OK (app open, IFX negotiated)\n");

    pico_optiga_info_t info;
    rc = pico_optiga_get_info(&info);
    if (PICO_OPTIGA_OK != rc) {
        printf("get_info: FAIL — %s\n", pico_optiga_strerror(rc));
    } else {
        printf("lifecycle: 0x%02X%s\n", (unsigned)info.lifecycle,
               (0x07u == info.lifecycle) ? " (operational)" : "");
        printf("CIM 0x%02X   platform 0x%02X   model 0x%02X   "
               "ROM 0x%04X\n",
               (unsigned)info.cim_id, (unsigned)info.platform_id,
               (unsigned)info.model_id, (unsigned)info.rom_code);
        print_hex("chip type:", info.chip_type, 6u);
        print_hex("batch number:", info.batch_number, 6u);
        printf("wafer position: %u / %u\n", (unsigned)info.wafer_pos_x,
               (unsigned)info.wafer_pos_y);
        printf("firmware id: 0x%08lX   ESW build (BCD): %02X %02X\n",
               (unsigned long)info.fw_identifier,
               (unsigned)info.esw_build[0], (unsigned)info.esw_build[1]);
        print_hex("full UID:", info.uid, PICO_OPTIGA_UID_LEN);
    }

    pico_optiga_deinit();
    printf("done\n");
    return 0;
}
