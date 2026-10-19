"""Minimal OSC sender for testing: sends one message with float arguments (or none).

Usage: python oscsend.py <port> <address> [value ...] [--host <host>] [--int] [--bundle]

  --int     send the values as int32 instead of float32
  --bundle  wrap the message in a bundle

Example: python oscsend.py 4322 /wiimosc/0/mote/rumble 1
"""
import socket
import struct
import sys


def padded(text):
    data = text.encode() + b"\0"
    return data + b"\0" * (-len(data) % 4)


def message(address, values, as_int):
    tags = "," + ("i" if as_int else "f") * len(values)
    data = padded(address) + padded(tags)
    for value in values:
        data += struct.pack(">i", int(value)) if as_int else struct.pack(">f", float(value))
    return data


def main():
    args = sys.argv[1:]
    host = "127.0.0.1"
    if "--host" in args:
        index = args.index("--host")
        host = args[index + 1]
        del args[index:index + 2]
    as_int = "--int" in args
    is_bundle = "--bundle" in args
    args = [arg for arg in args if arg not in ("--int", "--bundle")]
    if len(args) < 2:
        print(__doc__)
        sys.exit(1)

    data = message(args[1], args[2:], as_int)
    if is_bundle:
        data = padded("#bundle") + struct.pack(">Q", 1) + struct.pack(">i", len(data)) + data
    socket.socket(socket.AF_INET, socket.SOCK_DGRAM).sendto(data, (host, int(args[0])))


if __name__ == "__main__":
    main()
