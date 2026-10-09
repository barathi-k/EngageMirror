"""Send an mDNS PTR query and report which responders answer."""
import socket
import struct
import sys
import time

MCAST = "224.0.0.251"
PORT = 5353
SERVICES = [b"_airplay._tcp.local", b"_raop._tcp.local"]


def encode_name(name):
    out = b""
    for label in name.split(b"."):
        out += bytes([len(label)]) + label
    return out + b"\x00"


def build_query(services):
    header = struct.pack("!HHHHHH", 0x1234, 0x0000, len(services), 0, 0, 0)
    body = b""
    for s in services:
        body += encode_name(s) + struct.pack("!HH", 12, 0x0001)  # PTR, IN (unicast bit off)
    return header + body


def read_name(data, off):
    labels = []
    jumped = False
    start = off
    guard = 0
    while True:
        guard += 1
        if guard > 128 or off >= len(data):
            break
        ln = data[off]
        if ln == 0:
            off += 1
            break
        if ln & 0xC0 == 0xC0:
            ptr = struct.unpack("!H", data[off:off + 2])[0] & 0x3FFF
            if not jumped:
                start = off + 2
            jumped = True
            off = ptr
            continue
        labels.append(data[off + 1:off + 1 + ln])
        off += 1 + ln
    return b".".join(labels), (start if jumped else off)


def parse(data):
    if len(data) < 12:
        return []
    qd, an, ns, ar = struct.unpack("!HHHH", data[4:12])
    off = 12
    for _ in range(qd):
        _, off = read_name(data, off)
        off += 4
    found = []
    for _ in range(an + ns + ar):
        name, off = read_name(data, off)
        if off + 10 > len(data):
            break
        rtype, _cls, _ttl, rdlen = struct.unpack("!HHIH", data[off:off + 10])
        off += 10
        rdata = data[off:off + rdlen]
        if rtype == 12:  # PTR
            target, _ = read_name(data, off)
            found.append(("PTR", name.decode(errors="replace"), target.decode(errors="replace")))
        elif rtype == 33 and rdlen >= 6:  # SRV
            _prio, _w, port = struct.unpack("!HHH", rdata[:6])
            tgt, _ = read_name(data, off + 6)
            found.append(("SRV", name.decode(errors="replace"),
                          f"{tgt.decode(errors='replace')}:{port}"))
        off += rdlen
    return found


def main():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("0.0.0.0", 0))
    sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 4)
    sock.settimeout(0.5)

    sock.sendto(build_query(SERVICES), (MCAST, PORT))

    deadline = time.time() + 5.0
    seen = {}
    while time.time() < deadline:
        try:
            data, addr = sock.recvfrom(9000)
        except socket.timeout:
            continue
        for rec in parse(data):
            key = (addr[0], rec)
            if key not in seen:
                seen[key] = True
                print(f"  from {addr[0]:<15} {rec[0]:<4} {rec[1]}  ->  {rec[2]}")

    if not seen:
        print("  (no mDNS responses received)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
