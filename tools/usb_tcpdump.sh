#!/usr/bin/env bash
# Capture USB wire traffic (via tcpdump/usbmon) for the FIDO2 Pico while a
# short INIT+PING test runs. Run WITH sudo:
#   sudo bash tools/usb_tcpdump.sh
#
# Shows every interrupt IN/OUT transfer with a timestamp and length, plus
# the exact moments the stimulus sends INIT and PING. Correlating the two
# tells us definitively:
#   - PING OUT transfer to the device present  -> PING reached the device
#   - IN transfer from the device after that   -> device sent a reply
# If the PING OUT is there but no IN reply follows, it is the firmware.
set -u

PCAP=/tmp/fido2_usb.pcap
STIM=/tmp/fido2_stim.log
rm -f "$PCAP" "$STIM"

# Capture all USB buses.
tcpdump -i usbmon0 -w "$PCAP" -s 0 >/dev/null 2>&1 &
TCP_PID=$!
sleep 0.5

# The stimulus: one INIT (broadcast) + one PING on the allocated CID.
# Timestamps are logged so we can line them up with the wire trace.
cd "$(dirname "$0")/.."
python3 -u - >>"$STIM" 2>&1 <<'PY'
import os, time
from tools.fido2_bench import Fido2Hid
def ts(): return time.strftime("%H:%M:%S.") + f"{int(time.monotonic()*1000)%1000:03d}"
h = Fido2Hid(); h.open()
try:
    print(f"[{ts()}] opening done", flush=True)
    cid, proto, ver, caps = h.init()
    print(f"[{ts()}] INIT ok cid={cid:#010x}", flush=True)
    data = os.urandom(16)
    h.send_message(cid, 0x81, data)
    print(f"[{ts()}] PING SENT data={data.hex()[:8]}...", flush=True)
    t0 = time.monotonic(); got = None
    while time.monotonic() - t0 < 3.0:
        try:
            got = os.read(h.fd, 64); break
        except BlockingIOError:
            time.sleep(0.01)
    if got:
        print(f"[{ts()}] PING reply received: cmd={got[4]:#04x}", flush=True)
    else:
        print(f"[{ts()}] PING: NO REPLY (3 s)", flush=True)
finally:
    h.close()
PY

sleep 0.5
kill "$TCP_PID" 2>/dev/null
wait "$TCP_PID" 2>/dev/null

echo "=== STIMULUS TIMELINE ==="
cat "$STIM"
echo
echo "=== USB INTERRUPT TRANSFERS (all buses; len shown) ==="
# Decode the pcap. usbmon lines look like:
#   <time> USB INTERRUPT <COMPLETE|SUBMIT> <to|from> <bus>:<dev>:<ep> <len>
tcpdump -r "$PCAP" -n -s 0 -tttt 2>/dev/null \
  | grep -aE "USB INTERRUPT" \
  | tail -120 \
  || echo "(decode failed or no interrupt traffic)"
