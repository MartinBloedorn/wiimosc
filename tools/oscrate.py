"""Prints OSC messages per second, per device id, to spot stalls. Status and battery messages are printed as they arrive.

Usage: python oscrate.py <port> <seconds>
"""
import socket
import sys
import time

port = int(sys.argv[1])
duration = float(sys.argv[2])
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.bind(("127.0.0.1", port))
sock.settimeout(0.05)
start = time.time()
second = 0
counts = {}
while time.time() < start + duration:
    try:
        data, _ = sock.recvfrom(4096)
        address = data[: data.index(b"\0")].decode()
        device = address.split("/")[2]
        counts[device] = counts.get(device, 0) + 1
        if address.endswith("/battery") or address.endswith("/status"):
            print(f"  t={time.time() - start:5.1f} {address}", flush=True)
    except socket.timeout:
        pass
    now = int(time.time() - start)
    if now != second:
        print(f"t={second:3d}s " + " ".join(f"[{k}]={v}" for k, v in sorted(counts.items())), flush=True)
        counts = {}
        second = now
