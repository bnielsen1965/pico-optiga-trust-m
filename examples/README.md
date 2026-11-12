# Examples

Proof-of-concept applications that demonstrate the `pico_optiga` library
on real hardware. Each example is a standalone `.uf2` that flashes to the
Pico and runs a specific feature.

All examples use stdio over **UART0** (CP210x USB-serial bridge, 115200 8N1)
— not the Pico's native USB port. The FIDO2 example is the exception: it
uses the Pico's native USB for the CTAP2 HID transport, with UART0 for
the debug console.

## Building and flashing

```sh
# Build all examples
cmake --build build/pico2 -j

# Build a specific example
cmake --build build/pico2 --target fido2

# Flash: copy the .uf2 to the Pico's BOOTSEL drive
# (hold BOOTSEL, plug in USB, release, copy, unplug, replug)
cp build/pico2/<example>.uf2 /media/$USER/RPI-RP2/
```

After flashing, open the serial port at 115200 8N1 and reset the Pico.

## The examples

### `i2c_probe` — raw I2C bus scan

Scans the I2C0 bus and reports any devices found. The first test that
confirms the wiring (SDA/SCL/power) is correct.

Expected: `found device at 0x30 (Trust M)`, then a `PASS` line.

### `optiga_open` — IFX open/close

Opens an IFX connection to the chip through the full stack
(PAL → physical layer → link layer → command layer) and closes it.
Confirms the I2C protocol handshake works.

Expected: `LcsG = 0x07` (operational), then a clean close.

### `optiga_hello` — chip identity

Reads the chip's metadata object (chip ID, firmware version, key version)
through the `pico_optiga` public API. The first example that uses the
library rather than raw stack calls.

### `optiga_metadata` — full metadata dump

Reads and decodes all metadata objects from the chip.

### `data_objects` — data object read/write

Exercises the IFX data-object read/write path. Writes a test pattern to a
scratch DO and reads it back.

### `random` — chip TRNG

Requests random bytes from the chip's true random number generator and
prints them in hex.

### `crypt` — ECDSA P-256 sign/verify

Generates a keypair on the chip, signs a 32-byte digest, and verifies the
signature (on-chip). Demonstrates the full crypto offload path.

**Note:** the OPTIGA Trust M returns ECDSA signatures as bare INTEGERs
(`02 <len> r 02 <len> s`) with no `0x30` DER SEQUENCE header.

### `vault` — PIN-gated password store

A simple password store that uses the chip's crypto for AES-256-GCM
encryption. PIN-gated (the PIN is checked on-chip).

### `fs_audit` — Littlefs filesystem audit

Reads and reports the contents of the littlefs volume at flash `0x80000`.
Read-only (never writes).

### `fido2` — FIDO2 (CTAP2) passkey authenticator

A complete CTAP2 passkey authenticator. See the [FIDO2 section](#fido2-passkey-authenticator)
below.

---

## FIDO2 passkey authenticator

The `fido2` example is a complete **FIDO2 (CTAP2) passkey authenticator**
that presents to a host as a pure-HID USB security key (VID/PID `2E8A:000B`)
and registers/logs in as a spec-compliant passkey in modern browsers
(verified on webauthn.io).

### Features

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
- **User presence:** optional GP10 button (GND, internal pull-up, 30 ms
  debounce) + console `up` command. 30 s UP timeout, 500 ms keep-alive
  throttle (status 0x02 UP_NEEDED).
- **RESET window:** 10 s from power-on (CTAP 2.3 §6.6). Rejected with
  `0x30 NOT_ALLOWED` after the window.
- **AAGUID:** `65f5107f-31a6-4785-9301-6788e10ec776` (demo UUID, NOT
  FIDO-registered).

### Hardware (FIDO2-specific)

| Item | Detail |
|---|---|
| USB | Pico's native USB (the CTAP2 HID transport) |
| Button | Optional momentary switch **GP10 → GND** (internal pull-up) for user presence |
| Flash | littlefs volume at `0x80000` (384 × 4 KB = 1.5 MB) for the credential store |

### Console commands

| Command | Effect |
|---|---|
| `up` | Simulate user presence (alternative to the GP10 button) |
| `wipe` | Erase the credential store (4 littlefs slots) |
| `provision` | Re-run the 0xE0F3 keypair generation (then reboot) |
| `pin` | Report the GP10 button level (wiring diagnostic) |

### Bench verification

**Bench-verified** (10/10 checks) and **browser-verified** (webauthn.io
register + authenticate). See [`tools/BENCH_STATUS.md`](../tools/BENCH_STATUS.md)
for the full root-cause table.

```sh
# Full bench (minus in-window RESET)
python3 tools/fido2_bench.py run-all --no-reset --serial /dev/ttyUSB0

# In-window RESET (press the Pico RESET button when prompted)
python3 tools/fido2_bench.py post-reset --serial /dev/ttyUSB0
```

Bench checks:

| # | Check | What it verifies |
|---|---|---|
| a | CTAPHID INIT / PING / WINK | Transport layer |
| b | CTAP2 GET_INFO | Device capabilities |
| c | CTAP2 MAKE_CREDENTIAL | Key gen + attestation signature |
| d | CTAP2 GET_ASSERTION ×3 | On-chip sign + counter monotonicity |
| d | GET_ASSERTION preflight (up:false) | Synchronous AT-only response |
| a | CANCEL a pending MAKE_CREDENTIAL | Keep-alive cancel (0x2d) |
| e | RESET outside 10 s window | Window enforcement (0x30) |
| e | RESET inside 10 s window (post-reset) | Store wipe + re-register + login |

### Tools

| File | Purpose |
|---|---|
| `tools/fido2_bench.py` | Bench client (USB HID + serial console) |
| `tools/BENCH_STATUS.md` | Bench status + root-cause table |
| `tools/usbmon_capture.sh` | USB monitor capture (raw HID traffic) |
| `tools/usb_tcpdump.sh` | USB tcpdump capture |
