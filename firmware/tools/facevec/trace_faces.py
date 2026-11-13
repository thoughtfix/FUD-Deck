#!/usr/bin/env python3
"""Wrapper: trace every face PNG in a folder and build the firmware's face data.

Each PNG is traced by trace_face.py (one face > vectors). This wrapper then reuses shapes
between faces (a translated copy within limits becomes a reference), builds one palette, and
writes the compact data.

Outputs (under firmware/tools/facevec/; run.sh copies the keepers into place):
  out/svg/<face>.svg          transparent SVG master per face
  out/svg/<face>.min.svg      minified copy (for size comparison)
  out/facevec_data.h          compact data for firmware/facevec.h
  out/trace.json              geometry and stats, read by validate.py

Faces: every *.png in art/ (or FACE_ART). FUD's 15 come first in their usual order, any others
follow by name. Add a face by dropping its PNG in and re-running.

Usage: ./run.sh [--tol 0.35]
"""
import argparse, glob, json, os, sys
import numpy as np
from scipy import ndimage as ndi
import trace_face as TF
from trace_face import (N, U, K, ART, PALETTE, ORDER, BRUISE_BANDS, RELAXED, rgb565, from565,
                        in_ellipse, best_shift, color_of, fmt)

ROOT = os.path.dirname(os.path.abspath(__file__))              # firmware/tools/facevec
FW = os.path.dirname(os.path.dirname(ROOT))                     # firmware
# Face PNGs: art/ holds one example; drop the rest of FUD-Firmware's images/ in beside it.
SRC = os.environ.get("FACE_ART") or os.path.join(ROOT, "art")
UI = os.path.join(FW, "src", "ui")                              # facevec.h, facevec_data.h
OUT = os.path.join(ROOT, "out")                                 # work files, gitignored

# FUD's smileys, in the order the firmware has always had them.
FUD_FACES = ["defaultsmile", "glance-left", "glance-right", "winkingface", "yawning-face",
             "total-blank-face", "sadface", "grinningface", "neutralface", "shockedface",
             "tongueface", "redface", "heartface", "swirly-face", "abused-face"]
# FUD art that is not a Zuse face: never traced.
SKIP = {"zerocool", "acidburn", "lord-nikon"}
# Per-face trace options. --bands for others: name=N on the command line.
BANDS = {"abused-face": BRUISE_BANDS}


def discover():
    have = {os.path.splitext(os.path.basename(p))[0] for p in glob.glob(os.path.join(SRC, "*.png"))}
    have -= SKIP
    return [f for f in FUD_FACES if f in have] + sorted(have - set(FUD_FACES))


FACES = discover()   # validate.py reads this
ALL = list(FACES)


def load(name):
    return TF.load(os.path.join(SRC, name + ".png"))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tol", type=float, default=0.35, help="simplify tolerance, source px")
    ap.add_argument("--relaxed-tol", type=float, default=0.8, help="tolerance for RELAXED features")
    ap.add_argument("--share-haus", type=float, default=0.75, help="max px error to reuse a shape")
    ap.add_argument("--share-xor", type=float, default=0.02, help="max XOR / area to reuse a shape")
    ap.add_argument("--bands", action="append", default=[], metavar="NAME=N",
                    help="split a face's shaded extra color into N bands (abused-face=3 is built in)")
    args = ap.parse_args()
    if not FACES:
        sys.exit("no face PNGs in %s" % SRC)
    bands = dict(BANDS)
    for b in args.bands:
        k, v = b.split("=")
        bands[k] = int(v)
    os.makedirs(os.path.join(OUT, "svg"), exist_ok=True)

    faces = {}
    for name in FACES:
        faces[name] = TF.trace(os.path.join(SRC, name + ".png"), name, args.tol, args.relaxed_tol,
                               bands.get(name, 0))

    # FUD specifics, only when those faces are in the set. heartface's source art has no
    # patch: borrow defaultsmile's, placed relative to the disk center. patch-only is
    # defaultsmile's disk and patch with no eye or mouth, a base for faces drawn in code.
    if "defaultsmile" in faces:
        src = faces["defaultsmile"]
        patch = max((s for s in src["shapes"] if s["role"] == "black"),
                    key=lambda s: s["mask1"].sum())
        sd = src["disk"]
        if "heartface" in faces:
            hd = faces["heartface"]["disk"]
            ddx = int(round((hd["cx"] - sd["cx"]) * K)); ddy = int(round((hd["cy"] - sd["cy"]) * K))
            faces["heartface"]["shapes"].append({"role": "black", "contours": patch["contours"],
                                                 "mask1": None, "added": True, "shift": [ddx, ddy]})
        faces["patch-only"] = {"disk": sd, "base": "yellow", "roles": src["roles"], "unexplained_px": 0,
                               "shapes": [{"role": "black", "contours": patch["contours"], "mask1": None,
                                           "added": True, "shift": [0, 0]}]}
        ALL.append("patch-only")

    # sharing: reuse an earlier shape when a translated copy matches within limits
    table = []           # unique shapes: contours
    for name in ALL:
        f = faces[name]
        refs = []
        for s in f["shapes"]:
            if s.get("added"):
                idx = next(i for i, t in enumerate(table) if t["contours"] == s["contours"])
                refs.append({"shape": idx, "role": s["role"], "dx": s["shift"][0], "dy": s["shift"][1]})
                continue
            tgt = s["mask1"]
            hit = None
            for i, t in enumerate(table):
                if t["role"] != s["role"] or abs(t["area"] - tgt.sum()) > 0.08 * tgt.sum():
                    continue
                # coarse align by centroid, then search
                ty, tx = ndi.center_of_mass(tgt)
                dx0 = int(round((tx - t["cx"]) * K)); dy0 = int(round((ty - t["cy"]) * K))
                moved = [[[p[0] + dx0, p[1] + dy0] for p in c] for c in t["contours"]]
                dx, dy, x, haus = best_shift(moved, tgt, 2)
                if x <= args.share_xor * tgt.sum() and haus <= args.share_haus:
                    own_v = sum(len(c) for c in s["contours"])
                    hit = (i, dx0 + dx, dy0 + dy, x, haus)
                    break
            if hit:
                i, dx, dy, x, haus = hit
                refs.append({"shape": i, "role": s["role"], "dx": dx, "dy": dy,
                             "shared_xor": x, "shared_haus": haus})
            else:
                cy_, cx_ = ndi.center_of_mass(tgt)
                table.append({"role": s["role"], "contours": s["contours"], "area": int(tgt.sum()),
                              "cx": cx_, "cy": cy_, "first": name})
                refs.append({"shape": len(table) - 1, "role": s["role"], "dx": 0, "dy": 0})
        f["refs"] = refs

    write_outputs(faces, table, args)


def write_outputs(faces, table, args):
    # global palette in RGB565
    pal = []
    def pidx(c):
        v = rgb565(c)
        if v not in pal:
            pal.append(v)
        return pal.index(v)
    pidx(PALETTE["black"])

    # ---------- C data
    shape_bytes = bytearray()
    shape_ofs = []
    for t in table:
        shape_ofs.append(len(shape_bytes))
        shape_bytes.append(len(t["contours"]))
        for c in t["contours"]:
            assert len(c) <= 255
            shape_bytes.append(len(c))
            shape_bytes += bytes([c[0][0] & 255, c[0][0] >> 8, c[0][1] & 255, c[0][1] >> 8])
            for (x0, y0), (x1, y1) in zip(c[:-1], c[1:]):
                dx, dy = x1 - x0, y1 - y0
                assert -127 <= dx <= 127 and -127 <= dy <= 127
                shape_bytes += bytes([dx & 255, dy & 255])
    ref_bytes = bytearray()
    face_rows = []
    for name in ALL:
        f = faces[name]
        d = f["disk"]
        base = pidx(color_of(f, f["base"]))
        start = len(ref_bytes) // 4
        for r in f["refs"]:
            assert -128 <= r["dx"] <= 127 and -128 <= r["dy"] <= 127
            ref_bytes += bytes([r["shape"], pidx(color_of(f, r["role"])), r["dx"] & 0xFF, r["dy"] & 0xFF])
        face_rows.append((name, round(d["cx"] * 8), round(d["cy"] * 8), round(d["rox"] * 8),
                          round(d["roy"] * 8), round(d["rix"] * 8), round(d["riy"] * 8),
                          base, start, len(f["refs"])))
    assert len(table) <= 255 and len(pal) <= 255

    L = ["// Generated by tools/facevec/trace_faces.py from the FUD face PNGs (thoughtfix, GPL-3.0).",
         "// Do not edit. Format is described in facevec.h.",
         "// source-sha1: %s" % source_hash(), "// source-faces: %s" % ",".join(FACES),
         "#pragma once", "#include <stdint.h>", "",
         "namespace facevec_data {", ""]
    L.append("static const uint16_t PALETTE[] = {" + ", ".join("0x%04X" % v for v in pal) + "};")
    L.append("")
    L.append("// Art units: quarter source pixels, ART across the face box.")
    L.append("static const int ART = %d;" % ART)
    L.append("// Shapes: [contour count], then per contour: [point count] [x0 lo, x0 hi, y0 lo, y0 hi]")
    L.append("// then (count - 1) int8 steps [dx dy]. The contour closes back to its first point.")
    L.append("static const uint8_t SHAPES[] = {")
    for i in range(0, len(shape_bytes), 24):
        L.append("  " + ",".join(str(b) for b in shape_bytes[i:i + 24]) + ",")
    L.append("};")
    L.append("static const uint16_t SHAPE_OFS[] = {" + ",".join(str(o) for o in shape_ofs) + "};")
    L.append("")
    L.append("// Refs: shape index, palette index, dx, dy (signed art units)")
    L.append("static const uint8_t REFS[] = {")
    for i in range(0, len(ref_bytes), 24):
        L.append("  " + ",".join(str(b) for b in ref_bytes[i:i + 24]) + ",")
    L.append("};")
    L.append("")
    L.append("struct Face { const char* name; uint16_t cx, cy, rox, roy, rix, riy; uint8_t base, ref0, nref; };")
    L.append("// Center and rim radii (outer x/y, inner x/y) in 1/8 source px. The disks are ellipses")
    L.append("// because some source faces are slightly squashed.")
    L.append("static const Face FACES[] = {")
    for row in face_rows:
        L.append('  {"%s", %d, %d, %d, %d, %d, %d, %d, %d, %d},' % row)
    L.append("};")
    L.append("static const int FACE_COUNT = %d;" % len(face_rows))
    L.append("")
    L.append("}  // namespace facevec_data")
    with open(os.path.join(OUT, "facevec_data.h"), "w") as fh:
        fh.write("\n".join(L) + "\n")

    # ---------- SVG masters (from the quantized data, so they match the firmware exactly)
    svg_sizes = {}
    for name, row in zip(ALL, face_rows):
        f = faces[name]
        _, cx, cy, rox, roy, rix, riy, base, start, n = row
        def hexc(v):
            r, g, b = from565(v)
            return "#%02x%02x%02x" % (r, g, b)
        E = '<ellipse cx="%s" cy="%s" rx="%s" ry="%s" fill="%s"/>'
        parts = [E % (fmt(cx / 8), fmt(cy / 8), fmt(rox / 8), fmt(roy / 8), "#000"),
                 E % (fmt(cx / 8), fmt(cy / 8), fmt(rix / 8), fmt(riy / 8), hexc(pal[base]))]
        for r in f["refs"]:
            t = table[r["shape"]]
            dpath = ""
            for c in t["contours"]:
                pts = [((x + r["dx"]) / K, (y + r["dy"]) / K) for x, y in c]
                dpath += "M" + " ".join("%s %s" % (fmt(x), fmt(y)) for x, y in pts) + "Z"
            col = hexc(pal[pidx(color_of(f, r["role"]))])
            parts.append('<path fill="%s" fill-rule="evenodd" d="%s"/>' % (col, dpath))
        body = "".join(parts)
        full = ('<?xml version="1.0" encoding="UTF-8"?>\n'
                '<svg xmlns="http://www.w3.org/2000/svg" width="135" height="135" viewBox="0 0 135 135">\n'
                '<!-- %s: traced from FUD art (thoughtfix, GPL-3.0). Transparent background. -->\n%s\n</svg>\n'
                % (name, body.replace("/>", "/>\n")))
        mini = '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 135 135">%s</svg>' % body
        with open(os.path.join(OUT, "svg", name + ".svg"), "w") as fh:
            fh.write(full)
        with open(os.path.join(OUT, "svg", name + ".min.svg"), "w") as fh:
            fh.write(mini)
        svg_sizes[name] = {"svg": len(full.encode()), "min_svg": len(mini.encode())}

    # ---------- stats for validate.py
    js = {"tol": args.tol, "palette565": pal, "faces": {}, "shape_bytes": len(shape_bytes),
          "ref_bytes": len(ref_bytes), "face_table_bytes": len(face_rows) * 20,
          "palette_bytes": 2 * len(pal), "shape_ofs_bytes": 2 * len(shape_ofs),
          "shapes": [{"role": t["role"], "first": t["first"], "verts": sum(len(c) for c in t["contours"]),
                      "bytes": 1 + sum(3 + 2 * len(c) for c in t["contours"])} for t in table]}
    for name in ALL:
        f = faces[name]
        js["faces"][name] = {"disk": f["disk"], "base": f["base"], "roles": f["roles"],
                             "unexplained_px": f["unexplained_px"],
                             "refs": f["refs"], "svg": svg_sizes[name]}
    with open(os.path.join(OUT, "trace.json"), "w") as fh:
        json.dump(js, fh, indent=1, default=float)
    total = len(shape_bytes) + len(ref_bytes) + len(face_rows) * 20 + 2 * len(pal) + 2 * len(shape_ofs)
    print(f"shapes={len(table)} shape_bytes={len(shape_bytes)} refs={len(ref_bytes)} palette={len(pal)} total~{total} B")


def source_hash():
    """One hash over the source PNGs, so a build can tell when the art changed."""
    import hashlib
    h = hashlib.sha1()
    for name in FACES:
        with open(os.path.join(SRC, name + ".png"), "rb") as fh:
            h.update(name.encode() + b"\0" + fh.read())
    return h.hexdigest()


if __name__ == "__main__":
    main()
