#!/usr/bin/env bash
# Merge bootloader, partition table, boot_app0 and the app into one image that
# flashes at offset 0. ESP Web Tools needs this single file.
#   tools/merge_bin.sh <env> <output.bin>     run from the repo root after "pio run -e <env>"
set -euo pipefail
ENV="$1"
OUT="$2"
BUILD="firmware/.pio/build/$ENV"
PIO_HOME="${PLATFORMIO_CORE_DIR:-$HOME/.platformio}"
BOOT_APP0="$PIO_HOME/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin"
PY="${PYTHON:-python3}"
# esptool from PlatformIO if it's there, otherwise the pip package ("pip install esptool").
ESPTOOL="${ESPTOOL:-$PIO_HOME/packages/tool-esptoolpy/esptool.py}"
if [ -f "$ESPTOOL" ]; then RUN=("$PY" "$ESPTOOL"); else RUN=("$PY" -m esptool); fi

"${RUN[@]}" --chip esp32 merge_bin -o "$OUT" \
  --flash_mode keep --flash_freq keep --flash_size 4MB \
  0x1000 "$BUILD/bootloader.bin" \
  0x8000 "$BUILD/partitions.bin" \
  0xe000 "$BOOT_APP0" \
  0x10000 "$BUILD/firmware.bin"
echo "merged: $OUT"
