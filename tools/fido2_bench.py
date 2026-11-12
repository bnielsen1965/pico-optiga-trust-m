#!/usr/bin/env python3
"""fido2_bench.py — bench client for the pico-optiga FIDO2 key.

Drives the OPTIGA Trust M FIDO2 authenticator (examples/fido2, PLAN.md
Phase 7.2) over USB HID (CTAPHID) and, in parallel, operates the UART0
debug console for user presence (the 'up' command) and console logging.

Sub-steps covered (PLAN.md 7.2 bench verification):
  (a) CTAPHID transport: INIT / PING / WINK / CANCEL
  (b) CTAP2 GET_INFO
  (c) CTAP2 MAKE_CREDENTIAL (packed self-attestation, signature verified)
  (d) CTAP2 GET_ASSERTION (on-chip sign, signature verified, sync counter
      strictly increasing)
  (e) CTAP2 RESET (10-second power-on window; a Pico RESET-button press
      is needed for the positive test)

Wire notes (see examples/fido2/fido_ctaphid.h + fido_ctap.h):
  * 64-byte raw HID reports, no report IDs, FIDO usage page 0xF1D0.
  * First packet: CID(4,BE) | CMD(1, 0x80|cmd) | BCNT(2,BE) | DATA(57);
    continuations: CID(4,BE) | SEQ(1) | DATA(59).
  * Keep-alive (device -> host): [CID][0xBB][0x00 0x01][status].
  * CTAP2 success response: [0x00 status][CBOR payload]; failure: a
    single status byte.
  * makeCredential response map: {1: "packed", 2: authData,
    3: {"alg": -7, "sig": DER}} (the v2.1 layout the firmware emits —
    see the fido_ctap.c section note).

Usage:
  python3 tools/fido2_bench.py run-all [--serial /dev/ttyUSB0]
  python3 tools/fido2_bench.py init | ping | wink | get-info
  python3 tools/fido2_bench.py make-credential | get-assertion
  python3 tools/fido2_bench.py cancel-test
  python3 tools/fido2_bench.py reset          # expect 0x30 outside the window
  python3 tools/fido2_bench.py reset-now      # press Pico RESET first
"""

import argparse
import base64
import hashlib
import json
import os
import random
import sys
import threading
import time
from collections import deque

# Heavy / optional deps (pyserial, cbor2, cryptography) are imported lazily —
# as a LOCAL `import` inside each function that actually uses them — so the
# USB-only paths (init/ping/wink) run without cbor2/cryptography, and nothing
# needs them until the console / credential-verification code runs.
#
# NOTE: a module-level PEP 562 __getattr__ does NOT cover references to these
# names *inside* this module — a global name lookup (LOAD_GLOBAL) does not
# consult it, so `serial.Serial(...)` etc. would raise NameError. The imports
# are therefore local, at the point of use. __getattr__ below is kept only for
# external `fido2_bench.<name>` attribute access.
def __getattr__(name):
    if name == "serial":
        import serial
        return serial
    if name == "cbor2":
        import cbor2
        return cbor2
    if name == "ec":
        from cryptography.hazmat.primitives.asymmetric import ec
        return ec
    if name == "hashes":
        from cryptography.hazmat.primitives import hashes
        return hashes
    raise AttributeError(f"module {__name__!r} has no attribute {name!r}")

VID, PID = 0x2E8A, 0x000B
BROADCAST = 0xFFFFFFFF

# CTAPHID command bytes (0x80 | cmd)
CMD_PING, CMD_INIT, CMD_WINK = 0x81, 0x86, 0x88
CMD_CBOR, CMD_CANCEL = 0x90, 0x91
CMD_KEEPALIVE = 0xBB

# CTAP2 INS values
INS_MAKE_CREDENTIAL, INS_GET_ASSERTION = 0x01, 0x02
INS_GET_INFO, INS_RESET = 0x04, 0x07

# CTAP2 status codes
CTAP_OK = 0x00
CTAP_ERR_KEEPALIVE_CANCEL = 0x2D
CTAP_ERR_NO_CREDENTIALS = 0x2E
CTAP_ERR_NOT_ALLOWED = 0x30
CTAP_ERR_OTHER = 0x7F

# Keep-alive statuses
KA_PROCESSING, KA_UP_NEEDED = 0x01, 0x02

RP_ID = "bench.local"
ORIGIN = "http://localhost:8080"


def hexd(b, width=16):
    return " ".join(f"{x:02x}" for x in b[:width]) + ("…" if len(b) > width else "")


def b64(b):
    return base64.urlsafe_b64encode(b).decode()


# ---------------------------------------------------------------------------
# UART0 console: background log + 'up' / 'provision' injection
# ---------------------------------------------------------------------------

class SerialConsole:
    def __init__(self, port):
        import serial
        self.s = serial.Serial(port, 115200, timeout=0.2)
        self.lines = deque(maxlen=400)
        self.lock = threading.Lock()
        threading.Thread(target=self._reader, daemon=True).start()

    def _reader(self):
        buf = b""
        while True:
            d = self.s.read(512)
            if not d:
                continue
            buf += d
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                with self.lock:
                    self.lines.append(line.decode("utf-8", "replace"))

    def send(self, line):
        with self.lock:
            self.lines.append(f"<<< {line}")
        self.s.write((line + "\n").encode())

    def report(self, n=20):
        with self.lock:
            return list(self.lines)[-n:]


# ---------------------------------------------------------------------------
# USB HID + CTAPHID transport
# ---------------------------------------------------------------------------

class ProtocolError(Exception):
    pass


class Fido2Hid:
    """The key over hidraw — the same access path the browser uses
    (WebHID on Linux), so no udev rules and no kernel-driver detaching."""

    HID_ID = f"0003:{VID:08X}:{PID:08X}"   # bus:vid:pid in sysfs

    def __init__(self):
        self.fd = None
        self.path = None
        self.cid = 0

    @classmethod
    def find_node(cls):
        try:
            entries = os.listdir("/sys/class/hidraw")
        except OSError:
            return None
        for entry in sorted(entries):
            try:
                with open(f"/sys/class/hidraw/{entry}/device/uevent") as f:
                    for line in f:
                        if line.startswith("HID_ID=") and \
                                line.strip().split("=", 1)[1] == cls.HID_ID:
                            return f"/dev/{entry}"
            except (OSError, IndexError):
                continue
        return None

    def open(self):
        self.path = self.find_node()
        if self.path is None:
            raise RuntimeError(f"no hidraw node for {VID:04x}:{PID:04x}")
        # O_NONBLOCK: select()/poll() do NOT time out on this hidraw fd
        # (they block past the deadline), so reads are done with an
        # os.read + EAGAIN poll loop instead. The kernel still buffers
        # reports in the hidraw read queue, so nothing is lost.
        self.fd = os.open(self.path, os.O_RDWR | os.O_NONBLOCK)
        self.cid = 0

    def close(self):
        if self.fd is not None:
            os.close(self.fd)
            self.fd = None

    def reopen(self):
        """Re-find the device (after a Pico reset re-enumeration)."""
        self.close()
        for _ in range(50):
            if self.find_node() is not None:
                break
            time.sleep(0.2)
        else:
            raise RuntimeError("device did not re-enumerate")
        self.open()

    # -- raw reports ---------------------------------------------------------

    def write_pkt(self, pkt):
        assert len(pkt) == 64
        # hidraw write path (kernel usbhid_output_report): the leading byte is
        # consumed as the report ID and NOT sent to the wire ONLY when it is
        # 0x00; any other first byte is sent verbatim. This device has no
        # report IDs and the firmware accepts ONLY exactly-64-byte reports
        # (tud_hid_set_report_cb drops anything else). So we must always
        # prepend a 0x00 that the kernel strips — matching the proven
        # pico-fido2 bench client. Without it, a report whose first byte is
        # 0x00 (every allocated CID 0x00000002..) loses that byte and arrives
        # as 63 bytes, which the firmware silently drops. (The broadcast INIT
        # CID 0xFFFFFFFF starts with 0xFF, so it slipped through — which is
        # why only INIT appeared to work.)
        os.write(self.fd, b'\x00' + pkt)

    def read_pkt(self, timeout_ms):
        # Poll the non-blocking fd until a full 64-byte report is queued.
        # os.read on hidraw can return a PARTIAL report (fewer than 64
        # bytes), so accumulate until we have a whole one — a short read
        # returned as-is would misalign every subsequent report.
        deadline = time.monotonic() + timeout_ms / 1000.0
        buf = b""
        while len(buf) < 64:
            try:
                chunk = os.read(self.fd, 64 - len(buf))
            except BlockingIOError:
                if time.monotonic() >= deadline:
                    raise TimeoutError(
                        f"no IN report within {timeout_ms} ms")
                time.sleep(0.002)
                continue
            if not chunk:
                raise ProtocolError("hidraw node closed")
            buf += chunk
        return buf

    @staticmethod
    def _first_pkt(cid, cmd, data, total):
        pkt = bytearray(64)
        pkt[0:4] = cid.to_bytes(4, "big")
        pkt[4] = cmd
        # bcnt is the TOTAL payload length (across all reports), not just
        # this first chunk — the firmware reassembles exactly bcnt bytes.
        # A single-report message has total == len(chunk), which is why the
        # old len(data) only broke multi-report commands (makeCredential).
        pkt[5:7] = total.to_bytes(2, "big")
        pkt[7:7 + len(data)] = data
        return bytes(pkt)

    @staticmethod
    def _cont_pkt(cid, seq, data):
        pkt = bytearray(64)
        pkt[0:4] = cid.to_bytes(4, "big")
        pkt[4] = seq
        pkt[5:5 + len(data)] = data
        return bytes(pkt)

    def send_message(self, cid, cmd, data):
        """Frame and send a CTAPHID message (any length <= 7609)."""
        off = 0
        first = True
        seq = 0
        total = len(data)
        while off < len(data) or first:
            if first:
                chunk = data[off:off + 57]
                self.write_pkt(self._first_pkt(cid, cmd, chunk, total))
                first = False
            else:
                chunk = data[off:off + 59]
                self.write_pkt(self._cont_pkt(cid, seq, chunk))
                seq = (seq + 1) & 0x7F
            off += len(chunk)

    def read_message(self, want_cmd, cid, on_keepalive=None, timeout=15.0):
        """Read one full response message; skip keep-alives (hooked)."""
        deadline = time.monotonic() + timeout
        first = None
        while True:
            rem = deadline - time.monotonic()
            if rem <= 0:
                raise TimeoutError(f"no {want_cmd:#04x} response within "
                                   f"{timeout:.0f} s")
            pkt = self.read_pkt(max(100, int(rem * 1000)))
            b4 = pkt[4]
            if b4 == CMD_KEEPALIVE:
                if int.from_bytes(pkt[0:4], "big") == cid:
                    if want_cmd == CMD_KEEPALIVE:
                        return bytes([pkt[7]])
                    if on_keepalive is not None:
                        on_keepalive(pkt[7])
                continue
            if b4 & 0x80:
                if b4 == want_cmd and int.from_bytes(pkt[0:4], "big") == cid:
                    first = pkt
                    break
                print(f"  [ignoring unsolicited message cmd={b4:#04x} "
                      f"cid={int.from_bytes(pkt[0:4], 'big'):#010x}]")
                continue
            # a continuation before we saw the first packet: drop it
        bcnt = int.from_bytes(first[5:7], "big")
        data = bytearray(first[7:64])
        while len(data) < bcnt:
            rem = deadline - time.monotonic()
            if rem <= 0:
                raise TimeoutError("response truncated")
            cpkt = self.read_pkt(max(100, int(rem * 1000)))
            if cpkt[4] & 0x80:
                raise ProtocolError(f"new message mid-response "
                                    f"(cmd={cpkt[4]:#04x})")
            data += cpkt[5:64]
        return bytes(data[:bcnt])

    # -- CTAPHID commands ------------------------------------------------------

    def init(self, timeout=5.0):
        """Broadcast INIT; returns (cid, proto, (maj, min, build), caps)."""
        nonce = os.urandom(8)
        self.send_message(BROADCAST, CMD_INIT, nonce)
        data = self.read_message(CMD_INIT, BROADCAST, timeout=timeout)
        if len(data) != 17 or data[0:8] != nonce:
            raise ProtocolError(f"INIT reply malformed: {hexd(data)}")
        cid = int.from_bytes(data[8:12], "big")
        proto = data[12]
        ver = tuple(data[13:16])
        caps = data[16]
        self.cid = cid
        return cid, proto, ver, caps

    def ping(self, data, timeout=5.0):
        self.send_message(self.cid, CMD_PING, data)
        r = self.read_message(CMD_PING, self.cid, timeout=timeout)
        return r

    def wink(self, ledger=1, timeout=5.0):
        self.send_message(self.cid, CMD_WINK, bytes([ledger]) + os.urandom(8))
        r = self.read_message(CMD_WINK, self.cid, timeout=timeout)
        return r

    def cancel(self):
        self.send_message(self.cid, CMD_CANCEL, b"")

    def cbor(self, payload, on_keepalive=None, timeout=30.0):
        """Send a CTAPHID_CBOR message; return the reassembled response
        ([0x00 + CBOR] on success, a single status byte on failure)."""
        self.send_message(self.cid, CMD_CBOR, payload)
        return self.read_message(CMD_CBOR, self.cid, on_keepalive, timeout)


# ---------------------------------------------------------------------------
# User-presence helper: fires 'up' on the console at the first UP_NEEDED
# keep-alive of each transaction.
# ---------------------------------------------------------------------------

class UpTrigger:
    def __init__(self, console, verbose=True):
        self.console = console
        self.verbose = verbose
        self.fired = False

    def __call__(self, status):
        if status == KA_UP_NEEDED and not self.fired:
            self.fired = True
            if self.verbose:
                print("  keep-alive UP_NEEDED -> console 'up'")
            self.console.send("up")


# ---------------------------------------------------------------------------
# CTAP2 commands + verification
# ---------------------------------------------------------------------------

def cose_to_pubkey(cose_bytes):
    """COSE_Key (CBOR map {1: kty, 3: alg, -1: crv, -2: x, -3: y})
    -> cryptography EC public key (P-256)."""
    import cbor2
    from cryptography.hazmat.primitives.asymmetric import ec
    d = cbor2.loads(cose_bytes)
    if d.get(1) != 2:
        raise ProtocolError(f"COSE kty {d.get(1)} != 2 (EC2)")
    if d.get(-1) != 1:
        raise ProtocolError(f"COSE crv {d.get(-1)} != 1 (P-256)")
    x, y = d[-2], d[-3]
    nums = ec.EllipticCurvePublicNumbers(int.from_bytes(x, "big"),
                                         int.from_bytes(y, "big"),
                                         ec.SECP256R1())
    return nums.public_key()


def verify_es256(pub, data, der_sig):
    from cryptography.hazmat.primitives.asymmetric import ec
    from cryptography.hazmat.primitives import hashes
    pub.verify(der_sig, data, ec.ECDSA(hashes.SHA256()))


def client_data_hash(ctype, challenge, extra=None):
    cd = {"type": ctype, "challenge": b64(challenge), "origin": ORIGIN}
    if extra:
        cd.update(extra)
    return hashlib.sha256(json.dumps(cd, separators=(",", ":")).encode()).digest()


class Ctap2:
    def __init__(self, hid, console):
        self.hid = hid
        self.console = console

    def _cmd(self, ins, params=None, up=False, timeout=30.0):
        import cbor2
        payload = bytes([ins]) + (cbor2.dumps(params) if params is not None else b"")
        trig = UpTrigger(self.console) if up else None
        resp = self.hid.cbor(payload, on_keepalive=trig, timeout=timeout)
        if resp and resp[0] != CTAP_OK:
            raise RuntimeError(f"CTAP2 error 0x{resp[0]:02x} "
                               f"({len(resp)}-byte response: {hexd(resp)})")
        return cbor2.loads(resp[1:]) if len(resp) > 1 else None

    def get_info(self):
        return self._cmd(INS_GET_INFO)

    def make_credential(self, challenge, user_id, rk=True):
        cdh = client_data_hash("webauthn.create", challenge,
                               {"rp": {"name": "bench", "id": RP_ID}})
        params = {
            1: cdh,
            2: {"id": RP_ID, "name": "bench"},
            3: {"id": user_id, "name": "bench-user",
                "displayName": "Bench User"},
            4: [{"type": "public-key", "alg": -7}],
            7: {"rk": rk},
        }
        return self._cmd(INS_MAKE_CREDENTIAL, params, up=True)

    def get_assertion(self, challenge, allow=None, up=True):
        cdh = client_data_hash("webauthn.get", challenge, {"rpId": RP_ID})
        params = {1: RP_ID, 2: cdh}
        if allow is not None:
            params[3] = [{"id": c} for c in allow]
        if not up:
            params[5] = {"up": False}  # options map (text keys)
        return self._cmd(INS_GET_ASSERTION, params, up=up)

    def reset(self):
        # RESET is deferred: it waits for user presence like the others.
        return self._cmd(INS_RESET, up=True, timeout=15.0)


# ---------------------------------------------------------------------------
# Response parsing + attestation / assertion verification
# ---------------------------------------------------------------------------

def parse_authdata(authdata):
    out = {
        "rp_id_hash": authdata[0:32],
        "flags": authdata[32],
        "sign_count": int.from_bytes(authdata[33:37], "big"),
    }
    if authdata[32] & 0x40:  # AT
        off = 37
        out["aaguid"] = authdata[off:off + 16]
        off += 16
        clen = int.from_bytes(authdata[off:off + 2], "big")
        off += 2
        out["cred_id"] = authdata[off:off + clen]
        off += clen
        out["cose_key"] = authdata[off:]
    return out


def check_make_credential(mc, challenge, expected_rp_hash):
    """Verify the packed self-attestation. Returns (cred_id, pub, sign_count)."""
    import cbor2
    if mc[1] != "packed":
        raise ProtocolError(f"unexpected fmt {mc[1]!r}")
    authdata = mc[2]
    a = parse_authdata(authdata)
    flags = a["flags"]
    if not (flags & 0x01):
        raise ProtocolError(f"UP bit not set in authData flags {flags:#04x}")
    if not (flags & 0x40):
        raise ProtocolError(f"AT bit not set in authData flags {flags:#04x}")
    if a["rp_id_hash"] != expected_rp_hash:
        raise ProtocolError("rpIdHash mismatch")
    pub = cose_to_pubkey(a["cose_key"])
    stmt = mc[3]
    if stmt.get("alg") != -7:
        raise ProtocolError(f"attStmt alg {stmt.get('alg')} != -7")
    if "x5c" in stmt:
        raise ProtocolError("self-attestation must not carry x5c")
    # attToSign = authData || clientDataHash (WebAuthn L3 §6.5.5 / packed
    # attestation). clientDataHash is the SHA-256 of the clientDataJSON the
    # bench sent as MakeCredential param 1 — recompute it here (same inputs
    # as make_credential: "webauthn.create", challenge, rp).
    cdh = client_data_hash("webauthn.create", challenge,
                           {"rp": {"name": "bench", "id": RP_ID}})
    sig_input = authdata + cdh
    verify_es256(pub, sig_input, stmt["sig"])
    print(f"  attestation: packed self-attestation signature VERIFIED")
    print(f"  credId: {b64(a['cred_id'])} ({len(a['cred_id'])} B)")
    print(f"  signCount (registration): {a['sign_count']}")
    print(f"  AAGUID: {a['aaguid'].hex()}")
    return a["cred_id"], pub, a["sign_count"]


def check_get_assertion(ga, challenge, pub, reg_sign_count, cred_id):
    """Verify an assertion. Returns the new sign_count."""
    if ga[1]["id"] != cred_id:
        raise ProtocolError("assertion credential id mismatch")
    authdata = ga[2]
    if len(authdata) != 37:
        raise ProtocolError(f"assertion authData is {len(authdata)} B, want 37")
    a = parse_authdata(authdata)
    if not (a["flags"] & 0x01):
        raise ProtocolError(f"UP bit not set in assertion flags {a['flags']:#04x}")
    if a["sign_count"] <= reg_sign_count:
        raise ProtocolError(f"signCount {a['sign_count']} not > "
                            f"registration {reg_sign_count}")
    # assertToSign = authData || clientDataHash (WebAuthn L3 §6.3.3 step 11 /
    # §7.2 step 21: "the concatenation authenticatorData || hash"). Same order
    # as attestation. clientDataHash is the SHA-256 of the clientDataJSON the
    # bench sent as getAssertion param 2 (same inputs as get_assertion:
    # "webauthn.get", challenge, rpId) — a SINGLE hash, not double-hashed.
    cdh = client_data_hash("webauthn.get", challenge, {"rpId": RP_ID})
    sig_input = authdata + cdh
    verify_es256(pub, sig_input, ga[3])
    print(f"  assertion: ES256 signature VERIFIED")
    print(f"  signCount: {reg_sign_count} -> {a['sign_count']} (strictly higher)")
    print(f"  user.id present: {'id' in ga.get(4, {})}")
    return a["sign_count"]


# ---------------------------------------------------------------------------
# Test sequence
# ---------------------------------------------------------------------------

def banner(title):
    print(f"\n=== {title} " + "=" * max(0, 60 - len(title)))


def run_all(console, hid, ctap, do_reset=True):
    import cbor2
    rp_hash = hashlib.sha256(RP_ID.encode()).digest()
    ok = 0
    total = 0
    # The Pico is reflashed (rebooted) just before the bench starts, so the
    # bench's own elapsed time approximates the Pico's uptime. The firmware's
    # RESET window check rejects RESETs once uptime > 10 s, so the negative
    # RESET test below needs the window to have actually expired — record the
    # start time and pad before that test if the user was fast.
    start_time = time.time()

    # (a) INIT ----------------------------------------------------------------
    banner("(a) CTAPHID INIT (broadcast)")
    total += 1
    cid, proto, ver, caps = hid.init()
    print(f"  allocated CID: {cid:#010x}")
    print(f"  protocol: {proto} (want 2)")
    print(f"  firmware: {ver[0]}.{ver[1]}.{ver[2]}")
    print(f"  capabilities: {caps:#04x} (want 0x07 = WINK|CBOR|NMSG)")
    assert proto == 2, "protocol version"
    assert caps == 0x07, "capabilities"
    ok += 1

    # (a) PING ----------------------------------------------------------------
    banner("(a) CTAPHID PING (echo)")
    total += 1
    data = os.urandom(32)
    r = hid.ping(data)
    assert r == data, f"echo mismatch: {hexd(r)}"
    print(f"  32-byte echo OK: {hexd(data, 8)}")
    ok += 1

    # (a) WINK ----------------------------------------------------------------
    banner("(a) CTAPHID WINK (LED flash)")
    total += 1
    r = hid.wink()
    assert r == b"", f"unexpected WINK payload: {hexd(r)}"
    print("  WINK accepted (watch the GP25 LED flash)")
    ok += 1

    # (b) GET_INFO ------------------------------------------------------------
    banner("(b) CTAP2 GET_INFO")
    total += 1
    info = ctap.get_info()
    print(f"  versions:  {info[1]}")
    print(f"  aaguid:    {info[3].hex()}")
    print(f"  options:   {info[4]}")
    print(f"  maxMsgSize: {info[5]}")
    print(f"  maxCredInList: {info[7]}   maxCredIdLen: {info[8]}")
    print(f"  transports: {info[9]}")
    print(f"  algorithms: {info[10]}")
    assert info[1] == ["FIDO_2_0"], "versions"
    assert info[5] == 256, "maxMsgSize"
    assert {"alg": -7, "type": "public-key"} in info[10], "algorithms"
    ok += 1

    # (c) MAKE_CREDENTIAL -------------------------------------------------------
    banner("(c) CTAP2 MAKE_CREDENTIAL (packed self-attestation)")
    total += 1
    challenge = os.urandom(32)
    user_id = os.urandom(16)
    print(f"  challenge: {b64(challenge)}")
    mc = ctap.make_credential(challenge, user_id)
    cred_id, pub, reg_count = check_make_credential(mc, challenge, rp_hash)
    ok += 1

    # (d) GET_ASSERTION (allowList) ---------------------------------------------
    banner("(d) CTAP2 GET_ASSERTION (on-chip sign)")
    total += 1
    challenge2 = os.urandom(32)
    print(f"  challenge: {b64(challenge2)}")
    ga = ctap.get_assertion(challenge2, allow=[cred_id])
    new_count = check_get_assertion(ga, challenge2, pub, reg_count, cred_id)
    ok += 1

    # (d) second assertion: counter keeps climbing ------------------------------
    banner("(d) GET_ASSERTION again (counter monotonic)")
    total += 1
    challenge3 = os.urandom(32)
    ga2 = ctap.get_assertion(challenge3, allow=[cred_id])
    new_count2 = check_get_assertion(ga2, challenge3, pub, new_count, cred_id)
    ok += 1

    # (d) preflight (up:false) — synchronous, UP bit clear ----------------------
    banner("(d) GET_ASSERTION preflight (options up:false)")
    total += 1
    ga3 = ctap.get_assertion(os.urandom(32), allow=[cred_id], up=False)
    a3 = parse_authdata(ga3[2])
    assert not (a3["flags"] & 0x01), "preflight must not set UP"
    print(f"  synchronous response, flags={a3['flags']:#04x} (UP clear) — OK")
    ok += 1

    # (a) CANCEL ----------------------------------------------------------------
    banner("(a) CANCEL a pending MAKE_CREDENTIAL")
    total += 1
    hid.send_message(hid.cid, CMD_CBOR,
                     bytes([INS_MAKE_CREDENTIAL]) + cbor2.dumps({
                         1: client_data_hash("webauthn.create", os.urandom(32)),
                         2: {"id": RP_ID, "name": "bench"},
                         3: {"id": os.urandom(16), "name": "bench-user",
                             "displayName": "Bench User"},
                         4: [{"type": "public-key", "alg": -7}],
                     }))
    ka = hid.read_message(CMD_KEEPALIVE, hid.cid, timeout=5)
    assert ka[0] == KA_UP_NEEDED, f"first keep-alive is {ka[0]:#04x}"
    print(f"  saw keep-alive {ka[0]:#04x} (UP_NEEDED); sending CANCEL")
    hid.cancel()
    r = hid.read_message(CMD_CBOR, hid.cid, timeout=5)
    assert r == bytes([CTAP_ERR_KEEPALIVE_CANCEL]), \
        f"expected 0x{CTAP_ERR_KEEPALIVE_CANCEL:02x}, got {hexd(r)}"
    print(f"  CANCEL answered 0x{CTAP_ERR_KEEPALIVE_CANCEL:02x} "
          f"(CTAP2_ERR_KEEPALIVE_CANCEL) — OK")
    ok += 1

    # (e) RESET — negative first (outside the 10 s power-on window) --------------
    # Guarantee the 10 s window has expired: the firmware rejects RESET once
    # uptime > FIDO_CTAP_RESET_WINDOW_MS (10 s). The Pico rebooted on reflash
    # just before the bench started, so pad our own elapsed time to > 10 s.
    banner("(e) CTAP2 RESET outside the 10 s window (expect NOT_ALLOWED)")
    elapsed = time.time() - start_time
    if elapsed < 11.0:
        print(f"  (waiting {11.0 - elapsed:.1f}s for the 10 s reset window to expire)")
        time.sleep(11.0 - elapsed)
    total += 1
    try:
        ctap.reset()
        raise ProtocolError("RESET succeeded outside the window?")
    except RuntimeError as e:
        print(f"  {e}")
        assert "0x30" in str(e), "expected 0x30 NOT_ALLOWED"
        ok += 1

    if do_reset:
        ok, total = reset_section(console, hid, ctap, rp_hash, ok, total)

    print(f"\n{'=' * 62}")
    print(f"RESULT: {ok}/{total} checks passed")
    print("\nLast console lines:")
    for line in console.report(60):
        print(f"  | {line}")
    return ok == total


def reset_section(console, hid, ctap, rp_hash, ok, total):
    """The in-window RESET (needs a Pico RESET-button press) + the
    post-RESET checks: store wiped, re-register, login."""

    # (e) RESET — positive (needs a Pico RESET-button press) ----------------------
    banner("(e) CTAP2 RESET inside the 10 s window")
    print("\n  Press the Pico's RESET button NOW, then press Enter here")
    input("  > ")
    hid.reopen()
    hid.init()
    try:
        ctap.reset()
        print("  RESET accepted (credential store wiped, counter zeroed)")
        ok += 1
    except RuntimeError as e:
        print(f"  RESET failed: {e}")
        raise

    # (e) post-RESET: no credentials ---------------------------------------------
    banner("(e) GET_ASSERTION after RESET (expect NO_CREDENTIALS)")
    total += 1
    try:
        ctap.get_assertion(os.urandom(32), up=False)
        raise ProtocolError("expected NO_CREDENTIALS after RESET")
    except RuntimeError as e:
        print(f"  {e}")
        assert "0x2e" in str(e), "expected 0x2e NO_CREDENTIALS"
        ok += 1

    # (c/d) re-register + login after RESET ---------------------------------------
    banner("(c/d) re-register + login after RESET")
    total += 2
    challenge = os.urandom(32)
    mc = ctap.make_credential(challenge, os.urandom(16))
    cred_id2, pub2, cnt2 = check_make_credential(mc, challenge, rp_hash)
    challenge4 = os.urandom(32)
    ga = ctap.get_assertion(challenge4, allow=[cred_id2])
    check_get_assertion(ga, challenge4, pub2, cnt2, cred_id2)
    ok += 2
    return ok, total


def main():
    import cbor2
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("command",
                    choices=["run-all", "init", "ping", "wink", "get-info",
                             "make-credential", "get-assertion", "cancel-test",
                             "reset", "reset-now", "post-reset"])
    ap.add_argument("--serial", default="/dev/ttyUSB0")
    ap.add_argument("--no-reset", action="store_true",
                    help="run-all: skip the in-window RESET section "
                         "(run it later with 'post-reset' after a "
                         "Pico RESET-button press)")
    args = ap.parse_args()

    console = SerialConsole(args.serial)
    hid = Fido2Hid()
    ctap = Ctap2(hid, console)

    try:
        hid.open()
    except RuntimeError as e:
        print(f"USB: {e}")
        return 2

    if args.command == "run-all":
        try:
            hid.init(timeout=3)
        except TimeoutError:
            print("Device did not answer INIT — not provisioned?")
            print("Sending 'provision' on the console; press the Pico RESET")
            print("button, then press Enter here.")
            console.send("provision")
            input("> ")
            hid.reopen()
            hid.init()
        try:
            ok = run_all(console, hid, ctap, do_reset=not args.no_reset)
        except (AssertionError, ProtocolError, RuntimeError, TimeoutError) as e:
            print(f"\nFAILED: {e}")
            print("\nLast console lines:")
            for line in console.report(60):
                print(f"  | {line}")
            return 1
        return 0 if ok else 1

    try:
        if args.command == "init":
            cid, proto, ver, caps = hid.init()
            print(f"cid={cid:#010x} proto={proto} fw={ver[0]}.{ver[1]}.{ver[2]}"
                  f" caps={caps:#04x}")
        elif args.command == "ping":
            d = os.urandom(16)
            r = hid.ping(d)
            print(f"echo {'OK' if r == d else 'FAIL'}: {hexd(r)}")
        elif args.command == "wink":
            hid.wink()
            print("wink sent")
        elif args.command == "get-info":
            print(cbor2.dumps(ctap.get_info(), indent=2))
        elif args.command == "make-credential":
            ch = os.urandom(32)
            mc = ctap.make_credential(ch, os.urandom(16))
            check_make_credential(mc, ch, hashlib.sha256(RP_ID.encode()).digest())
        elif args.command == "get-assertion":
            print("need a registered credId — use run-all")
            return 2
        elif args.command == "cancel-test":
            hid.init()
            hid.send_message(hid.cid, CMD_CBOR,
                             bytes([INS_MAKE_CREDENTIAL]) + cbor2.dumps({
                                 1: client_data_hash("webauthn.create",
                                                     os.urandom(32)),
                                 2: {"id": RP_ID, "name": "bench"},
                                 3: {"id": os.urandom(16), "name": "u",
                                     "displayName": "U"},
                                 4: [{"type": "public-key", "alg": -7}],
                             }))
            time.sleep(1.0)   # let the UP_NEEDED keep-alive fly
            hid.cancel()
            r = hid.read_message(CMD_CBOR, hid.cid, timeout=5)
            print(f"cancel response: {hexd(r)} "
                  f"(want 0x{CTAP_ERR_KEEPALIVE_CANCEL:02x})")
        elif args.command == "reset":
            hid.init()
            ctap.reset()
            print("RESET ok")
        elif args.command == "reset-now":
            hid.init()
            ctap.reset()
            print("RESET ok (in-window)")
        elif args.command == "post-reset":
            # In-window RESET: prompt for the Pico RESET-button press (which
            # reboots the chip and starts a fresh 10 s power-on window), wait
            # for it, re-locate the hidraw node (the bus/address shifts after
            # a RESET), then issue the RESET inside that window.
            print("\n  Press the Pico's RESET button NOW, then press Enter here")
            input("  > ")
            hid.reopen()
            hid.init()
            rp_hash = hashlib.sha256(RP_ID.encode()).digest()
            ok = total = 0
            try:
                ctap.reset()
                print("RESET accepted (credential store wiped, counter "
                      "zeroed)")
                ok += 1
            except RuntimeError as e:
                print(f"RESET failed: {e}")
                raise
            total += 1
            try:
                ctap.get_assertion(os.urandom(32), up=False)
                raise ProtocolError("expected NO_CREDENTIALS after RESET")
            except RuntimeError as e:
                print(f"  {e}")
                assert "0x2e" in str(e), "expected 0x2e NO_CREDENTIALS"
                ok += 1
            total += 1
            challenge = os.urandom(32)
            mc = ctap.make_credential(challenge, os.urandom(16))
            cred_id2, pub2, cnt2 = check_make_credential(mc, challenge,
                                                         rp_hash)
            challenge4 = os.urandom(32)
            ga = ctap.get_assertion(challenge4, allow=[cred_id2])
            check_get_assertion(ga, challenge4, pub2, cnt2, cred_id2)
            ok += 2
            total += 2
            print(f"\nRESULT: {ok}/{total} post-RESET checks passed")
    except (RuntimeError, TimeoutError, ProtocolError) as e:
        print(f"FAILED: {e}")
        return 1
    finally:
        hid.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
