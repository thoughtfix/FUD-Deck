# facevec: Zuse's faces as vectors

FUD's smiley PNGs (thoughtfix, GPL-3.0) traced into flat-color vector data. All 15 faces take
about 7.6 KB of flash including the renderer, against 534 KB as raw RGB565, and draw with no
heap. This folder is only needed to re-trace; the firmware builds from the committed output.

## Files

```
tools/facevec/art/           the tracer's input: one of FUD's PNGs (defaultsmile) as an example
images/vectors/*.svg         SVG master per face (for viewing and editing)
src/ui/facevec.h             renderer: header-only, any gfx with fillRect/startWrite/endWrite
src/ui/facevec_data.h        generated face data (do not edit by hand)
src/ui/face.cpp              face::draw(): traced faces, plus extras drawn on "patch-only"
scripts/check_facevec.py     build pre-script: if all 15 PNGs are in art/, warns when they changed without a re-trace
tools/facevec/trace_face.py  ONE PNG > vectors (an SVG and a JSON). Nothing else.
tools/facevec/trace_faces.py wrapper: every PNG in art/ > shared shapes, facevec_data.h, SVG masters
tools/facevec/validate.py    builds render_host.cpp, renders with the real C++, diffs vs source
tools/facevec/run.sh         trace, validate, copy the results into place
tools/facepreview/run.sh     contact sheet of every face as the device draws it
```

## Trace one face

```
tools/facevec/trace_face.py my-face.png -o somewhere/     # writes my-face.svg and my-face.json
tools/facevec/trace_face.py face.png --bands 3            # a shaded extra color as 3 flat bands
```

Input: a FUD-style face, 135 x 135 (other sizes are scaled): a black-rimmed round disk with
flat-color features. Use tools/facevec/.venv/bin/python (run.sh creates it).

## Add a face to the firmware

Drop its PNG into `tools/facevec/art/` next to the others and run `run.sh`. The wrapper traces
every PNG there (FUD's 15 first, in their usual order, then the rest by name) and the face is
drawn by its file name: `face::draw("my-face", ...)`.

## Re-trace

Only one source PNG is in this repo. Copy the other 14 smileys from FUD-Firmware's `images/`
(thoughtfix, GPL-3.0) into `tools/facevec/art/` (or point `FACE_ART` at a folder), then:

```
tools/facevec/run.sh            # first run creates tools/facevec/.venv (numpy, scipy, scikit-image, Pillow)
tools/facevec/run.sh --tol 0.5  # looser simplification: smaller data, more stray pixels
```

Results land in src/ui/facevec_data.h and images/vectors/. The report (sizes, per-pixel
validation, drawing cost) and diff images go to tools/facevec/out/ (gitignored).

## Format (facevec_data.h)

- `FACES[]`: name, center, rim outer and inner radii (x and y, 1/8 px), base color, ref range.
- `REFS[]`: 4 bytes per shape use: shape, palette index, dx, dy (int8, quarter px).
- `SHAPES[]`: contour count, then per contour: point count, 16-bit start x/y, int8 steps.
- Coordinates are quarter source pixels (540 across the 135 px face box). Drawing scales to
  any size. Fill is even-odd with hard edges, sampled at pixel centers.

## Why not SVG on the device

An SVG parser and rasterizer costs tens of KB of code and heap for the parse. The packed
tables are 6.1 KB and draw with about 1.4 KB of code and no heap.
