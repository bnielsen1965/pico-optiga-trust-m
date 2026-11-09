# Implementation Plan — OPTIGA Trust M on Pico II

Goal: a small, stable C library (`pico_optiga`) for the Raspberry Pi Pico II
(RP2350) that wraps the
[Infineon OPTIGA Trust M Host Library](https://github.com/Infineon/optiga-trust-m),
so applications can use the secure element without touching the IFX I2C
protocol. We proceed in phases; each phase ends with something flashable and
observable over the Pico's stdio (USB serial).

```
┌────────────┐   ┌─────────────┐   ┌──────────────────────┐   ┌─────────────────────────────┐
│  example   │ → │ pico_optiga │ → │  optiga_trust_m      │ → │  PAL (./pal, Pico port)     │
│  apps      │   │ (our API)   │   │  (Infineon host lib) │   │  i2c / timer / event / log  │
└────────────┘   └─────────────┘   └──────────────────────┘   └──────────────┬──────────────┘
                                                                              │ pico-sdk i2c
                                                                        ┌─────▼─────┐   I2C0
                                                                        │  RP2350   │ GP20 SDA
                                                                        │  I2C0     │ GP21 SCL
                                                                        └─────┬─────┘
                                                                              │ 0x30
                                                                        ┌─────▼─────┐
                                                                        │ OPTIGA    │
                                                                        │ Trust M   │
                                                                        └───────────┘
```

## Facts established during research

Verified against the datasheet in this repo
(`infineon-optiga-trust-m-sls32aia-datasheet-en.pdf`) and the upstream library
source:

| Fact | Value | Source |
|---|---|---|
| Default I2C address | **0x30** (7-bit) | Datasheet Table 20/21; `IFX_I2C_BASE_ADDR` in `ifx_i2c_config.h` |
| I2C modes | 100 kHz SM, 400 kHz FM, 1 MHz FM+ | Datasheet Tables 8–10 |
| I2C-ready after power-on | **15 ms** (`tSTARTUP`) | Datasheet Table 13 |
| I2C supply | 1.62–5.5 V (3.3 V IO compatible) | Datasheet Table 7 |
| Raw I2C registry | `0x80` DATA, `0x81` DATA_REG_LEN, `0x82` I2C_STATE (RO), `0x83` BASE_ADDR, `0x84` MAX_SCL_FREQU, `0x88` SOFT_RESET, `0x89` I2C_MODE | Datasheet Table 20 (Appendix A) |
| Frame size | 16–277 bytes; library default 277 | `ifx_i2c_config.h` |
| Library I2C negotiation | starts at 100 kHz, negotiates up to the configured 400 kHz; falls back to 100 kHz on failure | `ifx_i2c_config.c` |
| Shielded (encrypted) I2C | compiled out unless `OPTIGA_COMMS_SHIELDED_CONNECTION` is defined; **no Mbed TLS needed without it** | `ifx_i2c_config.h`, PAL README |
| Handshake error 0x107 | means a shielded-connection pairing exists that the host didn't complete (chip pre-paired) | upstream `examples/README.md` troubleshooting |
| PAL surface to implement | `pal_i2c` (init/deinit/read/write/set_bitrate), `pal_gpio` (VDD/RESET), `pal_os_timer`, `pal_os_event`, `pal_os_lock`, `pal_os_datastore`, `pal_logger`, `pal_ifx_i2c_config` | `extras/pal/NEW_PAL_TEMPLATE` |

## Phase 0 — Project assembly (done)

- CMake project: Pico SDK integration (local checkout / submodule / FetchContent
  fallback, pinned 2.3.1), `pico2` board default.
- Infineon host library added as git submodule `third_party/optiga-trust-m`
  and compiled into a static `optiga_trust_m` target (core sources only,
  shielded connection off).
- Directory skeleton: `include/pico_optiga`, `src`, `pal`, `examples`,
  `third_party`.
- README with hardware/software notes; this plan.

**Exit criteria:** `cmake --preset pico2-local && cmake --build build/pico2`
configures and compiles the `optiga_trust_m` static library for the RP2350
with no application code present.

## Facts established in Phase 1 (empirical, bench-verified)

Hard-won while debugging the raw probe against the bench unit
(scope-verified). **The Phase 2 PAL port must honor all of these:**

| Fact | Value | Source |
|---|---|---|
| Pico SDK `i2c_write_blocking` / `i2c_read_blocking` return | **number of bytes transferred** on success (not 0); `0` = address ACKed but data NAKed; negative = address NAK / timeout | `hardware/i2c.h`, `rp2_common/hardware_i2c/i2c.c` |
| GUARD_TIME between consecutive chip accesses | **≥ 50 µs** (STOP to next START); reference stack enforces exactly 50 µs (`PL_GUARD_TIME_INTERVAL_US`). Without it the chip NAKs the read address | Datasheet Table 24; `ifx_i2c_config.h` |
| Address ACK while busy/idle | chip does **not** acknowledge its address; host must "retry or repeat the transaction until it is successful", up to a **100 ms** budget | Datasheet A.2.1 |
| Empirical wake behavior | after reset + settle: first probe NAKs, a probe **5 ms later ACKs**; the ACK-ready window closes after ~20 ms of bus silence → scan with a tight **5 ms retry cadence**, never one shot + long pause | bench, scope |
| Warm reset (RST, active low) | low pulse **10 µs … 2.5 ms** (we use 1 ms); first transmission ≥ **tSTARTUP = 15 ms** (20 ms if NVM pending) after the rising edge; SDA/SCL high during the pulse; VCC stays powered | Datasheet 5.1.4.2, Table 14 |
| RST wiring | **GP22 → module RST** (wired). Bring the pin up as output-high *before* enabling the driver — the RP2350 GPIO OUT register defaults to 0, so a naive `gpio_set_dir(OUT)` emits a low spike on RST | bench |
| RP2350 I2C0 pin options | SDA 0/4/8/12/16/**20**/24/28, SCL 1/5/9/13/17/**21**/25/29 — GP20/21 is a valid I2C0 mapping (unlike the RP2040) | `pico-sdk/src/rp2350/rp2350a_interface_pins.json` |
| Pico SDK 2.x I2C API | `i2c_write/read_blocking(i2c, addr, data, len, nostop)`: `addr` is the **7-bit address, unshifted** (the driver writes it straight into the hw target register; `addr >= 0x80` is rejected); the last param is **`nostop`** (false = issue a STOP — opposite polarity of SDK 1.x `ack`); returns bytes transferred, `0` = addr ACKed/data NAKed, `-1` (`PICO_ERROR_GENERIC`) = addr NAK, `-3` (`PICO_ERROR_TIMEOUT`) = timeout | `pico-sdk/src/rp2_common/hardware_i2c/i2c.c` |

The IFX stack's own busy polling (`PL_POLLING_INVERVAL_US` = 1 ms) is
consistent with the observed 5 ms wake window, so the Phase 3 handshake
should inherit the behavior for free once the PAL is in place.

## Phase 1 — Raw I2C probe (no Infineon library) — **done**

The smallest possible experiment: bypass the whole protocol stack and just ask
the chip, over plain I2C, "are you there and what are your settings?".

- `examples/i2c_probe/main.c`:
  - `pico_sdk_init()`, configure **I2C0 on GP20 (SDA) / GP21 (SCL)** at 100 kHz
    (pico-sdk `i2c_init`, `gpio_function_select` happens inside `i2c_init`).
  - `i2c_scan`: sweep addresses 0x03–0x77, print responders (sanity check for
    wiring, pull-ups, and the 0x30 address).
  - Read the IFX I2C registry (datasheet Table 20):
    - `0x82` I2C_STATE (4 bytes) — device state / response length
    - `0x84` MAX_SCL_FREQU (4 bytes) — max clock in kHz
    - `0x89` I2C_MODE (2 bytes) — expected default `011` (SM & FM)
  - Print results over stdio; non-`0xFF` data at 0x30 = chip is alive.
- Register access pattern (per Appendix A.2 sample logs): write the register
  address to the slave, then read the data bytes.

**This phase also answers the open question "does the chip need to be …?"**
(see Open questions below): a live, unpaired chip answers the registry reads
and — later — the library handshake. A pre-paired/locked chip will show up
here as NAKs or as a 0x107 handshake in Phase 3.

**Exit criteria:** probe prints a sane I2C_STATE / MAX_SCL_FREQU from address
0x30 on the bench unit.

**Outcome (bench-verified):** the probe now detects 0x30 on every reset
(warm reset on GP22 at boot, scan with 5 ms retry cadence — the chip
answers on the second probe) and reads I2C_STATE / MAX_SCL_FREQU /
I2C_MODE. Every failure mode encountered is documented in the empirical
facts table above.

## Phase 2 — Pico PAL port — **done (build + bench-verified)**

Port `extras/pal/NEW_PAL_TEMPLATE` to the Pico SDK, one file in `pal/` per
template file (plus `pal_os_memory.c`, which the template omits but the
library needs). The Zephyr PAL (`Infineon/optiga-trust-m-zephyr`) is the
reference for an RTOS-free/embedded port.

| File (in `pal/`) | Maps to |
|---|---|
| `pal_ifx_i2c_config.c` | `pal_i2c_t optiga_pal_i2c_context_0` → `i2c0` (pico-sdk instance macro, *no* `&`), address `0x30`; `optiga_reset_0` → GP22; `optiga_vdd_0` → NULL hw context (VDD hardwired to 3.3 V, so the VDD toggle is a no-op) |
| `pal_i2c.c` | `pal_i2c_init/deinit/read/write/set_bitrate` → `i2c_init`, `i2c_write_blocking` / `i2c_read_blocking` (7-bit address **unshifted**, last arg `nostop=false` = issue STOP — IFX is a STOP-based protocol), `i2c_set_baudrate`; blocking driver, so the upper-layer callback is invoked inline with the mapped event |
| `pal_gpio.c` | `pal_gpio_set_high/low` → `gpio_put`, with safe bring-up (arm OUT register high *before* `gpio_set_dir(OUT)` — no low spike on RST) |
| `pal_os_timer.c` | `get_time_in_microseconds/milliseconds` → `to_us/ms_since_boot`; `delay_in_milliseconds` → `sleep_ms` |
| `pal_os_event.c` | oneshot callback registration → **software deadline**, pumped by the application (see design note below — no timer ISR) |
| `pal_os_lock.c` | single-core, no OS: trivial counter (template as-is) |
| `pal_os_datastore.c` | static RAM buffer (template as-is; shielded-connection key storage is only needed in Phase 6) |
| `pal_logger.c` | `pal_logger_write` → `fwrite` to stdio (UART0); defines `logger_console` (the library declares it `extern`) |
| `pal_os_memory.c` | `pal_os_malloc/calloc/free/memcpy/memset` → libc (template omits this file; Linux PAL is the reference) |
| `pal.c` | `pal_init`/`pal_deinit` no-ops (template as-is) |

Design notes (decisions made while porting):
- **`pal_os_event` is a software deadline, not a hardware timer alarm.**
  The stack's state machines are chains of one-shots (50 µs guard time,
  1 ms busy polling, 2 ms + 12 ms reset sequence — the vendor passes its
   "MSEC" constants straight into the µs parameter, a unit mix-up present
   in all their PALs) with at most one pending at
  a time. Firing the registered callback from a timer ISR would run blocking
  I2C and printf in ISR context. Instead: `register_callback_oneshot()`
  arms `{callback, args, deadline}`; the application's wait loop calls
  `pal_os_event_trigger_registered_callback()` (the "pump") at ~1 ms
  cadence, which matches the stack's own `PL_POLLING_INVERVAL_US`. The 50 µs
  guard stretches to ≤ ~1 ms in practice — fine, the datasheet specifies a
  minimum. The upstream `WAIT_AND_CHECK_STATUS` example macro is an *empty*
  busy-wait (works on Linux because the Linux PAL runs an event thread);
  our pump loop is the bare-metal replacement.
- **I2C failure → `PAL_I2C_EVENT_BUSY`, never hard ERROR.** An address NAK
  means the chip is busy/idle (Phase 1 facts). The PL treats BUSY and ERROR
  identically (retry after 1 ms), but BUSY is the honest mapping; the PL
  tolerates `PL_POLLING_MAX_CNT` = 200 consecutive NAKs (~200 ms) before
  declaring a fatal error — well past the measured ~5 ms wake window.
- **Link order:** the *stack* calls *into* the PAL, so `liboptiga_trust_m.a`
  must precede `libpico_optiga.a` on the final link line (single-pass
  archive resolution). `pico_optiga` therefore takes the vendor include
  dirs directly (not via a link dependency). *Superseded in Phase 4:*
  once `pico_optiga` itself calls into the stack (`optiga_util_*`), the
  two archives depend on each other and no single-pass order works —
  examples now link them in a `--start-group`/`--end-group` link group.
- **Vendor config selection:** the vendor default config
  (`optiga_lib_config_m_v3.h`) defines `OPTIGA_COMMS_SHIELDED_CONNECTION`,
  which pulls in the crypto PAL (`pal_crypt_*`, Mbed TLS). We select
  `include/optiga_lib_config_pico.h` via `OPTIGA_LIB_EXTERNAL` — based on
  v3 with the shielded connection removed and
  `OPTIGA_COMMS_DEFAULT_RESET_TYPE` set to WARM (2).

**Exit criteria:** `optiga_trust_m` + `pal` link into a trivial example
without linker errors (all `pal_*` symbols resolved).

**Outcome (bench-verified):** `examples/optiga_open` opens *and* closes the
application on the bench (`open_application: OK (0x0000)`,
`close_application: OK (0x0000)`) with the **stock vendor timing constants**
— the port carries no `ifx_i2c_config.h` patches.

Bench debugging found and fixed three PAL bugs (the vendor stack itself is
untouched):
 1. `pal_os_event` deadline check was inverted
    (`absolute_time_diff_us(deadline, now) <= 0` is true *before* the
    deadline) — the one-shot chain fired once early, then never again
    (`05fc0c0`).
 2. `pal_i2c` double-shifted the address: the Pico SDK takes the 7-bit
    address *unshifted* (the driver writes it straight into the hw target
    register and the controller appends the R/W bit); passing
    `addr << 1` put `2*addr` on the wire and the chip NAKed every
    transaction — 200 NAKs, `OPTIGA_COMMS_ERROR` (0x0102).
 3. `pal_i2c` used the SDK 2.x `nostop` parameter with 1.x polarity:
    `true` = *no* STOP, so register writes did not terminate with the STOP
    the IFX protocol requires.

Red herring, documented for the next porter: while chasing bug 2, the three
vendor timing constants were patched (RST low 1000, startup 25000, poll
5000) and the open passed — then all three were reverted to stock
(2000 / 12000 / 1000) and the open **still passes**. The datasheet margins
that motivated the patches (2 ms RST pulse vs. the 2.5 ms max, 12 ms
startup vs. the 15 ms tSTARTUP minimum) are real but not binding in
practice on this bench: the PL's 200× NAK-retry loop absorbs the 3 ms
early start, and the stock pulse (2.0–2.x ms with 1 ms pump jitter)
stays inside the window.

## Phase 3 — First library-level communication

The milestone the project is named after: *communicate with the chip* through
the real Infineon stack.

- `examples/optiga_hello/main.c` (directly on the Infineon API, before our
  wrapper exists). Steps 1, 2 and 4 below are already implemented and
  bench-verified in `examples/optiga_open` (the Phase 2 exit test) —
  extend it, don't rewrite:
  1. `optiga_util_create(0, callback, NULL)`
  2. `optiga_util_open_application(me, 0)` — the IFX I2C open: negotiates
     frame size, sets up the protocol
  3. Read identity data objects:
     - `0xE0C2` Coprocessor UID (27 bytes) — the chip's unique ID
       plus firmware id (offset 21, uint32 LE) and ESW build
       (offset 25, 2 bytes BCD). Field layout per the SRM table
       "Coprocessor UID OPTIGA Trust M Family". Read AC = ALW.
     - `0xE0C0` Global LcsG (1 byte; 0x07 = operational).
     - Note: the 0x7001/0x7002 "chip version / serial number" OIDs
       belong to the older OPTIGA TM05 / Trust (S) family. The
       Trust M exposes identity via the 0xE0C0–0xE0C6 common
       objects (confirmed in the SRM + datasheet Appendix B sample
       log, which shows the exact wire sequence for the 0xE0C2 read).
  4. `optiga_util_close_application`
  5. Print everything over stdio
- The Infineon API is **asynchronous with a quirk**: the util calls
  return `OPTIGA_LIB_SUCCESS` to mean *"accepted, in flight"* (not
  *"done"*) and complete later in the registered callback (see the
  vendor's `WAIT_AND_CHECK_STATUS` macro: pump when the return is
  SUCCESS). The instance stays locked until the completion callback
  fires — the next API called too early fails with
  `OPTIGA_UTIL_ERROR_INSTANCE_IN_USE` (0x0305). Phase 3 needs a tiny
  synchronous wait loop (pump the one-shot chain while `sleep_ms(1)`
  until the callback sets the status), exactly as the PAL template's
  init sample does. That loop becomes a proper helper in Phase 4.

**Exit criteria:** `optiga_hello` prints the chip's version and serial number.
A `0x107` here would confirm a pre-paired chip → go to the unlock path in
Open questions.

## Phase 4 — The `pico_optiga` library — **done (build + bench-verified)**

Design and implement the stable API applications will use. Header in
`include/pico_optiga/optiga.h`, implementation in `src/`.

Proposed surface (grows per phase, stays small):

```c
// Lifecycle
int pico_optiga_init(void);          // i2c0 on GP20/GP21, 100k→400k, 15 ms power-up settle
int pico_optiga_deinit(void);

// Diagnostics
int pico_optiga_probe(void);         // raw registry read (Phase 1 logic), returns 0 / -errno-ish
int pico_optiga_get_info(pico_optiga_info_t *info);  // version, serial, fw

// Data objects (Phase 5+)
int pico_optiga_read_data_obj(uint16_t id, uint8_t *buf, size_t len, size_t *out_len);
int pico_optiga_write_data_obj(uint16_t id, const uint8_t *buf, size_t len);

// Crypto (Phase 6+)
int pico_optiga_random(uint8_t *buf, size_t len);
// ... aes-ccm, ecdsa, ecc keygen, etc.
```

Design rules:
- **Synchronous** API: internally wraps the Infineon async callbacks with a
  timeout-bounded wait (default e.g. 2 s). No user-visible callbacks in v1.
- **Single-core, no OS**: not thread-safe by design; document it. A
  `critical_section` guard is cheap insurance if FreeRTOS arrives later.
- **Errors**: flat `int` codes (`0` = OK, negative = `PICO_OPTIGA_E*`), with a
  `pico_optiga_strerror()`; map Infineon `optiga_lib_status_t` once, in one
  place.
- **Memory**: static contexts (one chip, one `optiga_util_t` instance) — no
  heap allocation in the steady state. The IFX I2C context is ~1–2 KB of
  buffers; budget is comfortable against 520 KB SRAM.
- No Mbed TLS in this phase (shielded connection stays off).

**Exit criteria:** `optiga_hello` re-written against `pico_optiga` with ~10
lines of app code; unit-testable logic (status mapping, timeouts) factored so
it can be exercised natively later.

**Outcome (build + bench-verified):** `optiga_hello` is now ~15 lines of app
code (`init` → `get_info` → print → `deinit`) and the whole IFX protocol is
hidden inside the library. Bench output (the Phase 4 exit test):

```
init: OK (app open, IFX negotiated)
lifecycle: 0x07 (operational)
CIM 0xCD   platform 0x16   model 0x33   ROM 0x01CC
firmware id: 0x71101080   ESW build (BCD): 25 64
  full UID: CD 16 33 CC 01 00 1C 00 01 00 00 0A 05 C7 40 00 08 00 1C 00 2D 80 10 10 71 25 64
done
```

The chip is operational (LcsG 0x07) and not pre-paired — no 0x0107
handshake, so the plain-IFX path is the one this unit uses.

Files:
- `include/pico_optiga/optiga.h` — the public surface (lifecycle,
  diagnostics, data objects, flat `int` errors + `pico_optiga_strerror`).
- `src/optiga.c` — synchronous wrapper: RST bring-up + warm reset, I2C0 on
  GP20/21, one static `optiga_util_t`, a timeout-bounded pump
  (`OPTIGA_OP_TIMEOUT_MS` = 2 s) around each async call, a
  `critical_section` re-entrancy guard, and the raw registry `probe()`.
- `src/optiga_status.{c,h}` — the vendor `optiga_lib_status_t` →
  `PICO_OPTIGA_E*` mapping, factored as a pure function.
- `tests/` — a standalone host CMake project (the main build is
  cross-compiled, so it cannot host a native target) with a plain-assert
  unit test for the status mapping.

Design decisions made while implementing:
- **`pico_optiga_init` opens the application** (not just brings up the
  hardware), so a successful init means `get_info`/`read_data_obj` work
  immediately. On an open failure the instance stays created so
  `probe()` (wiring check) and `deinit()` remain usable — the hello app
  prints a hint when init fails but the probe succeeds.
- **`deinit` only attempts the close when the open completed**, so a
  timed-out open is not stacked with a close command on a locked instance.
- **The circular archive dependency is real now:** `pico_optiga` calls
  `optiga_util_*` (into the stack) and the stack calls `pal_*` (into
  `pico_optiga`). No single-pass order resolves both, so examples link the
  pair inside `--start-group`/`--end-group` (see the Phase 2 note).
- **pico-sdk 2.x critical-section API:** `critical_section_enter` no
  exists — it is `critical_section_enter_blocking`, and the section must
  be `critical_section_init`-ed before first use (done in
  `pico_optiga_init`). The guard's check-and-set is atomic; the pump
  itself is *not* held under the section (it blocks up to the timeout).
- **Sync-wait testability:** the status mapping is a pure function
  (native-tested). The pump/timeout loop stays target-side — it needs the
  PAL event pump and pico time, so it is verified on the bench, not
  natively. (Now bench-verified: the open + both data-object reads
  complete well inside the 2 s timeout, on the first try.)

## Phase 5 — Examples on our library

Each in `examples/<name>/`, flashable `.uf2`, stdio output. (`optiga_info`
from the original list is subsumed by Phase 4's `optiga_hello`.)

**Safety model (agreed before any write):** data objects live in tamper-
resistant NVM, so a write is *permanent* (survives power/reset) but a slot's
*contents* can be rewritten many times (bounded only by NVM endurance). Two
irreversible hazards must never be touched:

1. **Life cycle state** (global LcsG `0xE0C0`, app LcsG `0xF1C0`, or any
   object's LcsO / metadata tag `0xC0`). States are one-directional
   (creation `0x01` → init `0x03` → operational `0x07` → **termination
   `0x0F` = bricked**). We only ever *read* life cycle values.
2. **Object metadata** (the access-condition tags `0xD0` Change / `0xD1`
   Read / `0xD3` Execute). Setting Change → `NEV` freezes a slot forever.
   `pico_optiga` deliberately exposes **no metadata-write API**.

Writing a slot's *data* changes neither its LcsO nor its metadata — safe and
repeatable. So the rule is: **read first, write only data, only to a slot
confirmed open, a handful of writes max.**

Steps:

1. **`optiga_metadata` — read-only metadata inventory (Step 1).** Reads and
   decodes each candidate object's TLV metadata (LcsO, Change/Read/Execute
   AC, max/used size, data type) and prints a "safe scratch candidates"
   summary: byte-string data + LcsO=creation + Change open. Pure reads —
   nothing on the chip can change. Output is the map for picking the Step 2
   target. *Do not assume a scratch OID a priori:* on some SLS32AIA units
   `0xF1D0` ships as a locked Authorization Reference (Read=NEV), so the
   inventory is what tells us which slots are actually open.
2. **`data_objects` — write to a confirmed-open slot (Step 2).** Pick a slot
   Step 1 marked safe. Write a small marker (erase-and-write), read it back,
   reset the chip, read again → proves persistence. Data only; no metadata,
   no life cycle.
3. **`random` — pull 32 bytes of true random from the chip (Step 3).**

## Phase 6 — Optional extensions (decide per need)

In rough order of likely value:

1. **Crypto offload**: AES-128 CCM, ECDSA (P-256) sign/verify, ECC keygen —
   the core reason to have a Trust M. Straightforward `pico_optiga_*`
   wrappers over `optiga_crypt`.
2. **Shielded connection** (encrypted I2C): define
   `OPTIGA_COMMS_SHIELDED_CONNECTION`, bring in Mbed TLS 4.x (the submodule
   `external/mbedtls-4.x` in the Infineon repo) + `pal_crypt_psa.c`, pair the
   chip (upstream `example_pair_host_and_optiga_using_pre_shared_secret.c`),
   store the shared secret via `pal_os_datastore` (move it from RAM to flash
   — pico-sdk flash or littlefs — at that point).
3. **Power management**: hibernate/wake of the chip (`optiga_util` close/open
   application), sleep current reduction.
4. **FreeRTOS**: if an app needs it — the PAL lock/event files already have
   the seams; nothing in the plan blocks this.
5. **Native (host) test build**: compile `pico_optiga`'s logic + the Infineon
   library with the upstream `test_pal` on Linux for CI, with the Pico PAL
   excluded.

## Open questions

1. ~~**Chip state — "does the chip need to be …?"**~~ — **resolved (Phase 4
   bench run):** the unit opens over plain IFX with no 0x107 handshake and
   reports LcsG 0x07 (operational), so it is **unlocked and not pre-paired** —
   plain (unprotected) IFX I2C is the correct path for this chip. (Had it been
   paired to a shielded connection, init would have surfaced
   `PICO_OPTIGA_EHANDSHAKE` / 0x0107 and required Infineon unlock tooling.)
   - *Unlocked?* A factory-fresh Trust M is unlocked and works with plain
     (unprotected) IFX I2C immediately. If this chip was paired to a shielded
     connection previously, plain communication fails with a **0x107
     handshake error** and it must be unlocked with Infineon's tooling
     (the pairing tooling / `example_pair_host_and_optiga_using_pre_shared_secret`
     flow, or a factory reset if the key is lost). Phase 1 + Phase 3 tell us
     which case we're in.
   - *Initialized?* No host-side provisioning is needed for basic use; the
     only "init" is the 15 ms power-up settle and the IFX I2C open
     negotiation, both handled in software.
2. ~~**VDD / RESET wiring**~~ — **resolved**: VDD is tied to the Pico's 3.3 V
   (no hibernate MOSFET — warm resets only). **RESET is wired to GP22** and
   the firmware pulses it at boot (see Phase 1 facts) — this turned out to be
   essential for reliable operation. Debug stdio goes over UART0 (CP210x
   bridge), not USB.
3. **Pull-ups** — confirm 4.7 kΩ (or module-integrated) pull-ups to 3.3 V on
   GP20/GP21; needed for reliable 100 kHz, more important at 400 kHz.
4. **I2C speed** — start at 100 kHz; the library auto-negotiates to 400 kHz
   once the link is stable. FM+ (1 MHz) is available later if wanted.
5. **Datastore persistence** — RAM-only until Phase 6.2; fine for v1.

## Test / verification plan

- Every phase ends with a flashable example whose stdio output is the
  verification (no logic analyser needed for 1–3; an I2C sniffer is useful if
  wiring ever misbehaves).
- Phase 2 gets a native Linux smoke build of the Infineon lib + Pico PAL
  stubs (via the upstream `test_pal` pattern) to catch type/signature slips
  before flashing.
- Phase 4 adds native-compiled unit tests for the status mapping
  (done — `tests/`, plain-assert, standalone host CMake project). The
  sync-wait/pump loop needs the PAL + pico time, so it is verified on the
  bench rather than natively.
- Keep the Infineon submodule pinned; bump deliberately (its API is stable
  but the mbedtls dependency churns between branches).
