#!/bin/bash
# Preview Zuse's drawn faces on this computer: builds src/ui/face.cpp natively, writes a PNG.
#   tools/facepreview/run.sh [out.png]     (default: tools/facepreview/faces.png)
set -e
D="$(cd "$(dirname "$0")" && pwd)"
OUT="${1:-$D/faces.png}"
c++ -std=c++17 -O2 -DFACE_PREVIEW -I"$D" "$D/main.cpp" "$D/../../src/ui/face.cpp" -o "$D/facepreview"
"$D/facepreview" "$D/faces.ppm" >/dev/null
sips -s format png "$D/faces.ppm" --out "$OUT" >/dev/null
rm -f "$D/faces.ppm"
echo "$OUT"
