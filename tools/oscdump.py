"""Minimal OSC receiver for testing: counts messages per address, prints first and last decoded values.

Usage: python oscdump.py <port> <seconds>
"""
import socket
import struct
import sys
import time


def read_string(data, offset):
    end = data.index(b"\0", offset)
    value = data[offset:end].decode()
    return value, (end + 4) & ~3


def decode(data):
    address, offset = read_string(data, 0)
    tags, offset = read_string(data, offset)
    args = []
    for tag in tags[1:]:
        if tag == "i":
            args.append(struct.unpack(">i", data[offset:offset + 4])[0])
            offset += 4
        elif tag == "f":
            args.append(round(struct.unpack(">f", data[offset:offset + 4])[0], 3))
            offset += 4
        elif tag == "s":
            value, offset = read_string(data, offset)
            args.append(value)
    return address, tags, args


port = int(sys.argv[1])
duration = float(sys.argv[2])
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.bind(("127.0.0.1", port))
sock.settimeout(0.2)
stats = {}
end = time.time() + duration
while time.time() < end:
    try:
        data, _ = sock.recvfrom(4096)
    except socket.timeout:
        continue
    address, tags, args = decode(data)
    entry = stats.setdefault(address, {"count": 0, "tags": tags, "first": args})
    entry["count"] += 1
    entry["last"] = args

for address in sorted(stats):
    entry = stats[address]
    print(f"{address:32} {entry['tags']:6} n={entry['count']:5}  first={entry['first']}  last={entry['last']}")
