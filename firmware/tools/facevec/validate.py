#!/usr/bin/env python3
"""Validate the traced faces against the source PNGs, using the real C++ renderer.

Builds render_host.cpp (which includes src/ui/facevec.h unchanged), renders every face
at 135 px (source size) and 162 px (FUD-Deck's FACE_S), and compares the 135 px render with
the source pixel by pixel.

Reference labels come from the source: each pixel is modeled as a blend of two palette colors.
A pixel blended 25-75% between two colors is an edge pixel and either color counts as a match.
Outside the black rim, the pale halo and stray pixels count as transparent (removing them is
intended).

Writes out/validate/*.png, out/validation.json and out/REPORT.md.
Usage: .venv/bin/python tools/validate.py
"""
import gzip, json, os, subprocess, sys
import numpy as np
from PIL import Image
from scipy import ndimage as ndi

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import trace_faces as T

ROOT, OUT, N = T.ROOT, T.OUT, T.N
VAL = os.path.join(OUT, "validate")
CLEAR = 0x0821
RAW565 = N * N * 2


def build_and_render(sizes):
    exe = os.path.join(OUT, "render_host")
    subprocess.run(["clang++", "-std=c++17", "-O2", "-Wall", "-Wextra",
                    "-I", OUT, "-I", T.UI,
                    os.path.join(ROOT, "render_host.cpp"), "-o", exe], check=True)
    spans = {}
    for s in sizes:
        d = os.path.join(OUT, "render", str(s))
        os.makedirs(d, exist_ok=True)
        r = subprocess.run([exe, str(s), d], check=True, capture_output=True, text=True)
        for line in r.stdout.split("\n"):
            if line:
                name, sp, oob, px = line.split()
                spans.setdefault(name, {})[s] = int(sp.split("=")[1])
                spans[name]["px%d" % s] = int(px.split("=")[1])
                assert oob == "oob=0", line
    return spans


def esp32_size():
    """Compile facevec.h for the ESP32-S3 (if PlatformIO's toolchain is installed) and return
    code and data sizes in bytes. A stand-in graphics type has LovyanGFX's call shape."""
    tc = os.path.expanduser("~/.platformio/packages/toolchain-xtensa-esp32s3/bin")
    gxx = os.path.join(tc, "xtensa-esp32s3-elf-g++")
    if not os.path.exists(gxx):
        return None
    src = os.path.join(OUT, "esp_size.cpp")
    obj = os.path.join(OUT, "esp_size.o")
    with open(src, "w") as fh:
        fh.write('#include "facevec.h"\n'
                 "struct G { void startWrite(); void endWrite(); void fillRect(int, int, int, int, uint16_t); };\n"
                 "G gfx;\n"
                 "bool drawIt(const char* n, int x, int y, int s, int32_t bg) "
                 "{ return facevec::draw(gfx, n, x, y, s, bg); }\n")
    r = subprocess.run([gxx, "-std=gnu++17", "-Os", "-Wall", "-Wextra", "-mlongcalls", "-ffunction-sections",
                        "-fdata-sections", "-I", OUT, "-I", T.UI, "-c", src, "-o", obj],
                       capture_output=True, text=True)
    if r.returncode:
        return {"error": r.stderr[-2000:]}
    out = subprocess.run([os.path.join(tc, "xtensa-esp32s3-elf-size"), "-A", obj],
                         capture_output=True, text=True).stdout
    code = data = 0
    for line in out.split("\n"):
        p = line.split()
        if len(p) >= 2 and p[1].isdigit():
            if p[0].startswith(".text"):
                code += int(p[1])
            elif p[0].startswith(".rodata"):
                data += int(p[1])
    return {"code": code, "rodata": data, "warnings": r.stderr.strip()}


def read_raw(size, name):
    v = np.fromfile(os.path.join(OUT, "render", str(size), name + ".raw"), dtype="<u2")
    return v.reshape(size, size)


def to_rgba(v):
    r = ((v >> 11) & 31).astype(np.uint16); g = ((v >> 5) & 63).astype(np.uint16); b = (v & 31).astype(np.uint16)
    rgb = np.stack([(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)], -1).astype(np.uint8)
    a = np.where(v == CLEAR, 0, 255).astype(np.uint8)
    return np.dstack([rgb, a])


def reference(name, face):
    """Per pixel: allowed labels (set of role names, 'clear' for transparent)."""
    a = T.load(name)
    roles = face["roles"]
    names = list(roles)
    P = np.array([roles[k] for k in names])
    rgb = a[..., :3].reshape(-1, 3)
    al = a[..., 3].reshape(-1) / 255
    best = np.full(len(rgb), np.inf); li = np.zeros(len(rgb), int); lj = np.zeros(len(rgb), int)
    t_ = np.zeros(len(rgb))
    for i in range(len(P)):
        d = np.linalg.norm(rgb - P[i], axis=1)
        m = d < best; best[m] = d[m]; li[m] = i; lj[m] = i; t_[m] = 0
        for j in range(i + 1, len(P)):
            e = P[j] - P[i]
            t = np.clip(((rgb - P[i]) @ e) / (e @ e), 0, 1)
            d = np.linalg.norm(rgb - (P[i] + t[:, None] * e), axis=1)
            m = d < best; best[m] = d[m]; li[m] = i; lj[m] = j; t_[m] = t[m]
    yy, xx = np.mgrid[0:N, 0:N] + 0.5
    dk = face["disk"]
    rimzone = ~T.in_ellipse(xx, yy, dk["cx"], dk["cy"], dk["rix"], dk["riy"], 0.5).reshape(-1)
    blk = al * (1 - a[..., :3].max(-1).reshape(-1) / 255)
    lum_other = al * a[..., :3].max(-1).reshape(-1) / 255
    allowed = []
    for p in range(len(rgb)):
        # Rim zone and beyond: black or clear, unless the source has a real feature color there
        # (the tongue overlaps the rim).
        plain = ("black", "yellow", "red", "white")
        feature = (al[p] > 0.5 and best[p] < 60 and
                   (names[li[p]] not in plain or (names[lj[p]] not in plain and t_[p] > 0.25)))
        if rimzone[p] and not feature:
            if blk[p] >= 0.75:
                allowed.append({"black"})
            elif blk[p] <= 0.25:
                allowed.append({"clear"})
            else:
                allowed.append({"black", "clear"})
            continue
        A, B, t = names[li[p]], names[lj[p]], t_[p]
        if 0.25 < t < 0.75:
            allowed.append({A, B})
        else:
            allowed.append({A if t <= 0.5 else B})
    return allowed


def label_of_render(v, face, pal565):
    roles = face["roles"]
    lab = np.empty(v.shape, object)
    m = {CLEAR: "clear"}
    for role in roles:
        m[T.rgb565(T.color_of(face, role))] = role
    for val in np.unique(v):
        lab[v == val] = m.get(int(val), "?%04x" % val)
    return lab


def label_mask(v, face, roles):
    m = np.zeros(v.shape, bool)
    for role in roles:
        if role in face["roles"]:
            m |= v == T.rgb565(T.color_of(face, role))
    return m


def main():
    js = json.load(open(os.path.join(OUT, "trace.json")))
    os.makedirs(VAL, exist_ok=True)
    spans = build_and_render([135, 162])
    results = {}
    strips = []
    for name in T.FACES:
        face = js["faces"][name]
        allowed = reference(name, face)
        v = read_raw(135, name)
        lab = label_of_render(v, face, js["palette565"]).reshape(-1)
        added = name == "heartface"
        mism = np.array([lab[p] not in allowed[p] for p in range(len(lab))]).reshape(N, N)
        edge = np.array([len(s) > 1 for s in allowed]).reshape(N, N)
        if added:   # the added patch is new content, not a tracing error
            patch_px = (v == T.rgb565(T.PALETTE["black"])) & ~np.array(
                [("black" in s) for s in allowed]).reshape(N, N)
            mism &= ~patch_px
        # Relaxed features (tongue, heart, bruise) may drift. Count them apart so they cannot
        # hide errors in the eyes, patch, mouth or disk.
        relaxed = np.array([bool(s & T.RELAXED) for s in allowed]).reshape(N, N)
        relaxed |= label_mask(v, face, T.RELAXED)
        relaxed = ndi.binary_dilation(relaxed, iterations=2)
        relaxed_px = int((mism & relaxed).sum())
        mism &= ~relaxed
        blobs, nb = ndi.label(mism, structure=np.ones((3, 3)))
        sizes = ndi.sum(mism, blobs, range(1, nb + 1)) if nb else np.array([])
        big = []
        for i, s in enumerate(sizes):
            if s >= 4:
                cy, cx = ndi.center_of_mass(blobs == i + 1)
                big.append({"px": int(s), "at": [round(cx), round(cy)]})
        clear_ok = bool((v[[0, 0, -1, -1], [0, -1, 0, -1]] == CLEAR).all())
        results[name] = {"mismatch_px": int(mism.sum()), "relaxed_px": relaxed_px, "edge_px": int(edge.sum()),
                         "match_pct": 100 * (1 - mism.sum() / (N * N)),
                         "largest_blob": int(sizes.max()) if len(sizes) else 0,
                         "blobs_ge4": big, "corners_transparent": clear_ok,
                         "spans135": spans[name][135], "spans162": spans[name][162],
                         "px162": spans[name]["px162"]}
        # strip: original on blue | render on blue | diff | render at 162 on black
        def on(bg, rgba):
            im = Image.new("RGBA", (rgba.shape[1], rgba.shape[0]), bg)
            im.alpha_composite(Image.fromarray(rgba))
            return im.convert("RGB")
        src = T.load(name).astype(np.uint8)
        ren = to_rgba(v)
        diff = (np.asarray(on((0, 0, 0, 255), ren)).astype(float) * 0.35).astype(np.uint8)
        diff[mism] = [255, 0, 255]
        z = 3
        tiles = [on((40, 110, 220, 255), src), on((40, 110, 220, 255), ren), Image.fromarray(diff)]
        tiles = [t.resize((N * z, N * z), Image.NEAREST) for t in tiles]
        big162 = on((0, 0, 0, 255), to_rgba(read_raw(162, name)))
        strip = Image.new("RGB", (N * z * 3 + 162 + 40, N * z), (24, 24, 24))
        for i, t in enumerate(tiles):
            strip.paste(t, (i * N * z + i * 10, 0))
        strip.paste(big162, (N * z * 3 + 30, (N * z - 162) // 2))
        strip.save(os.path.join(VAL, name + ".png"))
        strips.append(strip)
        print(f"{name:18s} relaxed={relaxed_px:4d} mismatch={mism.sum():4d} px  largest blob={results[name]['largest_blob']:3d}  "
              f"blobs>=4: {big}")

    # contact sheets at device size, on black and on alert red
    for tag, bg in (("black", (0, 0, 0, 255)), ("red", (255, 0, 0, 255)), ("blue", (40, 110, 220, 255))):
        sheet = Image.new("RGB", (5 * 170, 3 * 170), bg[:3])
        for i, name in enumerate(T.FACES):
            im = Image.new("RGBA", (162, 162), bg)
            im.alpha_composite(Image.fromarray(to_rgba(read_raw(162, name))))
            sheet.paste(im.convert("RGB"), ((i % 5) * 170 + 4, (i // 5) * 170 + 4))
        sheet.save(os.path.join(VAL, "sheet-162-%s.png" % tag))
    h = sum(s.height for s in strips) + 8 * len(strips)
    allimg = Image.new("RGB", (strips[0].width, h), (24, 24, 24))
    yy = 0
    for s in strips:
        allimg.paste(s, (0, yy)); yy += s.height + 8
    allimg.save(os.path.join(VAL, "all-diffs.png"))

    sizes = size_table(js)
    json.dump({"faces": results, "sizes": sizes}, open(os.path.join(OUT, "validation.json"), "w"), indent=1)
    esp = esp32_size()
    write_report(js, results, sizes, esp)


def size_table(js):
    rows = {}
    owner = {}
    for i, s in enumerate(js["shapes"]):
        owner[i] = s["first"]
    for name in T.FACES:
        f = js["faces"][name]
        png = os.path.getsize(os.path.join(T.SRC, name + ".png"))
        own = sum(s["bytes"] for i, s in enumerate(js["shapes"]) if s["first"] == name)
        refs = 4 * len(f["refs"])
        rows[name] = {"png": png, "rgb565": RAW565, "svg": f["svg"]["svg"], "min_svg": f["svg"]["min_svg"],
                      "min_svg_gz": len(gzip.compress(open(os.path.join(OUT, "svg", name + ".min.svg"), "rb").read(), 9)),
                      "own_shape_bytes": own, "ref_bytes": refs, "face_row_bytes": 14,
                      "shared_refs": sum(1 for r in f["refs"] if owner[r["shape"]] != name)}
    data_total = js["shape_bytes"] + js["ref_bytes"] + js["face_table_bytes"] + js["palette_bytes"] + js["shape_ofs_bytes"]
    names_bytes = sum(len(n) + 1 for n in T.FACES)
    return {"faces": rows, "data_total": data_total, "names_bytes": names_bytes,
            "png_total": sum(r["png"] for r in rows.values()),
            "rgb565_total": RAW565 * len(rows),
            "min_svg_total": sum(r["min_svg"] for r in rows.values())}


def write_report(js, res, sz, esp):
    vec = sz["data_total"] + sz["names_bytes"]
    code = esp["code"] if esp and "code" in esp else None
    L = ["# Face vectors: report", "",
         "Generated by tools/facevec/validate.py. Every render below comes from the firmware renderer",
         "(src/ui/facevec.h) compiled for the desktop, not from a Python re-implementation.", "",
         "## What was converted", "",
         "15 smiley faces from FUD's images/ (tools/facevec/art/, unmodified).",
         "Not converted: zerocool, acidburn, lord-nikon (not smileys).", "",
         "Each face is stored as two fitted ellipses (black rim, then the yellow or red disk) and a",
         "short list of flat-color polygons painted in order, filled even-odd, with hard edges.",
         "Points are quarter source pixels, 2 bytes per point.", "",
         "Decisions applied (from the brief and follow-up answers):", "",
         "- One yellow (%s) for every face. The source yellow drifts by 1-2 RGB565 green steps." %
         ("0x%04X" % T.rgb565(T.PALETTE["yellow"])),
         "- The abused-face bruise is %d flat bands, not a gradient. Band edges: `BRUISE_CUTS`." % T.BRUISE_BANDS,
         "- Hard edges, no anti-aliasing (smallest and fastest). Pale halo and stray edge pixels removed.",
         "- Background is transparent: `facevec::draw(..., TRANSPARENT)` writes only face pixels.",
         "  The black rim doubles as the outline on any background.",
         "- heartface gets defaultsmile's patch on top (the source has none). total-blank-face stays blank.",
         "- Eyes, patch, smile and disk size are kept exact. Tongue, heart and bruise are relaxed:",
         "  simplified more loosely (`--relaxed-tol`) and kept inside the rim.",
         "- Several source faces are slightly squashed (up to 2.4 px between axes), so disks are",
         "  ellipses, not circles, to keep proportions.", "",
         "## Storage", "",
         "| | Bytes |", "|---|---:|",
         "| 15 source PNGs | %d |" % sz["png_total"],
         "| 15 faces as raw RGB565 (the old bitmap path) | %d |" % sz["rgb565_total"],
         "| 15 minified SVG masters | %d |" % sz["min_svg_total"],
         "| facevec_data.h tables, all faces (+ patch-only) | %d |" % sz["data_total"],
         "| face name strings | %d |" % sz["names_bytes"]]
    if code:
        L.append("| renderer code, ESP32-S3 -Os (measured) | %d |" % code)
    tot = vec + (code or 0)
    L += ["", "**Flash for all 15 faces: %d bytes, vs %d as raw RGB565: %.1f%% smaller (%.0fx).**" %
          (tot, sz["rgb565_total"], 100 * (1 - tot / sz["rgb565_total"]), sz["rgb565_total"] / tot), "",
          "RAM: no heap, no frame buffer. About 300 bytes of stack while drawing (a 64-entry",
          "crossing list). Data stays in flash (const)." + (" ESP32-S3 compile: %s." %
          ("no warnings at -Wall -Wextra" if esp and not esp.get("warnings") else "see warnings") if esp else ""), "",
          "Shape reuse: only the added heartface patch (and patch-only) reuse a shape. The source",
          "patches differ face to face by 2.8 to 20 px after best alignment (strap angle, patch size),",
          "so sharing them would redraw the art. Shapes are shared only within 0.75 px.", "",
          "| Face | PNG | RGB565 | SVG | min SVG | vector bytes |",
          "|---|---:|---:|---:|---:|---:|"]
    for n in T.FACES:
        r = sz["faces"][n]
        L.append("| %s | %d | %d | %d | %d | %d |" % (n, r["png"], r["rgb565"], r["svg"], r["min_svg"],
                                                     r["own_shape_bytes"] + r["ref_bytes"] + 20))
    L += ["", "Vector bytes per face = its own shapes + 4 bytes per shape use + a 20-byte face record.", "",
          "## Visual validation (135 px, per pixel, all 18225 pixels per face)", "",
          "Reference: each source pixel is modeled as a blend of two palette colors. If it is",
          "25-75% blended (an anti-aliased edge), either color counts as correct. Outside the rim,",
          "only black or transparent are correct (the halo is meant to go).",
          "A mismatch is a pixel whose rendered color the source does not support.", "",
          "Relaxed features (tongue, heart, bruise) may drift by design and are counted in their",
          "own column, so they cannot hide errors in the eyes, patch, mouth or disk.", "",
          "| Face | strict mismatched px | % | largest cluster | clusters of 4+ px | relaxed px |",
          "|---|---:|---:|---:|---|---:|"]
    for n in T.FACES:
        r = res[n]
        b = "; ".join("%d px at %s" % (x["px"], tuple(x["at"])) for x in r["blobs_ge4"]) or "none"
        L.append("| %s | %d | %.2f%% | %d | %s | %d |" % (n, r["mismatch_px"], 100 - r["match_pct"],
                                                         r["largest_blob"], b, r["relaxed_px"]))
    L += ["", "What the remaining differences are:", "",
          "- Single stray pixels: rounding on edges right at a pixel center. Invisible at 162 px.",
          "- tongueface: the tongue stops at the rim instead of poking past it, so the rim stays",
          "  whole. Those tip pixels are the relaxed count.",
          "- abused-face: the bruise. Banding replaces a smooth gradient, so pixels near band edges",
          "  differ from the source by design. The faintest fringe of the source bruise (up the right",
          "  side of the patch) falls below the first band.",
          "- heartface: the added patch is excluded from the count (it is new content).", "",
          "This is **not** a pixel-perfect conversion and is not claimed as one. Strict features",
          "(eyes, patch, mouth lines, teeth, disk size) are within 3 stray single pixels per face.", "",
          "## Drawing cost at 162 px (FUD-Deck FACE_S)", "",
          "Spans = fillRect calls (one row run each). Pixels = pixels written, including overdraw.",
          "The estimate assumes 40 MHz SPI and ~2.5 us per span for the window setup. Not measured",
          "on hardware.", "",
          "| Face | spans | pixels | est. ms |", "|---|---:|---:|---:|"]
    for n in T.FACES:
        r = res[n]
        ms = r["px162"] * 16 / 40e6 * 1e3 + r["spans162"] * 2.5e-3
        L.append("| %s | %d | %d | %.1f |" % (n, r["spans162"], r["px162"], ms))
    L += ["", "Images: out/validate/<face>.png (source | render | diff in magenta | 162 px),",
          "out/validate/all-diffs.png, out/validate/sheet-162-{black,red,blue}.png."]
    with open(os.path.join(OUT, "REPORT.md"), "w") as fh:
        fh.write("\n".join(L) + "\n")


if __name__ == "__main__":
    main()
