# Face art

Zuse's faces are compiled from vectors: `src/ui/facevec_data.h`, with an SVG master per face in
`vectors/`. FUD's original PNGs (thoughtfix, GPL-3.0) were the source for the trace. They live
in [FUD-Firmware](https://github.com/thoughtfix/FUD-Firmware)'s `images/`; one is kept as an
example in `tools/facevec/art/`, the tracer's input folder.

To add or change a face: drop the PNG into `tools/facevec/art/` and run `tools/facevec/run.sh`.
It traces, installs the result into `src/ui/` and `vectors/`, and checks it. See
`tools/facevec/README.md`.
