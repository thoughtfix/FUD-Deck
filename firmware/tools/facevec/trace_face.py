#!/usr/bin/env python3
"""Trace ONE face PNG into flat-color vectors. Nothing else: no face table, no sharing.

Usage:
  trace_face.py face.png [more.png ...] [-o OUTDIR] [--bands N] [--tol T]

Writes OUTDIR/<name>.svg (transparent, 135 x 135 box) and OUTDIR/<name>.json (disk, colors,
shapes in art units) for each PNG. Default OUTDIR: out/single/.

Input: a FUD-style face, 135 x 135 (other sizes are scaled): a black-rimmed round disk with
flat-color features on it. Model: a black disk (outer rim edge) and a base-colored disk
(inner rim edge), each a fitted ellipse, then flat-color polygon shapes painted in order with
even-odd fill. Coordinates are quarter pixels ("art units", 540 span the 135 px box).

trace_faces.py is the wrapper that traces a whole folder and builds facevec_data.h.
"""
import argparse, json, os, sys
import numpy as np
from PIL import Image
from scipy import ndimage as ndi
from skimage import measure

ROOT = os.path.dirname(os.path.abspath(__file__))              # firmware/tools/facevec
N = 135              # source size
U = 4                # supersample factor for classification and contour tracing
K = 4.0              # art units per source pixel (quarter pixels)
ART = int(N * K)     # art units across the face box

# Output palette (RGB888). One yellow for every face. RGB565 is derived from these.
PALETTE = {
    "black":  (0, 0, 0),
    "yellow": (252, 208, 4),
    "red":    (248, 4, 4),       # redface disk
    "white":  (252, 252, 252),
    "heart":  (244, 4, 4),
    "pink":   (250, 58, 132),
}
# Paint order of feature layers. Each layer may spill under any later layer.
ORDER = ["bruise1", "bruise2", "bruise3", "black", "grey", "white", "heart", "pink"]
BRUISE_BANDS = 3     # what --bands does for abused-face
# Features allowed to drift from the source to save bytes. Eyes, patch, mouth lines, teeth
# and the disk are traced tightly; these get one loose simplification pass.
RELAXED = {"pink", "heart", "bruise1", "bruise2", "bruise3"}


def rgb565(c):
    r, g, b = c
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def from565(v):
    r, g, b = (v >> 11) & 31, (v >> 5) & 63, v & 31
    return ((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2))


def load(path):
    """A face PNG as float RGBA. 135 x 135, like FUD's art; other sizes are scaled to it."""
    im = Image.open(path).convert("RGBA")
    if im.size != (N, N):
        im = im.resize((N, N), Image.LANCZOS)
    return np.asarray(im).astype(np.float64)


# ---------------------------------------------------------------- disk fitting

def fit_ellipse(pts):
    """Least-squares axis-aligned ellipse through points (x, y) with outlier rejection.
    Several source faces are squashed by up to 2.4 px, so a circle is not enough."""
    x, y = pts[:, 0], pts[:, 1]
    keep = np.ones(len(pts), bool)
    for _ in range(6):
        M = np.c_[x * x, y * y, x, y][keep]
        (A, B, C, D), *_ = np.linalg.lstsq(M, np.ones(keep.sum()), rcond=None)
        cx, cy = -C / (2 * A), -D / (2 * B)
        F = 1 + C * C / (4 * A) + D * D / (4 * B)
        rx, ry = np.sqrt(F / A), np.sqrt(F / B)
        res = np.abs(np.hypot((x - cx) / rx, (y - cy) / ry) - 1) * (rx + ry) / 2
        keep = res < max(0.35, 2.5 * np.median(res[keep]))
    return cx, cy, rx, ry, float(np.median(res[keep])), int(keep.sum())


def in_ellipse(xx, yy, cx, cy, rx, ry, grow=0.0):
    return ((xx - cx) / (rx + grow)) ** 2 + ((yy - cy) / (ry + grow)) ** 2 < 1


def fit_disk(a):
    """Outer and inner edge of the black rim, from rays cast out of the face center."""
    al = a[..., 3] / 255
    blk = al * (1 - a[..., :3].max(-1) / 255)         # 1 = black, 0 = yellow/white/clear
    yy, xx = np.mgrid[0:N, 0:N] + 0.5
    A = al.sum()
    c0 = ((al * xx).sum() / A, (al * yy).sum() / A)
    rs = np.arange(48, 72, 0.05)
    outer, inner = [], []
    for th in np.linspace(0, 2 * np.pi, 720, endpoint=False):
        px = c0[0] + rs * np.cos(th)
        py = c0[1] + rs * np.sin(th)
        v = ndi.map_coordinates(blk, [py - 0.5, px - 0.5], order=1, mode="constant")
        on = v >= 0.5
        if not on.any():
            continue
        io = np.nonzero(on)[0][-1]                    # last dark sample = outer rim edge
        if io + 1 < len(rs):
            t = (v[io] - 0.5) / max(1e-6, v[io] - v[io + 1])
            r = rs[io] + t * 0.05
            outer.append((c0[0] + r * np.cos(th), c0[1] + r * np.sin(th)))
        # inner edge: walk inward from the outer edge while dark
        j = io
        while j > 0 and on[j - 1]:
            j -= 1
        if j > 0:
            t = (0.5 - v[j - 1]) / max(1e-6, v[j] - v[j - 1])
            r = rs[j - 1] + t * 0.05
            inner.append((c0[0] + r * np.cos(th), c0[1] + r * np.sin(th)))
    o = fit_ellipse(np.array(outer))
    i = fit_ellipse(np.array(inner))
    return o, i


# ---------------------------------------------------------------- color classification

def flat_colors(rgb, mask):
    """Mean colors of flat (low local contrast) regions, merged into clusters."""
    mx = ndi.maximum_filter(rgb, size=(3, 3, 1))
    mn = ndi.minimum_filter(rgb, size=(3, 3, 1))
    flat = ((mx - mn).max(-1) < 24) & mask
    q = rgb[flat]
    groups = []
    for c in q[np.lexsort(q.T)]:
        for g in groups:
            if np.abs(g[0] / g[1] - c).max() <= 28:
                g[0] += c; g[1] += 1; break
        else:
            groups.append([c.copy(), 1])
    return [(g[0] / g[1], g[1]) for g in groups if g[1] >= 12]


def mixture_labels(img, pal):
    """Each pixel is modeled as a blend of two palette colors. Returns the nearer label of the
    best pair, the blend fraction toward the other color, and the fit error."""
    P = np.array(pal, float)
    flat = img.reshape(-1, 3)
    best_err = np.full(len(flat), np.inf)
    lab = np.zeros(len(flat), int)
    frac = np.zeros(len(flat))
    for i in range(len(P)):
        d = np.linalg.norm(flat - P[i], axis=1)
        m = d < best_err
        best_err[m] = d[m]; lab[m] = i; frac[m] = 0
        for j in range(i + 1, len(P)):
            e = P[j] - P[i]
            t = np.clip(((flat - P[i]) @ e) / (e @ e), 0, 1)
            d = np.linalg.norm(flat - (P[i] + t[:, None] * e), axis=1)
            m = d < best_err
            best_err[m] = d[m]
            lab[m] = np.where(t[m] < 0.5, i, j)
            frac[m] = np.where(t[m] < 0.5, t[m], 1 - t[m])
    sh = img.shape[:2]
    return lab.reshape(sh), frac.reshape(sh), best_err.reshape(sh)


def face_palette(a, disk_mask, bands=0):
    """Roles and measured colors for one face. Extra colors (bruise, grey) come from pixels the
    basic palette cannot explain."""
    rgb = a[..., :3]
    flats = flat_colors(rgb, disk_mask)
    ref = {"black": (0, 0, 0), "white": (252, 252, 252), "yellow": (251, 205, 4),
           "red": (249, 4, 4), "pink": (251, 58, 132)}
    roles = {}
    for c, n in sorted(flats, key=lambda t: -t[1]):
        role = min(ref, key=lambda k: np.linalg.norm(np.array(ref[k]) - c))
        if np.linalg.norm(np.array(ref[role]) - c) > 60:
            continue
        if role not in roles:
            roles[role] = c
    roles.setdefault("black", np.zeros(3))      # every face has a black rim, however thin
    if "yellow" in roles:
        base = "yellow"
    else:
        base = "red"
    if base == "yellow" and "red" in roles:      # red on a yellow face is the heart
        roles["heart"] = roles.pop("red")
    # unexplained pixels > extra colors
    names = list(roles)
    lab, frac, err = mixture_labels(rgb, [roles[k] for k in names])
    # Edge fringe is also "unexplained" but thin. A real extra color forms a solid patch.
    bad = ndi.binary_opening((err > 40) & disk_mask, np.ones((3, 3)))
    if bad.sum() > 30:
        q = rgb[bad]
        if bands:
            # a shaded extra color (abused-face's bruise): order unexplained pixels by
            # darkness and cut into bands
            yel = roles["yellow"]
            dist = np.linalg.norm(q - yel, axis=1)
            edges = np.quantile(dist, np.linspace(0, 1, bands + 1))
            for b in range(bands):
                sel = (dist >= edges[b]) & (dist <= edges[b + 1])
                roles["bruise%d" % (b + 1)] = np.median(q[sel], axis=0)
        else:
            roles["grey"] = np.median(q, axis=0)
    return base, roles, int(bad.sum())


# ---------------------------------------------------------------- tracing

def upsample(a):
    im = Image.fromarray(np.clip(a[..., :3], 0, 255).astype(np.uint8))
    return np.asarray(im.resize((N * U, N * U), Image.BICUBIC)).astype(np.float64)


def disk_mask_hi(cx, cy, rx, ry):
    yy, xx = (np.mgrid[0:N * U, 0:N * U] + 0.5) / U
    return in_ellipse(xx, yy, cx, cy, rx, ry)


def trace_mask(mask, tol):
    """Subpixel contours of a supersampled mask, simplified. Returns list of (n,2) arrays of
    (x, y) in source pixels."""
    f = ndi.gaussian_filter(np.pad(mask.astype(float), 2), 1.4)
    out = []
    for c in measure.find_contours(f, 0.5):
        c = measure.approximate_polygon(c, tol * U)
        if len(c) > 1 and np.allclose(c[0], c[-1]):
            c = c[:-1]
        if len(c) < 3:
            continue
        xy = np.c_[(c[:, 1] - 2 + 0.5) / U, (c[:, 0] - 2 + 0.5) / U]
        area = 0.5 * abs(np.dot(xy[:, 0], np.roll(xy[:, 1], 1)) - np.dot(xy[:, 1], np.roll(xy[:, 0], 1)))
        if area < 0.8:
            continue
        out.append(xy)
    return out


def quantize(contours):
    q = []
    for c in contours:
        v = np.clip(np.rint(c * K), 0, ART).astype(int)
        # drop repeated points after rounding
        keep = np.r_[True, np.any(v[1:] != v[:-1], axis=1)]
        v = v[keep]
        if len(v) > 1 and (v[0] == v[-1]).all():
            v = v[:-1]
        if len(v) < 3:
            continue
        # split steps that do not fit an int8 (edges longer than ~31 px)
        out = [v[0]]
        for i in range(1, len(v) + 1):
            a, b = out[-1], v[i % len(v)]
            n = int(np.ceil(np.abs(b - a).max() / 127.0))
            for k in range(1, n):
                out.append(np.rint(a + (b - a) * k / n).astype(int))
            if i < len(v):
                out.append(b)
        q.append(np.array(out))
    return q


BRUISE_CUTS = (0.16, 0.40, 0.66)   # band edges along yellow > darkest bruise


def band_bruise(hi, lab_hi, names, roles):
    """Replace the bruise's per-pixel labels with smooth concentric bands. Intensity = how far a
    pixel has moved from yellow toward the darkest bruise color, blurred a little so band edges
    follow the gradient instead of pixel noise. Band colors become the median source color in
    each band. Updates roles in place, returns new labels."""
    Y = np.array(roles["yellow"])
    soft = np.isin(lab_hi, [names.index(r) for r in names if r == "yellow" or r.startswith("bruise")])
    # only around the real bruise, so faint yellow variation elsewhere stays yellow
    bz = np.isin(lab_hi, [names.index(r) for r in names if r.startswith("bruise")])
    bz = ndi.binary_opening(bz, np.ones((U, U)))
    bz = ndi.distance_transform_edt(~bz) <= 2 * U
    q = hi[lab_hi == names.index("bruise%d" % BRUISE_BANDS)]
    D = q[np.argsort(np.linalg.norm(q - Y, axis=1))[int(0.95 * (len(q) - 1))]]
    e = D - Y
    I = np.clip(((hi - Y) @ e) / (e @ e), 0, 1)
    I = np.where(soft, I, 1.0)                      # black and white count as "under" the bruise
    Ib = ndi.gaussian_filter(I, U)
    lab = lab_hi.copy()
    yi = names.index("yellow")
    lab[soft] = yi
    for b, t in enumerate(BRUISE_CUTS):
        role = "bruise%d" % (b + 1)
        hi_t = BRUISE_CUTS[b + 1] if b + 1 < len(BRUISE_CUTS) else 2
        band = soft & bz & (Ib >= t) & (Ib < hi_t)
        lab[band] = names.index(role)
        if band.any():
            roles[role] = np.median(hi[band], axis=0)
    return lab


def layer_regions(lab_hi, names, base, inner_hi, outer_hi, outside_ok):
    """Masks to paint per feature layer. A layer spills into pixels that a later layer covers
    (and, for black, into the rim) so its outline stays simple."""
    present = [r for r in ORDER if r in names]
    regions = {}
    for k, role in enumerate(present):
        # Own pixels may sit on the rim. Past the rim only real art counts (the tongue tip and
        # its outline poke out of the disk), and only where it joins the face.
        lab = lab_hi == names.index(role)
        # Black on the rim itself is already drawn by the rim ellipse, so for black the rim is
        # don't-care (spill allowed, nothing required). Without this every face carries a
        # second copy of the rim as a ring polygon.
        # Relaxed features (tongue, heart, bruise) stay inside the rim: the rim stays whole and
        # the tongue ends a little shorter than in the source.
        own = lab & (inner_hi if role == "black" or role in RELAXED else outer_hi)
        if role not in ("white", "yellow") and role not in RELAXED:
            ext = lab & outside_ok & ~outer_hi
            ext &= ndi.distance_transform_edt(~own) <= 6 * U
            own |= ext
        later = np.zeros_like(own)
        for r2 in present[k + 1:]:
            later |= lab_hi == names.index(r2)
        later &= inner_hi
        if role == "black":
            later |= outer_hi & ~inner_hi
        if not own.any():
            continue
        near = ndi.distance_transform_edt(~own) <= 3 * U
        reg = own | (later & near)
        filled = ndi.binary_fill_holes(reg)
        holes, nh = ndi.label(filled & ~reg)
        if nh:
            ok = ndi.sum(~later[...], holes, range(1, nh + 1)) == 0  # hole made only of later px
            for h in np.nonzero(ok)[0] + 1:
                reg |= holes == h
        regions[role] = reg
    return present, regions


def components(mask):
    lab, n = ndi.label(mask, structure=np.ones((3, 3)))
    sl = ndi.find_objects(lab)
    comps = []
    for i in range(n):
        m = lab == (i + 1)
        if m.sum() < (U * U):          # under 1 source pixel
            continue
        comps.append(m)
    return comps


# ---------------------------------------------------------------- rasterizer (same rule as C++)

def raster_contours(contours, size, dx=0, dy=0):
    """Even-odd fill of art-unit contours into a size x size bool image, sampling pixel centers.
    Mirrors facevec.h exactly."""
    s = size / float(ART)
    img = np.zeros((size, size), bool)
    edges = []
    for c in contours:
        for i in range(len(c)):
            x0, y0 = c[i]; x1, y1 = c[(i + 1) % len(c)]
            edges.append(((x0 + dx) * s, (y0 + dy) * s, (x1 + dx) * s, (y1 + dy) * s))
    if not edges:
        return img
    E = np.array(edges, float)
    ymin = max(0, int(np.floor(E[:, [1, 3]].min())))
    ymax = min(size - 1, int(np.ceil(E[:, [1, 3]].max())))
    for py in range(ymin, ymax + 1):
        yc = py + 0.5
        a, b = E[:, 1], E[:, 3]
        hit = ((a <= yc) & (b > yc)) | ((b <= yc) & (a > yc))
        if not hit.any():
            continue
        e = E[hit]
        xs = np.sort(e[:, 0] + (yc - e[:, 1]) * (e[:, 2] - e[:, 0]) / (e[:, 3] - e[:, 1]))
        for xa, xb in zip(xs[0::2], xs[1::2]):
            x0 = max(0, int(np.ceil(xa - 0.5)))
            x1 = min(size, int(np.ceil(xb - 0.5)))
            if x1 > x0:
                img[py, x0:x1] = True
    return img


# ---------------------------------------------------------------- sharing

def best_shift(contours, target, search=4):
    """Translation (art units) that best lays shape over a target mask at 1x. Returns
    (dx, dy, xor_pixels, hausdorff_px)."""
    best = None
    tdist_in = ndi.distance_transform_edt(~target)
    for dy in range(-search, search + 1):
        for dx in range(-search, search + 1):
            m = raster_contours(contours, N, dx, dy)
            x = int((m ^ target).sum())
            if best is None or x < best[2]:
                best = (dx, dy, x, m)
    dx, dy, x, m = best
    d1 = ndi.distance_transform_edt(~m)
    haus = max(d1[target].max() if target.any() else 0, tdist_in[m].max() if m.any() else 0)
    return dx, dy, x, float(haus)


# ---------------------------------------------------------------- one face

def trace(path, name=None, tol0=0.35, relaxed_tol=0.8, bands=0, verbose=True):
    """Trace one PNG. Returns the face: disk, base color role, measured roles, shapes.
    Each shape: role, contours (art units), mask1 (source-px coverage mask, for sharing)."""
    name = name or os.path.splitext(os.path.basename(path))[0]
    a = load(path)
    (ocx, ocy, orx, ory, ores, _), (icx, icy, irx, iry, ires, _) = fit_disk(a)
    # one center for both ellipses: they agree within a fraction of a pixel on this art
    cx, cy = (ocx + icx) / 2, (ocy + icy) / 2
    yy, xx = np.mgrid[0:N, 0:N] + 0.5
    inner1 = in_ellipse(xx, yy, cx, cy, irx, iry, -1.0)
    base, roles, nbad = face_palette(a, inner1, bands)
    names = list(roles)
    hi = upsample(a)
    lab_hi, frac_hi, err_hi = mixture_labels(hi, [roles[k] for k in names])
    if "bruise1" in roles:
        lab_hi = band_bruise(hi, lab_hi, names, roles)
    inner_hi = disk_mask_hi(cx, cy, irx, iry)
    outer_hi = disk_mask_hi(cx, cy, orx, ory)
    al_hi = np.asarray(Image.fromarray(a[..., 3].astype(np.uint8)).resize((N * U, N * U), Image.BICUBIC))
    # Real art past the rim is opaque and either dark or strongly colored. The halo is pale grey.
    outside_ok = (al_hi >= 128) & ((hi.max(-1) < 90) | (hi.max(-1) - hi.min(-1) > 90)) & (err_hi < 70)
    present, regions = layer_regions(lab_hi, names, base, inner_hi, outer_hi, outside_ok)
    shapes = []
    for role in present:
        if role not in regions:
            continue
        rim_band = outer_hi & ~disk_mask_hi(cx, cy, irx - 2, iry - 2)
        for comp in components(regions[role]):
            # A black speck lying wholly against the rim is the source rim wobbling by a
            # pixel, not a feature. Dropping it costs a stray pixel and saves a shape.
            if role == "black" and not (comp & ~rim_band).any() and comp.sum() < 60 * U * U:
                continue
            cov = comp.reshape(N, U, N, U).mean(axis=(1, 3))
            sure_in, sure_out = cov >= 0.9, cov <= 0.1
            # Tighten the tolerance until no clear-cut pixel comes out wrong (thin tooth
            # gaps need it, big smooth shapes do not).
            tols = (relaxed_tol,) if role in RELAXED else (tol0, 0.25, 0.15, 0.08)
            for tol in tols:
                cs = quantize(trace_mask(comp, tol))
                m = raster_contours(cs, N)
                bad = int((m & sure_out).sum() + (~m & sure_in).sum())
                if bad <= 1:
                    break
            if cs:
                shapes.append({"role": role, "contours": [c.tolist() for c in cs],
                               "mask1": cov >= 0.5, "tol": tol, "bad": bad})
    face = {
        "disk": {"cx": cx, "cy": cy, "rox": orx, "roy": ory, "rix": irx, "riy": iry,
                 "fit_res": [ores, ires], "center_gap": float(np.hypot(ocx - icx, ocy - icy))},
        "base": base, "roles": {k: [float(x) for x in v] for k, v in roles.items()},
        "unexplained_px": nbad, "shapes": shapes,
    }
    nv = sum(len(c) for s in shapes for c in s["contours"])
    if verbose:
        print(f"{name:18s} base={base:6s} R={orx:.2f}x{ory:.2f}/{irx:.2f}x{iry:.2f} gap={face['disk']['center_gap']:.2f} "
              f"roles={','.join(names)} shapes={len(shapes)} verts={nv}")
    face["name"] = name
    return face


def color_of(face, role):
    if role == "yellow":
        return PALETTE["yellow"]
    if role in PALETTE:
        return PALETTE[role]
    c = np.array(face["roles"][role])
    return tuple(int(round(v)) for v in c)


def fmt(v):
    s = ("%.2f" % v).rstrip("0").rstrip(".")
    return s


def svg(face):
    """Standalone SVG of one traced face (its own shapes, no sharing)."""
    d = face["disk"]
    def hexc(c):
        r, g, b = from565(rgb565(c))
        return "#%02x%02x%02x" % (r, g, b)
    E = '<ellipse cx="%s" cy="%s" rx="%s" ry="%s" fill="%s"/>'
    parts = [E % (fmt(round(d["cx"] * 8) / 8), fmt(round(d["cy"] * 8) / 8), fmt(round(d["rox"] * 8) / 8),
                  fmt(round(d["roy"] * 8) / 8), "#000"),
             E % (fmt(round(d["cx"] * 8) / 8), fmt(round(d["cy"] * 8) / 8), fmt(round(d["rix"] * 8) / 8),
                  fmt(round(d["riy"] * 8) / 8), hexc(color_of(face, face["base"])))]
    for s in face["shapes"]:
        dx, dy = s.get("shift", [0, 0])
        dpath = ""
        for c in s["contours"]:
            dpath += "M" + " ".join("%s %s" % (fmt((x + dx) / K), fmt((y + dy) / K)) for x, y in c) + "Z"
        parts.append('<path fill="%s" fill-rule="evenodd" d="%s"/>' % (hexc(color_of(face, s["role"])), dpath))
    return ('<?xml version="1.0" encoding="UTF-8"?>\n'
            '<svg xmlns="http://www.w3.org/2000/svg" width="135" height="135" viewBox="0 0 135 135">\n'
            '<!-- %s: traced by tools/facevec/trace_face.py. Transparent background. -->\n%s\n</svg>\n'
            % (face["name"], "".join(parts).replace("/>", "/>\n")))


def to_json(face):
    """The face without its masks (numpy), for files."""
    return {"name": face["name"], "disk": face["disk"], "base": face["base"], "roles": face["roles"],
            "unexplained_px": face["unexplained_px"],
            "shapes": [{"role": s["role"], "contours": s["contours"]} for s in face["shapes"]]}


def main():
    ap = argparse.ArgumentParser(description="Trace face PNGs into vectors (SVG + JSON).")
    ap.add_argument("png", nargs="+")
    ap.add_argument("-o", "--out", default=os.path.join(ROOT, "out", "single"))
    ap.add_argument("--bands", type=int, default=0,
                    help="split one shaded extra color into N flat bands (abused-face's bruise: 3)")
    ap.add_argument("--tol", type=float, default=0.35, help="simplify tolerance, source px")
    ap.add_argument("--relaxed-tol", type=float, default=0.8, help="tolerance for RELAXED features")
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    for p in args.png:
        f = trace(p, tol0=args.tol, relaxed_tol=args.relaxed_tol, bands=args.bands)
        with open(os.path.join(args.out, f["name"] + ".svg"), "w") as fh:
            fh.write(svg(f))
        with open(os.path.join(args.out, f["name"] + ".json"), "w") as fh:
            json.dump(to_json(f), fh, indent=1, default=float)
    print("wrote", args.out)


if __name__ == "__main__":
    main()
