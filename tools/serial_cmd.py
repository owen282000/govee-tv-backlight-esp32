#!/usr/bin/env python3
"""Send commands to the firmware over USB serial without resetting the ESP32.

    serial_cmd.py /dev/cu.usbserial-0001 "max 0.25" "c 0 8000 0 0 0"

Needs pyserial (PlatformIO ships it).
"""
import sys
import time

import serial

s = serial.Serial()
s.port, s.baudrate, s.timeout = sys.argv[1], 115200, 0.5
s.dtr = s.rts = False  # opening the port must not reset the board
s.open()
time.sleep(0.3)
s.reset_input_buffer()
for cmd in sys.argv[2:]:
    s.write((cmd + "\n").encode())
    time.sleep(0.3)
    print(">", cmd, "\n", s.read(2000).decode(errors="replace").strip())
s.close()
