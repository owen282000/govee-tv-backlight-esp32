#!/usr/bin/env python3
"""Write the ESP Web Tools manifest for one build: make_manifest.py <env> <version>."""
import json
import sys

env, version = sys.argv[1], sys.argv[2]
names = {"esp32dev": "ESP32 dev board", "gledopto": "Gledopto GL-C-015WL-M"}
print(json.dumps({
    "name": f"Govee TV Backlight ESP32 ({names[env]})",
    "version": version,
    "new_install_prompt_erase": True,
    "builds": [{"chipFamily": "ESP32", "parts": [{"path": f"govee-tv-backlight-{env}-full.bin", "offset": 0}]}],
}, indent=2))
