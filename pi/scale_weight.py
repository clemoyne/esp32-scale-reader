#!/usr/bin/env python3
"""Connect to a QN (Renpho) scale over BLE and print the weight it reports.

Standard library only (ctypes is used for the L2CAP connect Python 3.7
lacks). Find the scale's address with scan_scale.py, then:

    sudo python3 scale_weight.py AA:BB:CC:DD:EE:FF [seconds] [--kg]

The handshake also sets the scale's display unit: pounds, or kg with --kg.

Tap the scale to wake it, wait for "write ... 02..." (the time, which ends
the handshake), then step on: the scale only sends live weight while it is
measuring. Stop anything else scanning on the adapter first. On a Pi 3 A+ the link only held with the
scale a metre or two away and a 4 s supervision timeout for the test:
    echo 400 | sudo tee /sys/kernel/debug/bluetooth/hci0/supervision_timeout
(default 42, i.e. 420 ms; put it back afterwards).

Protocol as reverse-engineered by openScale (BluetoothQNScale): service
FFE0, notifications on FFE1/FFE2, config written to FFE3, time to FFE4.
"""
import ctypes
import datetime
import os
import socket
import struct
import sys
import time

ARGS = [a for a in sys.argv[1:] if not a.startswith("--")]
if not ARGS:
    sys.exit(__doc__)
ADDR = ARGS[0]
RUN_SECONDS = int(ARGS[1]) if len(ARGS) > 1 else 180
UNIT_BYTE = 0x01 if "--kg" in sys.argv[1:] else 0x02  # display unit: 1 kg, 2 lb
AF_BLUETOOTH, BTPROTO_L2CAP = 31, 0
ATT_CID = 4
BDADDR_LE_PUBLIC = 1
LB_PER_KG = 2.2046226218

libc = ctypes.CDLL(None, use_errno=True)


def ts():
    return datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]


def log(*a):
    print(ts(), *a)
    sys.stdout.flush()


def sockaddr_l2(addr, addr_type):
    b = bytes(reversed(bytes.fromhex(addr.replace(":", ""))))
    # sa_family, l2_psm, l2_bdaddr[6], l2_cid, l2_bdaddr_type
    return struct.pack("<HH6sHB", AF_BLUETOOTH, 0, b, ATT_CID, addr_type)


def connect(addr_type):
    s = socket.socket(AF_BLUETOOTH, socket.SOCK_SEQPACKET, BTPROTO_L2CAP)
    local = sockaddr_l2("00:00:00:00:00:00", BDADDR_LE_PUBLIC)
    if libc.bind(s.fileno(), local, len(local)) != 0:
        e = ctypes.get_errno()
        s.close()
        raise OSError(e, "bind: " + os.strerror(e))
    remote = sockaddr_l2(ADDR, addr_type)
    if libc.connect(s.fileno(), remote, len(remote)) != 0:
        e = ctypes.get_errno()
        s.close()
        raise OSError(e, os.strerror(e))
    return s


class Att:
    def __init__(self, sock):
        self.s = sock
        self.handler = None

    def _recv(self, timeout):
        self.s.settimeout(timeout)
        pdu = self.s.recv(512)
        op = pdu[0]
        if op in (0x1B, 0x1D):  # notification / indication
            handle = struct.unpack("<H", pdu[1:3])[0]
            if op == 0x1D:
                self.s.send(b"\x1e")
            if self.handler:
                self.handler(handle, pdu[3:])
            return None
        return pdu

    def request(self, pdu, timeout=5.0):
        self.s.send(pdu)
        deadline = time.time() + timeout
        while True:
            resp = self._recv(max(0.1, deadline - time.time()))
            if resp is not None:
                return resp

    def pump(self, seconds):
        end = time.time() + seconds
        while time.time() < end:
            try:
                self._recv(max(0.1, end - time.time()))
            except socket.timeout:
                pass

    def characteristics(self):
        chars, start = [], 1
        while start <= 0xFFFF:
            r = self.request(struct.pack("<BHHH", 0x08, start, 0xFFFF, 0x2803))
            if r[0] != 0x09:
                break
            n = r[1]
            for i in range(2, len(r), n):
                decl, props, vh = struct.unpack("<HBH", r[i:i + 5])
                uuid = r[i + 5:i + n]
                u = struct.unpack("<H", uuid)[0] if len(uuid) == 2 else uuid[::-1].hex()
                chars.append((decl, props, vh, u))
                start = decl + 1
        return chars

    def find_cccd(self, start, end):
        while start <= end:
            r = self.request(struct.pack("<BHH", 0x04, start, end))
            if r[0] != 0x05:
                return None
            n = 4 if r[1] == 1 else 18
            for i in range(2, len(r), n):
                h = struct.unpack("<H", r[i:i + 2])[0]
                if n == 4 and struct.unpack("<H", r[i + 2:i + 4])[0] == 0x2902:
                    return h
                start = h + 1
        return None

    def write(self, handle, value, props):
        if props & 0x08:  # write with response
            r = self.request(struct.pack("<BH", 0x12, handle) + value)
            return r[0] == 0x13
        self.s.send(struct.pack("<BH", 0x52, handle) + value)
        return True


def checksum(b):
    return sum(b) & 0xFF


def try_connect(deadline):
    while time.time() < deadline:
        try:
            sock = connect(BDADDR_LE_PUBLIC)
            log("Connected")
            return sock
        except OSError as e:
            log("no connection (%s), retrying" % (e.strerror or e))
            time.sleep(0.3)
    return None


def session(sock, deadline, result):
    att = Att(sock)
    chars = att.characteristics()
    by_uuid = {}
    for decl, props, vh, u in chars:
        by_uuid[u] = (vh, props)
        log("  char %s handle 0x%04x props 0x%02x" %
            (("%04X" % u) if isinstance(u, int) else u, vh, props))

    write_cfg = by_uuid.get(0xFFE3) or by_uuid.get(0xFFF2)
    write_time = by_uuid.get(0xFFE4)
    state = {"scale": 100.0, "cfg_sent": False, "time_sent": False}
    pending = []

    def on_data(handle, data):
        log("notify 0x%04x: %s" % (handle, data.hex()))
        if not data:
            return
        kind = data[0]
        if kind == 0x12 and len(data) > 10:
            state["scale"] = 100.0 if data[10] == 1 else 10.0
            if write_cfg and not state["cfg_sent"]:
                cfg = bytearray([0x13, 0x09, 0x15, UNIT_BYTE, 0x10, 0x00, 0x00, 0x00, 0x00])
                cfg[-1] = checksum(cfg[:-1])
                state["cfg_sent"] = True
                pending.append((write_cfg, bytes(cfg)))
        elif kind == 0x14 and write_time and not state["time_sent"]:
            secs = int(time.time()) - 946684800  # seconds since 2000-01-01 UTC
            state["time_sent"] = True
            pending.append((write_time, b"\x02" + struct.pack("<I", secs)))
        elif kind == 0x10 and len(data) > 5:
            kg = ((data[3] << 8) | data[4]) / state["scale"]
            stable = data[5] == 1
            log("  WEIGHT %.2f kg = %.1f lb  %s" %
                (kg, kg * LB_PER_KG, "STABLE" if stable else "(settling)"))
            if stable:
                result["kg"] = kg

    att.handler = on_data
    for i, (decl, props, vh, u) in enumerate(chars):
        if props & 0x30 and u in (0xFFE1, 0xFFE2, 0xFFF1):
            end = chars[i + 1][0] - 1 if i + 1 < len(chars) else 0xFFFF
            cccd = att.find_cccd(vh + 1, end)
            if cccd:
                att.write(cccd, b"\x01\x00" if props & 0x10 else b"\x02\x00", 0x08)
                log("Subscribed to %04X" % u)

    while time.time() < deadline:
        while pending:
            (vh, props), value = pending.pop(0)
            log("write 0x%04x: %s" % (vh, value.hex()))
            att.write(vh, value, props)
        att.pump(0.5)
        if "kg" in result:
            att.pump(2)
            return


def main():
    deadline = time.time() + RUN_SECONDS
    log("Waiting for %s (step on the scale)..." % ADDR)
    result = {}
    while time.time() < deadline and "kg" not in result:
        sock = try_connect(deadline)
        if sock is None:
            break
        try:
            session(sock, deadline, result)
        except (OSError, socket.timeout) as e:
            log("Link dropped (%s), reconnecting" % (e.strerror or e))
        finally:
            sock.close()
    if "kg" in result:
        kg = result["kg"]
        log("Final weight: %.2f kg = %.1f lb" % (kg, kg * LB_PER_KG))
        return 0
    log("No stable weight received.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
