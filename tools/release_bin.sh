#!/bin/bash
# Build a release and merge it into one image that flashes at offset 0 (bootloader, partition
# table, boot selector and app), for boards that still run other firmware.
#
#   tools/release_bin.sh [ENV] [VERSION]      default: div2, the version in screens.cpp
#
# Writes dist/fud-deck-ENV-VERSION.bin (full image, flash at 0x0) and
# dist/fud-deck-ENV-VERSION-app.bin (app only, for updating a board already on FUD-Deck at 0x10000).
# Needs PlatformIO; esptool comes from PlatformIO's Python, or set PYTHON to one that has it.
set -e
cd "$(dirname "$0")/.."
ENV=${1:-div2}
VER=${2:-v$(sed -n 's/^#define FUD_VERSION "\(.*\)"/\1/p' firmware/src/ui/screens.cpp)}
PIO=${PIO:-pio}
command -v "$PIO" >/dev/null || PIO=~/.platformio/penv/bin/pio
PY=${PYTHON:-$(dirname "$(command -v "$PIO")")/python}
[ -x "$PY" ] || PY=python3

(cd firmware && "$PIO" run -e "$ENV")
B=firmware/.pio/build/$ENV
BOOT_APP0=$(find ~/.platformio/packages/framework-arduinoespressif32*/tools/partitions -name boot_app0.bin | head -1)
[ -f "$BOOT_APP0" ] || { echo "boot_app0.bin not found in the Arduino framework package"; exit 1; }

mkdir -p dist
OUT=dist/fud-deck-$ENV-$VER.bin
"$PY" -m esptool --chip esp32s3 merge-bin -o "$OUT" \
  --flash-mode dio --flash-freq 80m --flash-size 16MB \
  0x0 "$B/bootloader.bin" 0x8000 "$B/partitions.bin" 0xe000 "$BOOT_APP0" 0x10000 "$B/firmware.bin"
cp "$B/firmware.bin" "dist/fud-deck-$ENV-$VER-app.bin"
ls -l dist/
