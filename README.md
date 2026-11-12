# pico-optiga-trust-m

A C library for talking to an **Infineon OPTIGA Trust M** (SLS32AIA) secure
element from a **Raspberry Pi Pico II (RP2350)**, built on top of
[Infineon's OPTIGA Trust M Host Library for C](https://github.com/Infineon/optiga-trust-m).

The library (`pico_optiga`) provides a small, stable, Pico-friendly C API that
applications can build on: probe the chip, read its identity, use its data
objects, and offload cryptography (ECDSA P-256 sign/verify, random, key
generation) to the secure element.

The `examples/fido2/` app is a complete **FIDO2 (CTAP2) passkey
authenticator** — it presents to a host as a pure-HID USB security key
(VID/PID `2E8A:000B`) and registers/logs in as a spec-compliant passkey in
modern browsers (verified on webauthn.io).

## Hardware

| Item | Detail |
|---|---|
| Host | Raspberry Pi Pico II (RP2350, Cortex-M33 @ 150 MHz, 520 KB SRAM) |
| Secure element | Infineon OPTIGA Trust M (SLS32AIA) |
| Bus | I2C0: **SDA = GP20**, **SCL = GP21** |
| I2C address | **0x30** (7-bit, factory default) |
| I2C speed | 100 kHz standard / 400 kHz fast mode (chip supports FM+ up to 1 MHz); the Infineon library starts at 100 kHz and auto-negotiates |
| Power | Trust M **VDD tied to the Pico's 3.3 V**; **RESET wired to GP22** (active low) |
| Power-on | I2C is ready **15 ms** after power-on; firmware warm-resets the chip on GP22 at boot (1 ms pulse, 25 ms settle) |
| Debug serial | **CP210x USB-UART bridge on the Pico's UART0** (GPIO0 TX / GPIO1 RX) — stdio at 115200 8N1 |
| FIDO2 button | Optional momentary switch **GP10 → GND** (internal pull-up) for user presence |
| Datasheet | [Infineon OPTIGA Trust M (SLS32AIA) datasheet](https://www.infineon.com/dgdl/Infineon-optiga_trust_m-DataSheet-SLS32AIA-DataSheets-SLS32AIA-Rev_1_00.pdf?fileId=5546d4526f3a00c4016f5a10d3e56b7b) |

Design consequences:

- PAL `optiga_vdd_0` is `NULL` (VDD is hardwired — no cold reset /
  hibernate); PAL `optiga_reset_0` is **GP22** (warm reset). The RST pin
  must be driven high *before* its output driver is enabled (the RP2350
  GPIO OUT register defaults to 0 — a naive `gpio_set_dir(OUT)` emits a
  low spike on RST).
- Examples are built with `pico_enable_stdio_uart(1)` /
  `pico_enable_stdio_usb(0)`.
- Pull-ups: the Trust M module carries them on board; the firmware also
  enables the Pico's weak internal pull-ups as a safety net.
- The PAL I2C layer uses `i2c_{read,write}_timeout_per_char_us()` (10 ms/char)
  + a bus-recovery function (re-init I2C core + re-pin GPIOs) to prevent
  hangs when the chip stalls the bus mid-transfer.

## Software stack

- **Pico SDK 2.3.1** — bare metal, single core, no OS.
- **Infineon OPTIGA Trust M Host Library** (fetched at build time from
  [GitHub](https://github.com/Infineon/optiga-trust-m) via CMake
  `FetchContent`; never vendored) — the IFX I2C protocol stack plus the
  high-level `optiga_util` / `optiga_crypt` APIs.
- **No Mbed TLS.** The shielded (encrypted) I2C path is compiled out via
  the absence of `OPTIGA_COMMS_SHIELDED_CONNECTION` — no crypto PAL, no TLS.
- Our own **PAL** (`pal/`) ports the Infineon platform-abstraction layer
  onto the Pico SDK (I2C, timers, events, logger, datastore, memory).
- **TinyUSB** (via Pico SDK) for the FIDO2 example's pure-HID CTAP2 transport.
- **pico-littlefs** (fetched at build time from
  [bnielsen1965/pico-littlefs](https://github.com/bnielsen1965/pico-littlefs))
  for the FIDO2 credential store.

## Repository layout

```
├── CMakeLists.txt            # Top-level build (module + examples)
├── CMakePresets.json         # "pico2" preset
├── LICENSE                   # MIT
├── README.md                 # This file
├── include/
│   └── pico_optiga/          # Public API (optiga.h, sha256.h)
├── src/                      # pico_optiga implementation
├── pal/                      # Pico PAL for the Infineon library
├── examples/
│   ├── i2c_probe/            # Phase 1: raw I2C bus scan
│   ├── optiga_open/          # Phase 2: IFX open/close
│   ├── optiga_hello/         # Phase 3: chip identity
│   ├── optiga_metadata/      # Phase 4: metadata read
│   ├── data_objects/         # Phase 5: data object read/write
│   ├── random/               # Phase 5: chip TRNG
│   ├── crypt/                # Phase 5: ECDSA sign/verify
│   ├── vault/                # Phase 5: PIN-gated password store
│   ├── fs_audit/             # Littlefs filesystem audit
│   └── fido2/                # FIDO2 (CTAP2) passkey authenticator
├── tests/                    # Native host unit tests (own CMake project)
├── tools/                    # Bench + debug scripts
│   ├── fido2_bench.py        # FIDO2 bench client (USB HID + serial)
│   ├── BENCH_STATUS.md       # Bench status + root-cause table
│   ├── usbmon_capture.sh     # USB monitor capture
│   └── usb_tcpdump.sh        # USB tcpdump capture
└── docs/
    └── BRINGUP.md            # OPTIGA Trust M bring-up checklist
```

## Using the module in your own project

**Primary: git submodule.**

```sh
git submodule add https://github.com/bnielsen1965/pico-optiga-trust-m.git libs/pico-optiga-trust-m
# (no nested submodules — all deps are fetched at build time)
```

In your top-level `CMakeLists.txt`:

```cmake
add_subdirectory(libs/pico-optiga-trust-m)
target_link_libraries(app PRIVATE pico_optiga::pico_optiga)
```

**Alternative: copy the files.**

| What | Where |
|---|---|
| Public headers | `include/pico_optiga/` (2 files) |
| Library source | `src/` (2 files) |
| PAL port | `pal/` (8 files) |
| CMake | The `pico_optiga` library section from `CMakeLists.txt` |
| Infineon library | Fetched at build time from [Infineon/optiga-trust-m](https://github.com/Infineon/optiga-trust-m) |
| pico-littlefs | Fetched at build time from [bnielsen1965/pico-littlefs](https://github.com/bnielsen1965/pico-littlefs) (examples only) |

## Building

Requirements: CMake ≥ 3.22, a GNU Arm Embedded toolchain
(`arm-none-eabi-gcc`), and the Pico SDK (2.3.1).

```sh
# 1. Get the repo (all dependencies are fetched at build time)
git clone https://github.com/bnielsen1965/pico-optiga-trust-m.git
cd pico-optiga-trust-m

# 2. Configure
cmake --preset pico2 -DPICO_SDK_PATH=/path/to/pico-sdk
#    (the OPTIGA Trust M host library and pico-littlefs are fetched
#     automatically from GitHub; for air-gapped builds:
#     -DOPTIGA_TRUST_M_SOURCE_DIR=/path/to/optiga-trust-m
#     -DPICO_LITTLEFS_SOURCE_DIR=/path/to/pico-littlefs)

# 3. Build all examples
cmake --build build/pico2 -j

# 4. Build just the FIDO2 authenticator
cmake --build build/pico2 --target fido2

# 5. Flash
#    copy build/pico2/<example>.uf2 to the Pico's RPI-RP2 drive
#    (hold BOOTSEL, plug in USB, release, copy, unplug, replug)
```

## FIDO2 passkey authenticator

The `examples/fido2/` app is a complete CTAP2 passkey:

- **Transport:** pure-HID USB (VID/PID `2E8A:000B`, usage page `0xF1D0`,
  two raw 64-byte reports, no report IDs). No CDC, no vendor bulk.
- **Commands:** INIT, PING, WINK, GET_INFO, MAKE_CREDENTIAL,
  GET_ASSERTION, RESET, CANCEL.
- **Attestation:** self ("packed") — the credential key signs
  `SHA-256(authData ‖ clientDataHash)`. No `x5c` certificate chain.
- **Crypto:** all ECDSA P-256 operations offloaded to the OPTIGA Trust M
  (on-chip key, on-chip sign). The private key never leaves the secure
  element.
- **Key model:** single keypair (OID 0xE0F3) for all signing. The public
  key is cached in littlefs after first generation.
- **Credential store:** 4 slots in littlefs (`/f2c_0` … `/f2c_3`),
  survives power cycles and reflashes.
- **User presence:** optional GP10 button (GND, internal pull-up) +
  console `up` command. 30 s UP timeout, 500 ms keep-alive throttle.
- **RESET window:** 10 s from power-on (CTAP 2.3 §6.6). Rejected with
  `0x30 NOT_ALLOWED` after the window.

**Bench-verified** (10/10 checks) and **browser-verified** (webauthn.io
register + authenticate). See `tools/BENCH_STATUS.md` for the full
root-cause table.

```sh
# Run the FIDO2 bench (as the user who owns /dev/ttyUSB0, no sudo)
python3 tools/fido2_bench.py run-all --no-reset --serial /dev/ttyUSB0

# In-window RESET (press the Pico RESET button when prompted)
python3 tools/fido2_bench.py post-reset --serial /dev/ttyUSB0
```

## Running the examples

1. Build: `cmake --build build/pico2 -j`
2. Copy `build/pico2/<example>.uf2` to the Pico's `RPI-RP2` drive.
3. Open the CP210x serial port at **115200 8N1**.
4. Reset the Pico (power cycle is a good reset for the Trust M too).

## Status

- [x] Phase 0 — CMake project assembled
- [x] Phase 1 — raw I2C probe (`i2c_probe`) — bench-verified
- [x] Phase 2 — Pico PAL port — bench-verified
- [x] Phase 3 — chip identity (`optiga_hello`) — bench-verified
- [x] Phase 4 — `pico_optiga` public API — bench-verified
- [x] Phase 5 — examples on the library (`data_objects`, `random`, `crypt`, `vault`) — bench-verified
- [x] Phase 7 — FIDO2 (CTAP2) passkey authenticator — bench-verified (10/10) + browser-verified

## References

- [OPTIGA Trust M Host Library (Infineon)](https://github.com/Infineon/optiga-trust-m)
- [PAL porting guide](https://github.com/Infineon/optiga-trust-m/blob/main/extras/pal/README.md)
- [Zephyr PAL + app (reference embedded port)](https://github.com/Infineon/optiga-trust-m-zephyr)
- [OPTIGA Trust M overview repo](https://github.com/Infineon/optiga-trust-m-overview)
- [Raspberry Pi Pico SDK](https://github.com/raspberrypi/pico-sdk)
- [CTAP 2.3 spec](https://fidoalliance.org/specs/fido-v2.3-ps-20260226/fido-client-to-authenticator-protocol-v2.3-ps-20260226.html)
- [W3C WebAuthn Level 3](https://www.w3.org/TR/webauthn-3/)
- Companion projects: [pico-atecc608](https://github.com/bnielsen1965/pico-atecc608)
  (ATECC608B variant), [pico-fido2](https://github.com/bnielsen1965/pico-fido2)
  (FIDO2 knowledge base)

## Licensing

MIT — see [LICENSE](LICENSE). The Infineon OPTIGA Trust M Host Library
(submodule) is under Infineon's own license and is not redistributed as
part of this repo.
