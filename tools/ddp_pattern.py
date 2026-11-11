#!/usr/bin/env python3
"""Show a pattern on the strip to find where each chip sits around your TV.

Chip 1 red, every 10th chip green, the last chip blue, the rest dim white.
Walk around the TV and note where red starts, which way the greens run and
where blue ends. That gives you the LED layout for Hyperion.

    ddp_pattern.py <esp32-ip> [--chips 90] [--seconds 120]
"""
import argparse
import socket
import time

ap = argparse.ArgumentParser()
ap.add_argument("ip")
ap.add_argument("--chips", type=int, default=90)
ap.add_argument("--seconds", type=float, default=120)
a = ap.parse_args()

px = []
for i in range(a.chips):
    if i == 0:
        px.append((255, 0, 0))
    elif i == a.chips - 1:
        px.append((0, 0, 255))
    elif i % 10 == 0:
        px.append((0, 255, 0))
    else:
        px.append((40, 40, 40))
data = bytes(v for p in px for v in p)
# DDP: version 1 + push, type RGB 8 bit, offset 0, length
hdr = bytes([0x41, 0, 0x0B, 1, 0, 0, 0, 0, len(data) >> 8, len(data) & 255])
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
end = time.time() + a.seconds
print(f"sending to {a.ip}:4048 for {a.seconds:.0f} s, Ctrl+C to stop")
while time.time() < end:
    s.sendto(hdr + data, (a.ip, 4048))  # keep sending: the firmware falls back after 3 s
    time.sleep(0.1)
