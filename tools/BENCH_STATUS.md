# FIDO2 bench — Phase 7.2 status: FULLY GREEN (10/10 checks pass)

Last updated: 2026-11-11. The full bench now passes end-to-end on the device:

```
(a) CTAPHID INIT / PING / WINK          PASS
(b) CTAP2 GET_INFO                       PASS
(c) CTAP2 MAKE_CREDENTIAL                PASS  (packed self-attestation sig VERIFIED)
(d) CTAP2 GET_ASSERTION (×3)             PASS  (ES256 sig VERIFIED, counter 0→1→2)
(d) GET_ASSERTION preflight (up:false)   PASS
(a) CANCEL a pending MAKE_CREDENTIAL     PASS  (0x2d KEEPALIVE_CANCEL)
(e) CTAP2 RESET outside the 10 s window  PASS  (0x30 NOT_ALLOWED)
==============================================================
RESULT: 10/10 checks passed
```

Run it (as `burnin`, no sudo):

```
python3 tools/fido2_bench.py run-all --no-reset --serial /dev/ttyUSB0
```

The in-window RESET (`reset_section`: wipe store, re-register, login) is a
separate step that needs a physical Pico RESET-button press:

```
python3 tools/fido2_bench.py post-reset --serial /dev/ttyUSB0
```

## Root causes — summary

Six distinct bugs were found and fixed. Three were **bench-client** bugs (the
firmware was already correct), three were **firmware** bugs.

| # | Symptom | Side | Root cause | Fix |
|---|---------|------|-----------|-----|
| 1 | PING no reply | bench | kernel `usbhid` strips a leading `0x00` byte | `write_pkt` prepends `b'\x00'` |
| 2 | MAKE_CRED `0x11` | bench | nested CTAP2 maps sent with INTEGER keys | send TEXT keys in nested maps |
| 3 | MAKE_CRED `0x12` | bench | first report `bcnt` = chunk len, not total | `_first_pkt` writes total length |
| 4 | I2C read hang | **firmware** | Pico SDK `i2c_*_blocking` has an unbounded TX-FIFO-space loop, no timeout | `pal_i2c_bus_recovery()` + `*_timeout_per_char_us` in `pal_i2c.c` |
| 5 | MAKE_CRED `0x7f` | **firmware** | chip returns the ECDSA sig as bare INTEGERs (`02 <len> r 02 <len> s`), **no `0x30` SEQUENCE header**; `fido_der_to_raw_rs` required `der[0]==0x30` | `fido_der_to_raw_rs` now accepts both the SEQUENCE-wrapped and the bare-INTEGER form |
| 6 | RESET allowed outside the 10 s window | **firmware** | `be_clock` returned `to_ms_since_boot(0)`, which the Pico SDK defines as *"convert a timestamp to ms-since-boot"* → **always 0** → the `clock > 10000` window check was a no-op | `be_clock` now returns `to_ms_since_boot(get_absolute_time())` (the real uptime; matches the reference's `osal_time_millis()`) |

Plus two bench-side **signature-verification** bugs (the firmware signed
correctly; the bench verified against the wrong input):

| Symptom | Side | Root cause | Fix |
|---------|------|-----------|-----|
| Attestation `InvalidSignature` | bench | bench verified over `authData ‖ SHA256(CBOR(attStmt))` | recompute `cdh = client_data_hash("webauthn.create", …)`; `sig_input = authData ‖ cdh` |
| Assertion `InvalidSignature` | bench | bench **double-hashed** the clientDataHash | single hash; `sig_input = authData ‖ cdh` |

And one bench-side **timing** fix: the negative-RESET test now pads to >11 s
since bench start (the Pico reboots on reflash and the earlier tests can finish
in <10 s, so the 10 s window may not have expired yet).

> **Assertion + attestation both sign `authData ‖ clientDataHash`** (authData
> FIRST), per W3C WebAuthn L3 §6.3.3 step 11 / §7.2 step 21 and the reference
> (`pico-atecc608/example/fido2/fido_ctap.c:1065`). Do NOT "fix" the assertion
> to the reversed order — the firmware is reference-identical here.

## Root cause #1 (bench) — PING no reply: the kernel `usbhid` dropped PING's first byte
The Linux kernel's `usbhid_output_report` (`drivers/hid/usbhid/hid-core.c:950`)
strips the first byte of a hidraw write **only when that byte is `0x00`**:

    if (buf[0] == 0x0) {   /* Don't send the Report ID */
        buf++;
        count--;
    }

This device has no report IDs (raw 64-byte IN/OUT), and the firmware accepts
**only exactly-64-byte** reports (`tud_hid_set_report_cb`,
`examples/fido2/fido_hid.c`: `if (FIDO2_REPORT_LEN != bufsize) return;`).

The old `write_pkt` wrote the 64 report bytes raw, with no leading `0x00`:
- INIT: broadcast CID `0xFFFFFFFF` → first byte `0xFF` → kernel does NOT strip
  → 64 bytes reach the wire → **INIT works**.
- PING: allocated CID `0x00000002` → first byte `0x00` → kernel strips it →
  63 bytes reach the wire → firmware drops it → **no reply**.

Every allocated CID (`0x00000002` …) starts with `0x00`, so every non-broadcast
command lost its first byte on the way out.

**Fix:** `write_pkt` (`tools/fido2_bench.py`) now prepends a `0x00` the kernel
always consumes, so the device always sees the full 64 bytes:
`os.write(self.fd, b'\x00' + pkt)`. Matches the proven reference client
(`pico-fido2/bench/fido_probe.py:111`).

## Root cause #2 (bench) — MAKE_CREDENTIAL `0x11` = CBOR_UNEXPECTED_TYPE: integer keys for the nested maps
**Note on this firmware's status codes:** they are NOT the usual CTAP2 values.
Here `0x11` = `CBOR_UNEXPECTED_TYPE`, `0x12` = `INVALID_CBOR`,
`0x14` = `MISSING_PARAMETER`, `0x26` = `UNSUPPORTED_ALGORITHM`,
`0x2C` = `INVALID_OPTION`, `0x30` = `NOT_ALLOWED`, `0x7f` = `OTHER`
(see the enum at `fido_ctap.h:61`).

The CTAP2 wire format is **mixed**: INTEGER keys at the top level, **TEXT keys
inside the nested maps** (`rp`, `user`, `pubKeyCredParams[]`, `allowList[]`).
The bench sent INTEGER keys for those nested maps; the firmware's parser
(`fido_ctap.c`) reads them as TEXT keys (identical to the `pico-atecc608`
reference and to what real browsers put on the wire) and rejects a non-TEXT map
key with `CBOR_UNEXPECTED_TYPE` (`0x11`).

**Fix (bench-only):** send TEXT keys for all nested maps (4 sites:
`make_credential`, the `get_assertion` allowList, and the two cancel-test
makeCredential requests). The `pubKeyCredParams` entry now encodes as
`a2 63"alg" 26 64"type" 6a"public-key"` — byte-identical to the reference bench.

## Root cause #3 (bench) — MAKE_CREDENTIAL `0x12` = INVALID_CBOR: `bcnt` was the first-chunk length, not the total
`send_message`'s first report set `bcnt = len(first_chunk)`, not the **total**
payload length. The firmware reassembles exactly `bcnt` bytes, so a 150-byte
makeCredential payload was truncated to 57 → a torn CBOR → `INVALID_CBOR`.
Single-report commands (PING 33 B, GET_INFO 1 B) never exposed it.

**Fix (bench-only):** `_first_pkt` now takes the total length and writes
`bcnt = total`; `send_message` passes `len(data)`. A 150-B payload now frames
as 3 reports with `bcnt = 150`.

## Root cause #4 (firmware) — I2C read hang: the Pico SDK's `i2c_*_blocking` has an unbounded TX-FIFO-space loop
`i2c_read_blocking_internal()` contains an unbounded
`while (!i2c_get_write_available(i2c)) tight_loop_contents();` loop (no
timeout check) that waits for TX-FIFO space. If the Trust M stalls the bus
mid-transfer (clock-stretch overrun, a NAK mid-burst), that loop spins forever
and the whole superloop hangs. Observed on the 0xE0F3 sign's final DATA-register
read.

**Fix (firmware, `pal/pal_i2c.c`):**
- `pal_i2c_bus_recovery(i2c)` — before every transfer, if the TX FIFO is stuck
  (no write available), re-init the I2C core and re-pin the GPIOs so the next
  transfer starts clean (a single register read in the normal case).
- Switch `i2c_read_blocking`/`i2c_write_blocking` to
  `i2c_read_timeout_per_char_us`/`i2c_write_timeout_per_char_us`
  (`PAL_I2C_TIMEOUT_PER_CHAR_US = 10000`), so a stalled byte maps to
  `PICO_ERROR_TIMEOUT` (reported as `PAL_I2C_EVENT_BUSY`) and the IFX physical
  layer retries on its 1 ms cadence instead of hanging.

## Root cause #5 (firmware) — MAKE_CREDENTIAL `0x7f`: the chip returns a headerless DER signature
The OPTIGA Trust M returns the ECDSA signature as **bare INTEGERs**
(`02 <len> <r> 02 <len> <s>`) with **no `0x30` SEQUENCE header** (observed
`der_len` 68/69/70 = 2+32/33 + 2+33/32, `der[0] == 0x02`).
`fido_der_to_raw_rs` required `der[0] == 0x30` → returned `false` → `be_sign`
returned `-1` → `0x7f` (OTHER), even though the chip signed fine.

**Fix (firmware, `examples/fido2/fido_der.c`):** `fido_der_to_raw_rs` now
accepts **both** the full DER form (SEQUENCE-wrapped) and the chip's
bare-INTEGER form. (`der_len` varying 68/69/70 per signature is normal ECDSA
DER — r/s high-bit padding.)

> The earlier "stack overflow" theory (2 KB→3 KB `PICO_STACK_SIZE` experiments)
> was a **red herring** — the sign's `canary` check passed, and the DER-form
> fix (not a bigger stack) resolved the `0x7f`. The stack is back at the SDK
> default 2 KB; the `PICO_STACK_SIZE` CMake block was removed.

## Root cause #6 (firmware) — RESET allowed outside the 10 s window: `be_clock` always returned 0
CTAP 2.3 §6.6 requires RESET to be rejected once >10 s have elapsed since
power-on. The check is `be_clock() > FIDO_CTAP_RESET_WINDOW_MS (10000)`.
`be_clock` (the OPTIGA key backend) returned `to_ms_since_boot(0)`.

The Pico SDK's `to_ms_since_boot` is:

    /*! \brief Convert a timestamp into a number of milliseconds since boot. */
    static inline uint32_t to_ms_since_boot(absolute_time_t t) {
        uint64_t us = to_us_since_boot(t);
        return us_to_ms(us);
    }

It **converts a timestamp** to ms-since-boot — it is *not* "ms elapsed since
`t`". So `to_ms_since_boot(0)` is **always 0**, the window check
`0 > 10000` was always false, and RESET was always allowed. (The ATECC608
reference uses `osal_time_millis()` = `to_ms_since_boot(get_absolute_time())`,
which is correct; the OPTIGA port regressed to `to_ms_since_boot(0)`.)

**Fix (firmware, `examples/fido2/fido_optiga.c`):** `be_clock` now returns
`to_ms_since_boot(get_absolute_time())` — the real uptime — matching the
reference. Confirmed on device: the negative-RESET test now gets `0x30
NOT_ALLOWED`, and the dispatch clock reads a live, increasing value.

## Attestation + assertion verification (bench) — the bench hashed the wrong input
The firmware signs `authData ‖ clientDataHash` for **both** the attestation and
the assertion (authData FIRST). Two bench bugs made the verification fail even
though the firmware signed correctly:
- **Attestation:** the bench verified over `authData ‖ SHA256(CBOR(attStmt))`.
  Fixed to recompute `cdh = client_data_hash("webauthn.create", challenge,
  {"rp": …})` and use `sig_input = authData ‖ cdh`.
- **Assertion:** the bench **double-hashed** the clientDataHash
  (`hashlib.sha256(client_data_hash(…))`). Fixed to a single hash.

## RESET timing (bench) — the negative test ran inside the window
The Pico reboots on reflash and the earlier tests can finish in <10 s, so the
10 s window may not have expired by the time the negative-RESET test ran. The
bench now records `start_time` in `run_all` and pads to >11 s before the
negative test.

## Files
- `tools/fido2_bench.py` — bench client (uncommitted). Subcommands: run-all
  [--no-reset], init, ping, wink, get-info, make-credential, get-assertion,
  cancel-test, reset, reset-now, post-reset. Fixes: `write_pkt` 0x00 prepend,
  TEXT nested-map keys (4 sites), `_first_pkt` total `bcnt`, attestation
  `sig_input = authData ‖ cdh`, assertion single-hash `sig_input = authData
  ‖ cdh`, negative-RESET >11 s pad, `console.report(60)`.
- `examples/fido2/fido_der.c` — `fido_der_to_raw_rs` bare-INTEGER form (KEEP).
- `examples/fido2/fido_optiga.c` — `be_clock` → `to_ms_since_boot(get_absolute_time())` (KEEP).
- `pal/pal_i2c.c` — `pal_i2c_bus_recovery()` + `*_timeout_per_char_us` (KEEP).
- `examples/fido2/fido_ctap.c` — assertion-signing comment (spec reference only;
  code is reference-identical).
- `pico-atecc608/example/fido2/` — the known-good reference baseline
  (`fido_ctap.c:1065` assertion order, `fido_hid.c:508` `be_clock` =
  `osal_time_millis()`).

## Commands cheat-sheet
```
# locate node (re-do after every Pico RESET)
grep -l 00002E8A /sys/class/hidraw/hidraw*/device/uevent
# full sequence minus in-window RESET (as burnin, no sudo)
python3 tools/fido2_bench.py run-all --no-reset --serial /dev/ttyUSB0
# in-window RESET (press the Pico RESET button when prompted)
python3 tools/fido2_bench.py post-reset --serial /dev/ttyUSB0
# build
cmake --build build/pico2-local --target fido2
```

## Next steps
1. Run the in-window RESET (`post-reset`) to exercise `reset_section`
   (store wiped, re-register, login) — needs the physical device + a Pico
   RESET-button press.
2. Browser end-to-end on webauthn.io.
3. Commit `tools/` + the three firmware fixes (`fido_der.c`, `fido_optiga.c`,
   `pal/pal_i2c.c`) + the `fido_ctap.c` comment.
