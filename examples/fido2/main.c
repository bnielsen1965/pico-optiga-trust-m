// pico-optiga-fido2 — FIDO2 (CTAP2) passkey authenticator on the
// Infineon OPTIGA Trust M.
//
// Wiring: OPTIGA Trust M on I2C0, SDA GP20 / SCL GP21 (same bus as the
// vault/console apps). The USB interface is the security key's pure-HID
// FIDO descriptor (VID/PID 2E8A:000B) and nothing else — there is no
// CDC, so flash via the BOOTSEL UF2 and read debug prints on UART0
// (GPIO 0 TX / GPIO 1 RX). Optional: a user-presence button between GP10
// and GND (internal pull-up) — pressing it records a touch exactly like
// typing 'up' on the console.
//
// Startup contract (house style, deliberately NOT auto-provisioning):
// the OPTIGA must be present (pico_optiga_init opens the IFX application)
// and the FIDO2 master key (0xE0F3) must be provisioned (its public key
// cached in littlefs). If not, the app still enumerates over USB but
// answers nothing, slow-blinks the LED (GP25), and says so on UART0. It
// never provisions — `provision` on the console is a deliberate,
// one-time action.
//
// The one-key model: a single P-256 key on 0xE0F3 is shared by EVERY
// credential and the attestation. Registration mints a fresh credential
// ID and stores {credId -> userHandle} metadata (up to 32 records on
// littlefs); login signs with 0xE0F3. The private key never leaves the
// chip.

#include <stdio.h>

#include "pico/stdlib.h"
#include "pico/time.h"
#include "pico/multicore.h"

#include "tusb.h"

#include "pico_littlefs.h"

#include "pico_optiga/optiga.h"

#include "fido_hid.h"
#include "fido_optiga.h"

int main(void)
{
    // stdio_init_all() is NOT called before main by the SDK — the app
    // must call it. The only stdio driver present is UART0 (no
    // stdio-over-USB — the USB interface is the HID key).
    stdio_init_all();

    printf("pico-optiga-fido2: OPTIGA Trust M FIDO2 authenticator "
           "(CTAPHID + CTAP2 GET_INFO + MakeCredential + "
           "GetAssertion + Reset)\n");
    printf("User presence: type 'up' on the UART0 console (or press the "
           "GP10 button) while a registration or login waits\n");

    // Startup contract: chip present + FIDO2 master (0xE0F3) provisioned.
    // This app does NOT auto-provision — `provision` on the console is a
    // deliberate, one-time action.
    bool chip_present = false;
    int rc = pico_optiga_init();
    if (PICO_OPTIGA_OK == rc) {
        chip_present = true;
        printf("OPTIGA on I2C0 GP20/GP21 (application open).\n");
    } else {
        printf("OPTIGA init failed: %s — check I2C0 GP20/GP21 wiring.\n",
               pico_optiga_strerror(rc));
    }

    // Pico-side persistence (littlefs, flash @ 0x80000), shared with the
    // vault/console apps: /io_key.bin, the /v_<name> entries and the
    // /f2c_* credential records + /f2_idkey.pub + /f2_cnt all survive
    // reflashes; only a full-flash erase wipes them. Mounted here (before
    // the provisioned check, which reads the pub-key cache from it).
    if (0 == pico_lfs_init(NULL)) {
        if (0 != pico_lfs_mount_existing()) {
            printf("littlefs: no formatted volume — formatting "
                   "(one-time; can take a while)...\n");
            if (0 != pico_lfs_mount()) {
                printf("littlefs mount FAILED — credential persistence "
                       "unavailable.\n");
            } else {
                printf("littlefs: fresh volume formatted.\n");
            }
        }
    } else {
        printf("littlefs init FAILED — credential persistence "
               "unavailable.\n");
    }

    const bool provisioned = chip_present && pico_lfs_is_mounted()
                             && fido_optiga_is_provisioned();
    if (provisioned) {
        printf("OPTIGA provisioned (FIDO2 master 0xE0F3 present): "
               "CTAPHID enabled.\n");
    } else {
        printf("OPTIGA not provisioned — run `provision` on the console "
               "(then reboot) to generate the 0xE0F3 master.\n");
        printf("The device still enumerates over USB but answers nothing "
               "until it is provisioned (LED slow-blinking).\n");
    }

    fido_hid_init(provisioned);

    // Dual-core split (the makeCredential delivery fix — see fido_hid.c):
    // core 1 runs the USB task in a tight spin (tud_task + IN-report
    // re-arm) so the next report is armed the instant the host ACKs the
    // previous one; core 0 (this loop) carries the slow CTAP / OPTIGA /
    // console work at a 1 ms cadence. The two cores exchange 64-byte
    // reports through the lock-free queues set up in fido_hid_init, so
    // init must complete before core 1 is launched.
    multicore_launch_core1(fido_usb_core1_task);

    // Core 0 (the card task): 1 ms cadence, well under the 500 ms
    // keep-alive / 750 ms busy-timeout granularity. The USB re-arm is on
    // core 1, so this loop can sleep freely.
    while (1) {
        fido_card_tick();
        sleep_ms(1);
    }
}
