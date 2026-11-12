# OPTIGA Trust M (SLS32AIA) — Bring-up checklist

A reusable checklist for bringing up a fresh OPTIGA Trust M on the Pico II.
Assumes the hardware wiring from the README (I2C0 GP20/GP21, RST GP22,
3.3 V).

## 1. Wiring verification

| Pin | Connection | Check |
|---|---|---|
| GP20 (SDA) | Trust M SDA | Continuity to module SDA pad |
| GP21 (SCL) | Trust M SCL | Continuity to module SCL pad |
| GP22 (RST) | Trust M RESET | Active low; drive HIGH at idle |
| 3.3 V | Trust M VDD | 3.3 ± 0.1 V at the module pin |
| GND | Trust M GND | Common ground |

The Trust M module carries its own I2C pull-ups. The firmware also enables
the Pico's weak internal pull-ups as a safety net.

**RST pin caution:** the RP2350 GPIO OUT register defaults to 0. A naive
`gpio_set_dir(GP22, OUT)` emits a low spike on RST (an unexpected warm
reset). The firmware drives GP22 HIGH *before* switching it to output mode.

## 2. First flash: `i2c_probe`

```sh
cmake --build build/pico2 --target i2c_probe
# copy build/pico2/i2c_probe.uf2 to RPI-RP2
```

Open the serial port at 115200 8N1. Expected output on a healthy chip:

```
I2C bus scan: found device at 0x30
I2C_STATE      = 08 80 00 00
MAX_SCL_FREQU  = 000001F4  (500 kHz)
I2C_MODE       = 00 03     (SM & FM)
PASS
```

If the scan finds nothing at `0x30`:
- Check wiring (SDA/SCL swap is the #1 cause).
- Verify 3.3 V at the module.
- Try a power cycle (unplug USB, wait 5 s, replug).
- Check for a stuck bus: if SDA is held low, the chip needs a 9-clock
  pulse on SCL (the `pal_i2c_bus_recovery()` in this repo handles this
  automatically on the next transfer).

## 3. IFX open/close: `optiga_open`

```sh
cmake --build build/pico2 --target optiga_open
```

Expected: the IFX stack opens a connection to the chip, reads the
`LcsG` (life cycle status) register, and closes cleanly.

- `LcsG = 0x07` → operational (factory-unlocked, not pre-paired).
- `LcsG = 0x04` → sealed (production-locked; limited access).
- Open failure → check I2C wiring, power, and that no other process
  holds the bus.

## 4. Chip identity: `optiga_hello`

```sh
cmake --build build/pico2 --target optiga_hello
```

Expected: the `pico_optiga` library opens the chip, reads the metadata
object (chip ID, firmware version, key version), and prints it.

This is the first test that exercises the full stack:
PAL → IFX physical layer → IFX link layer → OPTIGA command layer →
`pico_optiga` API.

## 5. Data objects: `data_objects`

Reads and writes the chip's data objects (DOs). Confirms that the
IFX data-object read/write path works end-to-end.

## 6. Cryptography: `crypt`

Exercises ECDSA P-256 sign/verify on the chip:
- Generate a keypair (or use a pre-provisioned key).
- Sign a 32-byte digest.
- Verify the signature (on-chip).

**Important:** the OPTIGA Trust M returns ECDSA signatures as **bare
INTEGERs** (`02 <len> <r> 02 <len> <s>`) with **no** `0x30` DER
SEQUENCE header. Any signature parser must accept both the bare form
and the full DER form.

## 7. FIDO2 provisioning: `fido2`

The FIDO2 example provisions the chip on first boot:
1. Opens the IFX connection.
2. Checks for an existing 0xE0F3 keypair (cached in littlefs).
3. If not present, generates a P-256 keypair on the chip (OID 0xE0F3).
4. Caches the public key in littlefs (`/f2c_pub`).
5. Enumerates the USB HID interface.

Console commands (serial, 115200 8N1):
- `up` — simulate user presence (alternative to the GP10 button).
- `wipe` — erase the credential store (4 littlefs slots).
- `provision` — re-run the 0xE0F3 keypair generation (then reboot).
- `pin` — report the GP10 button level (wiring diagnostic).

## 8. FIDO2 bench verification

```sh
# Full bench (minus in-window RESET)
python3 tools/fido2_bench.py run-all --no-reset --serial /dev/ttyUSB0

# In-window RESET (press the Pico RESET button when prompted)
python3 tools/fido2_bench.py post-reset --serial /dev/ttyUSB0
```

Expected: 10/10 checks pass (INIT, PING, WINK, GET_INFO,
MAKE_CREDENTIAL with verified attestation signature, GET_ASSERTION ×3
with verified signatures + monotonic counter, preflight, CANCEL,
negative-RESET 0x30).

## Known failure modes

| Symptom | Likely cause | Fix |
|---|---|---|
| I2C scan finds nothing | SDA/SCL swap, no power, dead chip | Check wiring; power-cycle |
| I2C hang (superloop stuck) | Chip stalled the bus mid-transfer | `pal_i2c_bus_recovery()` handles it; power-cycle if persistent |
| IFX open fails | Bus not ready ( < 15 ms after power-on) | Ensure 25 ms settle after RST pulse |
| `0x7f` (OTHER) on makeCredential | Signature parser rejected the chip's bare-INTEGER DER | `fido_der_to_raw_rs` must accept `02 <len> r 02 <len> s` |
| RESET always allowed | `be_clock` returned 0 (e.g., `to_ms_since_boot(0)`) | Use `to_ms_since_boot(get_absolute_time())` |
| PING no reply (bench) | Kernel `usbhid` stripped the leading `0x00` | Bench must prepend `b'\x00'` to hidraw writes |
| MAKE_CRED `0x11` (bench) | Nested CTAP2 maps sent with INTEGER keys | Use TEXT keys for rp/user/pubKeyCredParams/allowList |
| MAKE_CRED `0x12` (bench) | First report `bcnt` = chunk length, not total | Set `bcnt` = total payload length |

## Per-chip identifiers to record

For each Trust M unit, record:
- **Chip ID** (from the metadata object; unique per chip).
- **Firmware version** (from the metadata object).
- **0xE0F3 public key** (64 bytes; cached in littlefs after first
  FIDO2 provisioning).
- **AAGUID** (fixed in the firmware: `65f5107f-31a6-4785-9301-6788e10ec776`;
  a demo UUID, NOT FIDO-registered).

These are stable across power cycles and reflashes (the littlefs volume
at `0x80000+` is disjoint from the app region `0x0..~0x19000`).
