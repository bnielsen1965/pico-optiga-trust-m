# pico-optiga-trust-m

A C library for talking to an **Infineon OPTIGA Trust M** (SLS32AIA) secure
element from a **Raspberry Pi Pico II (RP2350)**, built on top of
[Infineon's OPTIGA Trust M Host Library for C](https://github.com/Infineon/optiga-trust-m).

The library (`pico_optiga`) provides a small, stable, Pico-friendly C API that
applications can build on: probe the chip, read its identity, use its data
objects, and offload cryptography (ECDSA P-256 sign/verify, random, key
generation) to the secure element.

See [examples/README.md](examples/README.md) for the example applications,
including a complete FIDO2 (CTAP2) passkey authenticator.

## AI Transparency

This library was developed using AI.

I am using an AMD Radeon AI Pro R9700 32GB GPU, a quantized Qwen3.8 27B model, a custom 
build of llama.cpp, and the OpenCode coding agent.


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
| Datasheet | [Infineon OPTIGA Trust M (SLS32AIA) datasheet](https://www.infineon.com/dgdl/Infineon-optiga_trust_m-DataSheet-SLS32AIA-DataSheets-SLS32AIA-Rev_1_00.pdf?fileId=5546d4526f3a00c4016f5a10d3e56b7b) |

Design consequences:

- PAL `optiga_vdd_0` is `NULL` (VDD is hardwired — no cold reset /
  hibernate); PAL `optiga_reset_0` is **GP22** (warm reset). The RST pin
  must be driven high *before* its output driver is enabled (the RP2350
  GPIO OUT register defaults to 0 — a naive `gpio_set_dir(OUT)` emits a
  low spike on RST).
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

## Repository layout

```
├── CMakeLists.txt            # Top-level build (module + examples)
├── CMakePresets.json         # "pico2" preset
├── LICENSE                   # MIT
├── README.md                 # This file (the module)
├── include/
│   └── pico_optiga/          # Public API (optiga.h, sha256.h)
├── src/                      # pico_optiga implementation
├── pal/                      # Pico PAL for the Infineon library
├── examples/                 # Example applications (see examples/README.md)
├── tests/                    # Native host unit tests (own CMake project)
├── tools/                    # Bench + debug scripts
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

## Building

Requirements: CMake ≥ 3.22, a GNU Arm Embedded toolchain
(`arm-none-eabi-gcc`), and the Pico SDK (2.3.1).

```sh
# 1. Get the repo (all dependencies are fetched at build time)
git clone https://github.com/bnielsen1965/pico-optiga-trust-m.git
cd pico-optiga-trust-m

# 2. Configure
cmake --preset pico2 -DPICO_SDK_PATH=/path/to/pico-sdk
#    (the OPTIGA Trust M host library is fetched automatically from GitHub;
#     for air-gapped builds: -DOPTIGA_TRUST_M_SOURCE_DIR=/path/to/local)

# 3. Build the library only (no examples)
cmake --build build/pico2 --target pico_optiga -j

# 4. Build everything (library + examples)
cmake --build build/pico2 -j
```

## Host unit tests

Native (host) unit tests in `tests/` — no Pico SDK, no chip required:

```sh
cmake -S tests -B tests/build
cmake --build tests/build
ctest --test-dir tests/build
```

## References

- [OPTIGA Trust M Host Library (Infineon)](https://github.com/Infineon/optiga-trust-m)
- [PAL porting guide](https://github.com/Infineon/optiga-trust-m/blob/main/extras/pal/README.md)
- [Zephyr PAL + app (reference embedded port)](https://github.com/Infineon/optiga-trust-m-zephyr)
- [OPTIGA Trust M overview repo](https://github.com/Infineon/optiga-trust-m-overview)
- [Raspberry Pi Pico SDK](https://github.com/raspberrypi/pico-sdk)
- Companion projects: [pico-atecc608](https://github.com/bnielsen1965/pico-atecc608)
  (ATECC608B variant), [pico-fido2](https://github.com/bnielsen1965/pico-fido2)
  (FIDO2 knowledge base)

## Licensing

MIT — see [LICENSE](LICENSE). The Infineon OPTIGA Trust M Host Library is
fetched at build time and is under Infineon's own license; it is not
redistributed as part of this repo.
