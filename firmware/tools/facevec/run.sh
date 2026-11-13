#!/bin/bash
# Re-trace FUD's face PNGs (tools/facevec/art/) into src/ui/facevec_data.h and the SVG masters
# (firmware/images/vectors/), then validate against the source with the real renderer.
# Needs Python 3.11-3.13 (numpy/scipy wheels) and clang++. First run creates .venv here.
#   ./run.sh            ./run.sh --tol 0.5   (looser: smaller data, more stray pixels)
set -e
cd "$(dirname "$0")"
PY=${PYTHON:-python3.13}
if [ ! -x .venv/bin/python ]; then
  "$PY" -m venv .venv
  .venv/bin/pip install -q numpy scipy scikit-image pillow
fi
.venv/bin/python trace_faces.py "$@"
cp out/facevec_data.h ../../src/ui/facevec_data.h   # before validate: facevec.h includes it
.venv/bin/python validate.py
for f in out/svg/*.svg; do case "$f" in *.min.svg) ;; *) cp "$f" ../../images/vectors/ ;; esac; done
echo "Done. Report: tools/facevec/out/REPORT.md, images: tools/facevec/out/validate/"
