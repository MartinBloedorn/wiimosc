"""Writes a mono 16-bit wav: `lead` s of silence, then a sine of `freq` Hz for `seconds` s.

For `wiimosc-speaker --test-mode file`, which quits by itself at the end (on Windows, run.sh can't stop the speaker
tool with Ctrl+C). The lead-in covers connecting and the speaker setup: playback starts when the remote connects.
"""
import argparse
import wave

import numpy as np

parser = argparse.ArgumentParser()
parser.add_argument("path")
parser.add_argument("freq", type=float)
parser.add_argument("seconds", type=float)
parser.add_argument("--lead", type=float, default=1.0)
parser.add_argument("--rate", type=int, default=48000)
args = parser.parse_args()

t = np.arange(int(args.seconds * args.rate)) / args.rate
tone = 0.9 * np.sin(2 * np.pi * args.freq * t)
data = np.concatenate([np.zeros(int(args.lead * args.rate)), tone])
with wave.open(args.path, "wb") as w:
    w.setnchannels(1)
    w.setsampwidth(2)
    w.setframerate(args.rate)
    w.writeframes((data * 32767).astype(np.int16).tobytes())
