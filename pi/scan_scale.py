#!/usr/bin/env python3
"""Watch for a Renpho / QN bathroom scale's BLE advertisements.

Standard library only: opens a raw HCI socket, turns on LE scanning and
prints every advertisement from a device named "QN Scale" (or similar) or
advertising one of Renpho's service UUIDs. Needs root (sudo).

    sudo python3 scan_scale.py          # scale only
    sudo python3 scan_scale.py --all    # every device, for checking the radio
"""
import datetime
import signal
import socket
import struct
import sys

DEVICE = 0  # hci0
AF_BLUETOOTH = getattr(socket, "AF_BLUETOOTH", 31)
BTPROTO_HCI = getattr(socket, "BTPROTO_HCI", 1)
SOL_HCI, HCI_FILTER = 0, 2
HCI_COMMAND_PKT, HCI_EVENT_PKT = 0x01, 0x04
EVT_CMD_COMPLETE, EVT_LE_META = 0x0E, 0x3E
LE_ADV_REPORT = 0x02
OGF_LE = 0x08
OCF_SET_SCAN_PARAMS, OCF_SET_SCAN_ENABLE = 0x000B, 0x000C

# 16-bit UUIDs seen on Renpho / QN (Yolanda) scales, plus the standard
# Weight Scale and Body Composition services.
RENPHO_UUIDS = {0xFFE0, 0xFFF0, 0xFFB0, 0x181B, 0x181D}
ADV_TYPES = {0: "ADV_IND", 1: "ADV_DIRECT", 2: "ADV_SCAN_IND",
             3: "ADV_NONCONN", 4: "SCAN_RSP"}


def opcode(ocf):
    return (OGF_LE << 10) | ocf


def send_cmd(sock, ocf, params, quiet=False):
    sock.send(struct.pack("<BHB", HCI_COMMAND_PKT, opcode(ocf), len(params)) + params)
    # Wait for its Command Complete and report a non-zero status.
    for _ in range(50):
        pkt = sock.recv(260)
        if len(pkt) >= 7 and pkt[0] == HCI_EVENT_PKT and pkt[1] == EVT_CMD_COMPLETE:
            op, status = struct.unpack("<HB", pkt[4:7])
            if op == opcode(ocf):
                if status and not quiet:
                    print("HCI command 0x%04x failed, status 0x%02x" % (op, status),
                          file=sys.stderr)
                return status
    return None


def parse_ad(data):
    """Split advertising data into name, 16-bit UUIDs, manufacturer and service data."""
    name, uuids, manuf, svc = None, set(), [], []
    i = 0
    while i < len(data):
        length = data[i]
        if length == 0 or i + 1 + length > len(data):
            break
        kind, value = data[i + 1], data[i + 2:i + 1 + length]
        if kind in (0x08, 0x09):
            name = value.decode("utf-8", "replace")
        elif kind in (0x02, 0x03):
            uuids.update(struct.unpack("<%dH" % (len(value) // 2), value[:len(value) // 2 * 2]))
        elif kind in (0x06, 0x07):
            for j in range(0, len(value) - 15, 16):
                u = value[j:j + 16][::-1]
                # Base-UUID 128-bit forms carry a 16-bit UUID in bytes 2-3.
                if u[4:] == bytes.fromhex("00001000800000805f9b34fb"):
                    uuids.add(int.from_bytes(u[2:4], "big"))
        elif kind == 0x16 and len(value) >= 2:
            u = struct.unpack("<H", value[:2])[0]
            uuids.add(u)
            svc.append((u, value[2:]))
        elif kind == 0xFF and len(value) >= 2:
            manuf.append((struct.unpack("<H", value[:2])[0], value[2:]))
        i += 1 + length
    return name, uuids, manuf, svc


def is_scale_name(name):
    n = (name or "").lower().replace("-", " ").replace("_", " ")
    return ("qn" in n and "scale" in n) or "renpho" in n


def main(sock):
    show_all = "--all" in sys.argv[1:]
    sock.bind((DEVICE,))
    # Pass only HCI events: Command Complete and LE Meta.
    sock.setsockopt(SOL_HCI, HCI_FILTER, struct.pack(
        "<IIIH2x", 1 << HCI_EVENT_PKT, 1 << EVT_CMD_COMPLETE,
        1 << (EVT_LE_META - 32), 0))

    def stop(*_):
        sys.exit(0)

    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)

    send_cmd(sock, OCF_SET_SCAN_ENABLE, b"\x00\x00", quiet=True)  # in case a scan is running
    # Active scan (gets scan responses, which often carry the name),
    # 10 ms interval and window, public address, accept all.
    send_cmd(sock, OCF_SET_SCAN_PARAMS, struct.pack("<BHHBB", 1, 0x10, 0x10, 0, 0))
    # Enable, duplicates NOT filtered, so changing weight readings keep coming.
    if send_cmd(sock, OCF_SET_SCAN_ENABLE, b"\x01\x00"):
        sys.exit(1)
    print("Scanning on hci0%s. Ctrl-C to stop." % (" (all devices)" if show_all else
                                                   " for QN / Renpho scales"))
    sys.stdout.flush()

    names, matched = {}, set()
    while True:
        pkt = sock.recv(260)
        if len(pkt) < 5 or pkt[0] != HCI_EVENT_PKT or pkt[1] != EVT_LE_META \
                or pkt[3] != LE_ADV_REPORT:
            continue
        p, end = 5, len(pkt)
        for _ in range(pkt[4]):
            if p + 9 > end:
                break
            evt_type, addr_type = pkt[p], pkt[p + 1]
            addr = ":".join("%02X" % b for b in reversed(pkt[p + 2:p + 8]))
            dlen = pkt[p + 8]
            data = pkt[p + 9:p + 9 + dlen]
            if p + 9 + dlen >= end:
                break
            rssi = struct.unpack("b", pkt[p + 9 + dlen:p + 10 + dlen])[0]
            p += 10 + dlen

            name, uuids, manuf, svc = parse_ad(data)
            if name:
                names[addr] = name
            name = names.get(addr)
            if is_scale_name(name) or uuids & RENPHO_UUIDS:
                matched.add(addr)
            if not show_all and addr not in matched:
                continue

            ts = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]
            parts = ["%s  %s  RSSI %4d dBm  %-11s" % (
                ts, addr, rssi, ADV_TYPES.get(evt_type, "type%d" % evt_type))]
            if name:
                parts.append("name=%r" % name)
            if uuids:
                parts.append("uuids=" + ",".join("%04X" % u for u in sorted(uuids)))
            for company, payload in manuf:
                parts.append("manuf[0x%04X]=%s" % (company, payload.hex()))
            for u, payload in svc:
                parts.append("svc[%04X]=%s" % (u, payload.hex()))
            if not manuf and not svc:
                parts.append("raw=" + data.hex())
            print("  ".join(parts))
            sys.stdout.flush()


if __name__ == "__main__":
    sock = socket.socket(AF_BLUETOOTH, socket.SOCK_RAW, BTPROTO_HCI)
    try:
        main(sock)
    finally:
        send_cmd(sock, OCF_SET_SCAN_ENABLE, b"\x00\x00", quiet=True)
        print("Scanning stopped.", file=sys.stderr)
