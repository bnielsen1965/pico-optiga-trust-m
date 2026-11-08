# pico-optiga-trust-m

A C library for talking to an **Infineon OPTIGA Trust M** (SLS32AIA) secure
element from a **Raspberry Pi Pico II (RP2350)**, built on top of
[Infineon's OPTIGA Trust M Host Library for C](https://github.com/Infineon/optiga-trust-m).

The end goal is a small, stable, Pico-friendly C library (`pico_optiga`) that
applications can build on: probe the chip, read its identity, use its data
objects, and (later) offload cryptography to the secure element.

## Hardware

| Item | Detail |
|---|---|
| Host | Raspberry Pi Pico II (RP2350, Cortex-M33 @ 150 MHz, 520 KB SRAM) |
| Secure element | Infineon OPTIGA Trust M (SLS32AIA) |
| Bus | I2C0: **SDA = GP20**, **SCL = GP21** |
| I2C address | **0x30** (7-bit, factory default — datasheet Table 20/21) |
| I2C speed | 100 kHz standard / 400 kHz fast mode supported (chip also does FM+ up to 1 MHz); the Infineon library starts at 100 kHz and auto-negotiates |
| Power | Trust M I2C supply is 1.62–5.5 V, so the Pico's 3.3 V IO is fine |
| Power-on | I2C is ready **15 ms** after power-on (datasheet Table 13) |
| Datasheet | [`infineon-optiga-trust-m-sls32aia-datasheet-en.pdf`](infineon-optiga-trust-m-sls32aia-datasheet-en.pdf) (in this repo) |

Open hardware questions (to confirm on the bench):

- Are the Trust M **VDD** and **RESET** pins wired to Pico GPIOs? The Infineon
  library can do cold/warm resets through them, but they are optional — if
  they aren't connected the PAL is configured with `NULL` and we rely on the
  power-on state.
- Are there **pull-up resistors** on the I2C bus (the Trust M breakout module
  has them on board; a bare chip needs external ~4.7 kΩ pull-ups to 3.3 V)?
- **Chip state**: is this chip factory-fresh, or has a *shielded connection*
  (pairing) been applied to it? A pre-paired chip will reject plain
  (unprotected) communication with a `0x107` handshake error and would need
  Infineon's tooling to unlock it. See PLAN.md, Phase 1.

## Software stack

- **Pico SDK 2.3.1** — bare metal, single core, no OS (FreeRTOS is a possible
  later extension, not needed now).
- **Infineon OPTIGA Trust M Host Library** (submodule in
  `third_party/optiga-trust-m`) — the IFX I2C protocol stack plus the
  high-level `optiga_util` / `optiga_crypt` APIs.
- **No Mbed TLS in the initial build.** The shielded (encrypted) I2C path is
  compiled out via the absence of `OPTIGA_COMMS_SHIELDED_CONNECTION`, which
  means no crypto PAL and no TLS library. It comes back in a later phase if/
  when we want protected communication.
- Our own **PAL** (`pal/`) ports the Infineon platform-abstraction layer onto
  the Pico SDK (I2C, timers, events, logger, datastore).

## Repository layout

```
├── CMakeLists.txt            # Top-level build (pico-sdk + Infineon lib + our lib + examples)
├── CMakePresets.json         # "pico2" preset (add CMakeUserPresets.json for local SDK path)
├── README.md                 # This file — project notes
├── PLAN.md                   # Implementation plan (phases 0–6)
├── infineon-optiga-trust-m-sls32aia-datasheet-en.pdf
├── include/
│   └── pico_optiga/          # Public API of our library (Phase 4)
├── src/                      # pico_optiga implementation (Phase 4)
├── pal/                      # Pico PAL for the Infineon library (Phase 2)
├── examples/                 # One folder per example app, each with a main.c
├── third_party/
│   └── optiga-trust-m/       # Infineon host library (git submodule)
└── docs/                     # Additional notes
```

## Building

Requirements: CMake ≥ 3.22, a GNU Arm Embedded toolchain
(`arm-none-eabi-gcc`), and the Pico SDK (2.3.1).

```sh
# 1. Get the repo with the Infineon submodule
git clone --recurse-submodules <this-repo>
cd pico-optiga-trust-m
# (or, if already cloned:)
git submodule update --init third_party/optiga-trust-m

# 2. Configure — point PICO_SDK_PATH at your Pico SDK checkout,
#    or drop the SDK into third_party/pico-sdk, or let CMake fetch 2.3.1
cmake --preset pico2 -DPICO_SDK_PATH=/path/to/pico-sdk
#    (a local CMakeUserPresets.json with the SDK path already set is
#     gitignored; `cmake --preset pico2-local` works on the dev machine)

# 3. Build
cmake --build build/pico2 -j

# 4. Flash (examples appear here as .uf2 once they exist)
#    copy build/pico2/<example>.uf2 to RPI-RP2, or:
pico_build/build/pico2/<example>_flash  # pico-sdk convenience script
```

## Status

- [x] Phase 0 — CMake project assembled, README + plan written
- [ ] Phase 1 — raw I2C probe of the chip (no Infineon library yet)
- [ ] Phase 2 — Pico PAL port
- [ ] Phase 3 — first library-level communication (chip version / serial)
- [ ] Phase 4 — `pico_optiga` public API
- [ ] Phase 5 — examples on our library
- [ ] Phase 6 — optional: shielded connection, data objects, crypto

See [PLAN.md](PLAN.md) for details.

## References

- [OPTIGA Trust M Host Library (Infineon)](https://github.com/Infineon/optiga-trust-m)
- [PAL porting guide](https://github.com/Infineon/optiga-trust-m/blob/main/extras/pal/README.md)
  and the [`NEW_PAL_TEMPLATE`](https://github.com/Infineon/optiga-trust-m/tree/main/extras/pal/NEW_PAL_TEMPLATE)
- [Zephyr PAL + app (reference embedded port)](https://github.com/Infineon/optiga-trust-m-zephyr)
- [OPTIGA Trust M overview repo](https://github.com/Infineon/optiga-trust-m-overview)
  (I2C protocol doc, solution reference manual)
- [Raspberry Pi Pico SDK](https://github.com/raspberrypi/pico-sdk)
- Datasheet: `infineon-optiga-trust-m-sls32aia-datasheet-en.pdf` (local)
