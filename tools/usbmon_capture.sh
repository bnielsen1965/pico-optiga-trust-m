#!/usr/bin/env bash
# Capture usbmon wire traffic for the FIDO2 Pico (2e8a:000b) while a
# short INIT+PING test runs. Run WITH sudo:
#   sudo bash tools/usbmon_capture.sh
# Prints a filtered trace: S/C lines for the device's IN/OUT endpoints.
set -u

LOG=/tmp/usbmon_fido2.log
: > "$LOG"

# Find the usbmon unit for the bus the device is on (bus 1 -> u1, etc.).
# We just open every uN and let grep filter.
UNITS=$(ls /sys/kernel/debug/usb/usbmon/u* 2>/dev/null)
if [ -z "$UNITS" ]; then
    echo "ERROR: usbmon not available (need: mount -t debugfs none /sys/kernel/debug)"
    exit 1
fi

pids=()
for u in $UNITS; do
    ( exec cat "$u" >> "$LOG" 2>/dev/null ) &
    pids+=($!)
done

sleep 0.3

# The stimulus: one INIT (broadcast) + one PING on the allocated CID.
cd "$(dirname "$0")/.."
python3 -u - <<'PY' 2>&1
import os, time
from tools.fido2_bench import Fido2Hid
h = Fido2Hid(); h.open()
try:
    cid, proto, ver, caps = h.init()
    print(f"[stimulus] INIT ok cid={cid:#010x}")
    data = os.urandom(16)
    h.send_message(cid, 0x81, data)
    print(f"[stimulus] PING sent; waiting 3s for a reply...")
    t0 = time.monotonic(); got = None
    while time.monotonic() - t0 < 3.0:
        try:
            got = os.read(h.fd, 64); break
        except BlockingIOError:
            time.sleep(0.01)
    if got:
        print(f"[stimulus] PING reply received: cmd={got[4]:#04x}")
    else:
        print("[stimulus] PING: NO REPLY (3 s)")
finally:
    h.close()
PY

sleep 0.5
for p in "${pids[@]}"; do kill "$p" 2>/dev/null; done
wait 2>/dev/null

echo
echo "=== filtered usbmon trace (64-byte HID transfers) ==="
# usbmon 'S' (submit) / 'C' (complete) lines for interrupt IN/OUT transfers
# carrying a 64-byte HID report. Our device is the only 64-byte HID on the
# bus, so this isolates the CTAPHID traffic.
grep -aE " (S|C) (in-0x|out-0x).* 64/0 " "$LOG" \
  | tail -80 \
  || echo "(no 64-byte in/out endpoint lines captured)"
