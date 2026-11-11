#!/usr/bin/env python3
"""Decode the Govee TV backlight strip protocol from a logic analyzer.

Captures live with sigrok-cli (any fx2lafw analyzer, such as the cheap 24 MHz
8-channel clones) or reads a saved .sr file, then prints the bit timing, the
frame length and the distinct chip values in the first full frame.

    decode_capture.py                       capture 70 ms on D0 (analyzer CH1)
    decode_capture.py --channel D1 --ms 200
    decode_capture.py --file captures/govee-controller-red.sr

Each chip is 90 bits: 5 channels (G, R, B, cold white, warm white), each 2
current bits and a 16-bit value. See docs/protocol.md.
"""
import argparse
import collections
import subprocess
import sys

ap = argparse.ArgumentParser()
ap.add_argument("--channel", default="D0")
ap.add_argument("--ms", default="70")
ap.add_argument("--file")
a = ap.parse_args()

if a.file:
    cmd = ["sigrok-cli", "-i", a.file, "-O", "vcd"]
else:
    cmd = ["sigrok-cli", "-d", "fx2lafw", "--config", "samplerate=24m",
           "--channels", a.channel, "--time", a.ms, "-O", "vcd"]
r = subprocess.run(cmd, capture_output=True, text=True)
if not r.stdout:
    sys.exit("no capture: " + r.stderr.strip())

ids, scale, ev, t = {}, 1.0, [], 0.0
units = {"ps": 1e-3, "ns": 1.0, "us": 1e3}
for line in r.stdout.splitlines():
    p = line.split()
    if not p:
        continue
    if p[0] == "$timescale":
        n = "".join(p[1:-1])  # e.g. "100 ps" -> "100ps"
        num = int("".join(ch for ch in n if ch.isdigit()))
        scale = num * units[n.lstrip("0123456789")]
    elif p[0] == "$var":
        ids[p[3]] = p[4]
    elif p[0].startswith("#"):
        t = int(p[0][1:]) * scale  # nanoseconds
        ev += [(t, int(tok[0])) for tok in p[1:] if ids.get(tok[1:]) == a.channel]
    elif ids.get(p[0][1:]) == a.channel:
        ev.append((t, int(p[0][0])))

highs = collections.Counter()
frames, cur, started = [], [], False
for (t0, v0), (t1, _) in zip(ev, ev[1:]):
    d = t1 - t0
    if v0 == 1:
        highs[round(d / 42) * 42] += 1
        if started:
            cur.append(1 if d > 600 else 0)
    elif d > 50000:  # more than 50 us low ends a frame
        if started and cur:
            frames.append(cur)
        cur, started = [], True

print("high pulses (ns: count):", sorted(highs.items(), key=lambda x: -x[1])[:6])
print("bits per frame:", dict(collections.Counter(len(f) for f in frames)))
if not frames:
    sys.exit("no complete frame seen")
f = frames[0]
chips = collections.Counter(tuple(f[i:i + 90]) for i in range(0, len(f) - 89, 90))
for px, n in chips.most_common(6):
    words = [(px[i:i + 2], int("".join(map(str, px[i + 2:i + 18])), 2)) for i in range(0, 90, 18)]
    print(f"{n:3d} x", "  ".join(f"[{a_}{b_}]{v:5d}" for (a_, b_), v in words), "  (G R B CW WW)")
