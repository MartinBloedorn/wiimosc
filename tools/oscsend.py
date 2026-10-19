"""Minimal OSC sender for testing: sends one message with float, int or string arguments (or none).

Usage: python oscsend.py <port> <address> [value ...] [--host <host>] [--int | --string] [--bundle]

  Values that are numbers are sent as float32, others as strings.
  --int     send the numbers as int32 instead
  --string  send all values as strings, numbers too
  --bundle  wrap the message in a bundle

Examples: python oscsend.py 4322 /wiimosc/0/mote/rumble/pulse 1 300
          python oscsend.py 4322 /wiimosc/0/mote/speaker/file C:/sounds/click.wav
"""
import socket
import struct
import sys


def padded(text):
    data = text.encode() + b"\0"
    return data + b"\0" * (-len(data) % 4)


def is_number(value):
    try:
        float(value)
        return True
    except ValueError:
        return False


def message(address, values, number_tag):
    """number_tag: "f" or "i" for the values that are numbers, "s" for sending everything as strings."""
    tags = [number_tag if number_tag != "s" and is_number(value) else "s" for value in values]
    data = padded(address) + padded("," + "".join(tags))
    for value, tag in zip(values, tags):
        if tag == "s":
            data += padded(value)
        else:
            data += struct.pack(">i", int(float(value))) if tag == "i" else struct.pack(">f", float(value))
    return data


def main():
    args = sys.argv[1:]
    host = "127.0.0.1"
    if "--host" in args:
        index = args.index("--host")
        host = args[index + 1]
        del args[index:index + 2]
    tag = "s" if "--string" in args else "i" if "--int" in args else "f"
    is_bundle = "--bundle" in args
    args = [arg for arg in args if arg not in ("--int", "--string", "--bundle")]
    if len(args) < 2:
        print(__doc__)
        sys.exit(1)

    data = message(args[1], args[2:], tag)
    if is_bundle:
        data = padded("#bundle") + struct.pack(">Q", 1) + struct.pack(">i", len(data)) + data
    socket.socket(socket.AF_INET, socket.SOCK_DGRAM).sendto(data, (host, int(args[0])))


if __name__ == "__main__":
    main()
