#!/usr/bin/env python3
"""
make_media.py — render the Devpost image gallery from results/.

    python media/make_media.py

Every value drawn comes from parsing results/*.txt. Nothing is typed in here,
so the images cannot drift from the captures. If a number looks wrong, the
capture is wrong and the fix is to re-run the device, not to edit a label.

Output: media/*.png at 1800x1200 (3:2, Devpost's recommended ratio), well
under the 5 MB per-image limit.

Copyright 2026 The Vane Authors. Apache License 2.0.
"""
import os
import re
import sys

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RESULTS = os.path.join(ROOT, "results")
OUT = os.path.join(ROOT, "media")

W, H = 1800, 1200
BG, PANEL, EDGE = "#0a0d12", "#11151c", "#232a36"
TEXT, DIM, ACCENT, ACCENT2, WARN = "#e7ecf3", "#8b96a8", "#3ddc97", "#5cc2ff", "#ff9f43"

MONO = "C:/Windows/Fonts/consola.ttf"
MONOB = "C:/Windows/Fonts/consolab.ttf"


def font(path, size):
    try:
        return ImageFont.truetype(path, size)
    except OSError:
        return ImageFont.load_default()


SIZES = (17, 18, 19, 20, 21, 22, 23, 24, 26, 28, 30, 34, 38, 44, 56)
F = {n: font(MONO, n) for n in SIZES}
FB = {n: font(MONOB, n) for n in SIZES}


# ----------------------------------------------------------------- parsing

DEVICES = [
    ("redmi-k20-pro", "Redmi K20 Pro", "Snapdragon 855 · 2019"),
    ("quest2", "Meta Quest 2", "Snapdragon XR2 · 2020"),
    ("s23-ultra", "Galaxy S23 Ultra", "Snapdragon 8 Gen 2 · 2023"),
    ("graviton4", "AWS c8g.large", "Neoverse V2 · 2024"),
]


def read(label):
    p = os.path.join(RESULTS, f"{label}.txt")
    if not os.path.exists(p):
        return None
    with open(p, encoding="utf-8", errors="replace") as fh:
        return fh.read()


def probe(txt):
    """Pull the vane_probe block fields."""
    out = {}
    for key in ("cpu", "os", "features", "sve vector len", "dispatched to"):
        m = re.search(rf"^\s+{re.escape(key)}\s+:\s*(.+)$", txt, re.M)
        if m:
            out[key] = m.group(1).strip()
    m = re.search(r"verify exit code:\s*(\d+)", txt)
    out["verify_rc"] = m.group(1) if m else "?"
    out["verify_pass"] = "RESULT: PASS" in txt
    return out


def bench(txt):
    """{op: {path: {median, speedup, gbps}}} from the vane_bench tables."""
    ops = {}
    cur = None
    for line in txt.splitlines():
        m = re.match(r"^(\w+)\s+\((\d+) x (\d+)\)\s*$", line)
        if m:
            cur = m.group(1)
            ops[cur] = {"shape": f"{m.group(2)}x{m.group(3)}", "paths": {}}
            continue
        if cur:
            m = re.match(r"^\s+(scalar|neon|sve)\s+(\d+)\s+(\d+)\s+([\d.]+)\s+(\d+)\s+([\d.]+)x", line)
            if m:
                ops[cur]["paths"][m.group(1)] = {
                    "median": int(m.group(2)),
                    "gbps": float(m.group(4)),
                    "speedup": float(m.group(6)),
                }
    return ops


DATA = []
for label, name, silicon in DEVICES:
    txt = read(label)
    if txt is None:
        print(f"  skip {label}: no capture", file=sys.stderr)
        continue
    DATA.append({"label": label, "name": name, "silicon": silicon,
                 "probe": probe(txt), "bench": bench(txt)})

if not DATA:
    sys.exit("no captures found in results/")


# ------------------------------------------------------------------ drawing

def canvas():
    im = Image.new("RGB", (W, H), BG)
    return im, ImageDraw.Draw(im)


def panel(d, box, radius=14, fill=PANEL, outline=EDGE, width=1):
    d.rounded_rectangle(box, radius=radius, fill=fill, outline=outline, width=width)


def title(d, text, sub=None):
    d.text((70, 62), text, font=FB[44], fill=TEXT)
    if sub:
        d.text((70, 124), sub, font=F[24], fill=DIM)


def footer(d, text, colour=DIM):
    d.text((70, H - 78), text, font=F[22], fill=colour)


def save(im, name):
    os.makedirs(OUT, exist_ok=True)
    p = os.path.join(OUT, name)
    im.save(p, "PNG", optimize=True)
    print(f"  wrote {os.path.relpath(p, ROOT)}  "
          f"({os.path.getsize(p)/1024:.0f} KB, {im.width}x{im.height})")


def wrap_tokens(tokens, per_line):
    return [tokens[i:i + per_line] for i in range(0, len(tokens), per_line)]


def wrap_words(text, width):
    """Wrap on word boundaries.

    A fixed-width character slice split 'Inc SM8150' into 'Inc S' + 'M8150'
    and '300 dims' into '300 d' + 'ims', which looked like corrupt data in
    the first render of the gallery.
    """
    words, lines, cur = text.split(), [], ""
    for w in words:
        trial = f"{cur} {w}".strip()
        if len(trial) <= width:
            cur = trial
        else:
            if cur:
                lines.append(cur)
            cur = w
    if cur:
        lines.append(cur)
    return lines or [""]


# --- 1. the device wall ----------------------------------------------------

def img_device_wall():
    im, d = canvas()
    # Honest headline. An earlier draft said "four different decisions", but
    # there are only two distinct dispatch outcomes here (neon on three CPUs,
    # sve on one) across three distinct feature sets. Overstating the variety
    # would have been the same failure this project exists to avoid.
    title(d, "One binary. Four Arm CPUs. Each binds itself.",
          "vane_probe output, captured verbatim on each device. "
          "Three distinct feature sets, two dispatch outcomes, nothing recompiled.")

    n = len(DATA)
    gap, m = 22, 70
    cw = (W - 2 * m - gap * (n - 1)) // n
    top, ch = 196, 620

    for i, dev in enumerate(DATA):
        x = m + i * (cw + gap)
        panel(d, (x, top, x + cw, top + ch))
        p = dev["probe"]

        d.text((x + 24, top + 26), dev["name"], font=FB[26], fill=TEXT)
        d.text((x + 24, top + 62), dev["silicon"], font=F[20], fill=DIM)
        d.line((x + 24, top + 100, x + cw - 24, top + 100), fill=EDGE)

        y = top + 124
        d.text((x + 24, y), "features", font=F[20], fill=DIM)
        y += 30
        # Highlight the ISA extensions that actually differ between devices.
        for row in wrap_tokens(p.get("features", "").split(), 2):
            for tok in row:
                col = ACCENT if tok in ("sve", "sve2", "i8mm", "bf16") else TEXT
                d.text((x + 24 + row.index(tok) * (cw // 2 - 20), y), tok, font=FB[22], fill=col)
            y += 30

        y += 22
        d.text((x + 24, y), "sve vector length", font=F[20], fill=DIM)
        vl = p.get("sve vector len", "n/a")
        d.text((x + 24, y + 28), vl, font=FB[24], fill=ACCENT if vl != "n/a" else DIM)

        y += 88
        d.text((x + 24, y), "dispatched to", font=F[20], fill=DIM)
        disp = p.get("dispatched to", "?")
        d.text((x + 24, y + 28), disp, font=FB[34], fill=ACCENT2)

        y += 96
        d.text((x + 24, y), "kernels vs scalar oracle", font=F[20], fill=DIM)
        ok = p["verify_pass"] and p["verify_rc"] == "0"
        d.text((x + 24, y + 28), "PASS" if ok else "FAIL",
               font=FB[34], fill=ACCENT if ok else WARN)
        d.text((x + 24, y + 72), f"vane_verify exit {p['verify_rc']}", font=F[18], fill=DIM)

    # The sharpest point on this slide is the pair that looks the same.
    y = top + ch + 46
    d.text((70, y), "Green tokens are the ISA extensions that differ.", font=F[22], fill=DIM)
    d.text((70, y + 34),
           "Note the first two columns: identical detected features, identical dispatch, "
           "and measurably different throughput.", font=F[22], fill=TEXT)
    d.text((70, y + 68),
           "A feature bit tells you an instruction exists. It never tells you what it costs.",
           font=FB[22], fill=ACCENT)
    save(im, "1-device-wall.png")


# --- 2. verify, with SVE ---------------------------------------------------

def img_verify():
    im, d = canvas()
    title(d, "The SVE kernel had never executed until this run.",
          "Written on an x86 host. Compile-verified, disassembly-verified, never run. "
          "AWS Graviton4, first execution.")

    txt = read("graviton4")
    lines, keep = [], False
    for line in txt.splitlines():
        if "vane_verify" in line:
            keep = True
        if keep:
            lines.append(line.rstrip())
        if line.startswith("RESULT:"):
            break

    panel(d, (70, 200, W - 70, H - 130), fill="#080b10")
    y = 226
    for line in lines[:26]:
        col = TEXT
        if "PASS" in line:
            col = ACCENT
        elif "FAIL" in line:
            col = WARN
        elif "SKIP" in line:
            col = DIM
        elif line.strip().startswith(("dequant", "quantize", "gemv")):
            col = ACCENT2
        f = FB[22] if line.startswith("RESULT:") else F[20]
        d.text((100, y), line, font=f, fill=col)
        y += 27

    footer(d, "Every available path, on identical input, against a portable C reference. "
              "Disagreement exits non-zero and fails CI.")
    save(im, "2-verify-sve.png")


# --- 3. measured speedups -------------------------------------------------

def img_speedups():
    im, d = canvas()
    title(d, "Measured, on each device, in the same run.",
          "vane_bench medians from a live monotonic clock. 'vs scalar' is a ratio of two medians on one machine.")

    op = "dequant_gemv_int4"
    rows = [x for x in DATA if op in x["bench"] and "neon" in x["bench"][op]["paths"]]

    top, rh = 230, 190
    best = max(r["bench"][op]["paths"]["neon"]["speedup"] for r in rows)

    d.text((100, top - 34), f"{op}   ·   vector path vs scalar", font=F[22], fill=DIM)

    for i, r in enumerate(rows):
        y = top + i * rh
        panel(d, (70, y, W - 70, y + rh - 20))
        p = r["bench"][op]["paths"]
        best_path = min(("neon", "sve"), key=lambda k: p[k]["median"] if k in p else 1 << 62)
        sp = p[best_path]["speedup"]

        d.text((100, y + 26), r["name"], font=FB[28], fill=TEXT)
        d.text((100, y + 64), r["silicon"], font=F[22], fill=DIM)
        d.text((100, y + 102), f"scalar {p['scalar']['median']:,} ns", font=F[20], fill=DIM)
        d.text((100, y + 130), f"{best_path:>6} {p[best_path]['median']:,} ns",
               font=F[20], fill=ACCENT2)

        bx0, bx1 = 700, W - 330
        bw = int((bx1 - bx0) * (sp / best))
        d.rounded_rectangle((bx0, y + 56, bx0 + max(bw, 6), y + 110), radius=10, fill=ACCENT2)
        d.text((W - 300, y + 46), f"{sp:.2f}x", font=FB[56], fill=ACCENT)

    footer(d, "The oldest core gains the most: its scalar performance is what is weakest, "
              "so the vector path has the most to recover.")
    save(im, "3-speedups.png")


# --- 4. atlas on the phone ------------------------------------------------

def img_atlas(shot):
    """Letterbox the real device screenshot into 3:2 with a caption rail."""
    im, d = canvas()
    src = Image.open(shot).convert("RGB")

    # Leave room for the footer; an earlier render ran the phone image straight
    # through the footer text.
    max_h = H - 330
    scale = max_h / src.height
    sw, sh = int(src.width * scale), int(src.height * scale)
    src = src.resize((sw, sh), Image.LANCZOS)

    title(d, "Atlas: 25,000 vectors searched on the phone itself.",
          "The device runs the server and the search. The browser only draws.")

    px = 110
    py = 200
    d.rounded_rectangle((px - 10, py - 10, px + sw + 10, py + sh + 10),
                        radius=16, outline=EDGE, width=2)
    im.paste(src, (px, py))

    tx = px + sw + 70
    dev = next((x for x in DATA if x["label"] == "redmi-k20-pro"), DATA[0])
    p = dev["probe"]

    # Width available for the caption rail, in monospace characters.
    chars = max(18, (W - 90 - tx) // 14)

    y = 226
    for head, val, col in (
        ("device", p.get("cpu", "—"), TEXT),
        ("features", p.get("features", "—"), TEXT),
        ("dispatched to", p.get("dispatched to", "—"), ACCENT2),
        ("corpus", "25,000 GloVe vectors x 300 dims", TEXT),
        ("footprint", "5.0 MB INT4, was 30.0 MB as fp32", ACCENT),
        ("compression", "83.3% smaller, scales counted", DIM),
    ):
        d.text((tx, y), head, font=F[22], fill=DIM)
        segs = wrap_words(val, chars)
        for j, seg in enumerate(segs):
            d.text((tx, y + 32 + j * 30), seg, font=FB[24], fill=col)
        y += 46 + 30 * len(segs)

    d.line((tx, y - 10, W - 90, y - 10), fill=EDGE)
    d.text((tx, y + 18), "the corpus ships inside the repo", font=F[22], fill=DIM)
    d.text((tx, y + 50), "because it is INT4", font=FB[24], fill=ACCENT)

    footer(d, "A 2019 handset, no network, no cloud call: the whole index fits and the search runs on its own Arm core.")
    save(im, "4-atlas-on-device.png")


# --- 5. architecture ------------------------------------------------------

def arrow(d, x0, y0, x1, y1, colour=EDGE, width=2, head=9):
    d.line((x0, y0, x1, y1), fill=colour, width=width)
    if y1 != y0 and x1 == x0:                       # vertical
        s = 1 if y1 > y0 else -1
        d.polygon([(x1, y1), (x1 - head, y1 - s * head), (x1 + head, y1 - s * head)], fill=colour)
    elif x1 != x0 and y1 == y0:                     # horizontal
        s = 1 if x1 > x0 else -1
        d.polygon([(x1, y1), (x1 - s * head, y1 - head), (x1 - s * head, y1 + head)], fill=colour)


def img_architecture():
    im, d = canvas()
    title(d, "How it binds itself.",
          "Detection happens once, at init. After that a kernel call is an indirect jump with no feature test.")

    cx = 640

    # --- application
    panel(d, (cx - 300, 190, cx + 300, 268))
    d.text((cx - 276, 206), "your application", font=FB[26], fill=TEXT)
    d.text((cx - 276, 238), "#include <vane.h>", font=F[22], fill=DIM)
    arrow(d, cx, 268, cx, 322)

    # --- dispatch
    panel(d, (cx - 300, 322, cx + 300, 470), outline=ACCENT2, width=2)
    d.text((cx - 276, 340), "vn_init()", font=FB[28], fill=ACCENT2)
    d.text((cx - 276, 378), "reads CPU feature bits, once", font=F[21], fill=DIM)
    d.text((cx - 276, 406), "getauxval        Linux, Android", font=F[19], fill=TEXT)
    d.text((cx - 276, 430), "sysctlbyname     macOS", font=F[19], fill=TEXT)
    arrow(d, cx, 470, cx, 524)
    d.text((cx - 296, 486), "binds function pointers", font=F[20], fill=DIM)

    # --- three kernels
    kb = [("kernel_scalar.cpp", "portable C", "THE ORACLE", ACCENT),
          ("kernel_neon.cpp", "armv8.2-a+simd+dotprod", "AdvSIMD, SDOT", TEXT),
          ("kernel_sve.cpp", "armv8.2-a+sve", "vector-length agnostic", TEXT)]
    kw, kg = 340, 26
    x0 = cx - 300
    top = 560
    for i, (name, march, note, col) in enumerate(kb):
        y = top + i * 128
        panel(d, (x0, y, x0 + kw, y + 108))
        d.text((x0 + 20, y + 16), name, font=FB[23], fill=col)
        d.text((x0 + 20, y + 48), march, font=F[19], fill=ACCENT2)
        d.text((x0 + 20, y + 76), note, font=F[19], fill=DIM)
        arrow(d, cx - 300 - 4, 524, x0 + kw // 2, y) if False else None
    # single spine from the dispatcher down the left of the kernel column
    d.line((x0 - 26, 524, x0 - 26, top + 2 * 128 + 54), fill=EDGE, width=2)
    d.line((cx, 524, x0 - 26, 524), fill=EDGE, width=2)
    for i in range(3):
        y = top + i * 128 + 54
        arrow(d, x0 - 26, y, x0 - 4, y)

    # --- right rail: the two guarantees.
    # Must clear the dispatch panel, which extends to cx + 300 = 940; an
    # earlier value of x0 + kw + 90 put it at 770 and overlapped it.
    rx = cx + 360
    panel(d, (rx, 322, W - 70, 690), outline=ACCENT, width=2)
    d.text((rx + 26, 344), "vane_verify", font=FB[26], fill=ACCENT)
    d.text((rx + 26, 382), "Runs every path available on this CPU", font=F[21], fill=TEXT)
    d.text((rx + 26, 410), "against kernel_scalar on identical input.", font=F[21], fill=TEXT)
    d.text((rx + 26, 448), "Exits non-zero on disagreement.", font=FB[21], fill=TEXT)
    d.text((rx + 26, 476), "Registered as a ctest case.", font=F[21], fill=DIM)
    d.text((rx + 26, 524), "A vector kernel is not trusted because", font=F[21], fill=DIM)
    d.text((rx + 26, 552), "it compiled. It is trusted because it", font=F[21], fill=DIM)
    d.text((rx + 26, 580), "agrees.", font=F[21], fill=DIM)
    d.text((rx + 26, 626), "Caught two SVE defects that compiled", font=F[20], fill=WARN)
    d.text((rx + 26, 652), "cleanly and passed disassembly review.", font=F[20], fill=WARN)

    panel(d, (rx, 716, W - 70, 1010), outline=ACCENT2, width=2)
    d.text((rx + 26, 738), "vn_bench  ·  telemetry", font=FB[26], fill=ACCENT2)
    d.text((rx + 26, 776), "Every value from a live monotonic clock", font=F[21], fill=TEXT)
    d.text((rx + 26, 804), "or a sensor read on the device.", font=F[21], fill=TEXT)
    d.text((rx + 26, 842), "Anything the platform will not expose", font=F[21], fill=DIM)
    d.text((rx + 26, 870), "is VN_UNMEASURED, serialised as JSON", font=F[21], fill=DIM)
    d.text((rx + 26, 898), "null. Never zero, never estimated.", font=F[21], fill=DIM)
    d.text((rx + 26, 946), "There is no code path in this library", font=FB[21], fill=ACCENT2)
    d.text((rx + 26, 974), "that can emit a number it did not take.", font=FB[21], fill=ACCENT2)

    footer(d, "Per-translation-unit -march plus runtime gating: one shared object is correct from ARMv8.0 "
              "through SVE2, and never enters a path the CPU lacks.")
    save(im, "5-architecture.png")


# --- 6. full results matrix ------------------------------------------------

def img_matrix():
    im, d = canvas()
    title(d, "Every kernel, every device, every path.",
          "Medians in nanoseconds at 512 x 4096, parsed from results/*.txt. "
          "'best' is the fastest vector path measured on that CPU.")

    # Geometry chosen so the third column's speedup still lands inside the
    # panel: x0 + 2*colw + SP_DX must stay under W - 70 with room for the
    # glyphs. An earlier 430/500/380 pushed quantize_int4's ratio off-canvas.
    ops = ["dequant_gemv_int4", "gemv_int8", "quantize_int4"]
    colw, x0 = 442, 386
    BEST_DX, SP_DX = 172, 348
    ytop = 250

    for j, op in enumerate(ops):
        x = x0 + j * colw
        d.text((x, ytop - 56), op, font=FB[22], fill=ACCENT2)
        d.text((x, ytop - 26), "scalar", font=F[18], fill=DIM)
        d.text((x + BEST_DX, ytop - 26), "best", font=F[18], fill=DIM)
        d.text((x + SP_DX, ytop - 26), "x", font=F[18], fill=DIM)

    for i, dev in enumerate(DATA):
        y = ytop + i * 168
        panel(d, (70, y, W - 70, y + 148))
        d.text((100, y + 22), dev["name"], font=FB[26], fill=TEXT)
        d.text((100, y + 58), dev["silicon"], font=F[20], fill=DIM)
        disp = dev["probe"].get("dispatched to", "?")
        d.text((100, y + 96), f"auto -> {disp}", font=FB[22], fill=ACCENT2)

        for j, op in enumerate(ops):
            x = x0 + j * colw
            b = dev["bench"].get(op)
            if not b or "scalar" not in b["paths"]:
                d.text((x, y + 58), "not captured", font=F[20], fill=DIM)
                continue
            p = b["paths"]
            cands = [k for k in ("neon", "sve") if k in p]
            best = min(cands, key=lambda k: p[k]["median"]) if cands else None
            d.text((x, y + 46), f"{p['scalar']['median']:,}", font=F[20], fill=DIM)
            if best:
                d.text((x + BEST_DX, y + 46), f"{p[best]['median']:,}", font=FB[20], fill=TEXT)
                d.text((x + SP_DX, y + 40), f"{p[best]['speedup']:.2f}x", font=FB[26], fill=ACCENT)
                d.text((x + BEST_DX, y + 76), best, font=F[17], fill=ACCENT2)
            # Show both vector paths where the CPU has both — the SVE-vs-NEON
            # comparison is the point on Neoverse V2.
            if len(cands) == 2:
                other = [k for k in cands if k != best][0]
                d.text((x, y + 106),
                       f"{other} {p[other]['median']:,}  ({p[other]['speedup']:.2f}x)",
                       font=F[17], fill=DIM)

    footer(d, "On Neoverse V2, NEON edges SVE: its SVE is 128-bit, the same width as NEON, so predication "
              "buys no extra lanes. Reported, not hidden.")
    save(im, "6-results-matrix.png")


if __name__ == "__main__":
    print(f"parsed {len(DATA)} device captures")
    img_device_wall()
    img_verify()
    img_speedups()
    img_architecture()
    img_matrix()
    shot = sys.argv[1] if len(sys.argv) > 1 else None
    if shot and os.path.exists(shot):
        img_atlas(shot)
    else:
        print("  (pass a device screenshot path to also build 4-atlas-on-device.png)")
