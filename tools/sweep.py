"""Render every parameter at both ends of its range and fail if any made no
difference.

**This is the only thing in the repo that catches a dead control.** A GLSL
uniform whose name does not match the C++ is silently ignored --
`glGetUniformLocation` returns -1 and `glUniform` on -1 is a documented no-op
-- so a slider can be stone dead while everything compiles, links, loads and
renders.

Each control is swept where it can act: a surface's own controls on that
surface, Roughness on metal where the lens's highlight is in view, the viewer
on Drapes (whose folds are deep enough for the viewer's place to show at
320x180; on real brick it moves the joints by about a pixel), Aim with the
projector off-centre (from the centre Pointed IS Lens Shift), Surface File
with a file the sweep writes, and File Holds with a colour file. An option
reads back as its index, and is set here by element name. A control whose
ends differ by less than `--floor` (mean 8-bit difference per channel) is
reported as barely alive: vectrix's lesson.

Every render is 320x180 (CI's raster), of twtest's look card.

Usage::

    tools/sweep.py [--build BUILD_DIR] [--verbose] [--jobs N] [--bare]

`--bare` drops every per-control context (keeping only the base), the
addendum's honesty check: what goes dead there is the list of controls that
only act with another one set, which AGENTS.md records.
"""

import argparse
import concurrent.futures
import pathlib
import re
import subprocess
import sys
import tempfile
import zlib

REPO = pathlib.Path(__file__).resolve().parent.parent

BASE = []

PAPER = ["Surface=Paper"]
BRICK = ["Surface=Brick"]
METAL = ["Surface=Corrugated"]
PLATE = ["Surface=Plate"]
DRAPES = ["Surface=Drapes"]
FILE = ["Surface=File"]

# Filled in by main(): a grey ramp and a colour picture, written to scratch.
HEIGHT_FILE = ""
PHOTO_FILE = ""

# name -> (low settings, high settings, context settings)
SWEEP = {
    "Surface": (["Surface=Flat"], ["Surface=Brick"], []),
    "Wall Height": (["Wall Height=0.3"], ["Wall Height=0.7"], BRICK),
    "Scale": (["Scale=0.3"], ["Scale=0.7"], BRICK),
    "Depth": (["Depth=0.3"], ["Depth=0.8"], DRAPES),
    "Seed": (["Seed=1"], ["Seed=50"], PAPER),
    "Crumple": (["Crumple=0.2"], ["Crumple=0.9"], PAPER),
    "Bond": (["Bond=Stretcher"], ["Bond=Flemish"], BRICK),
    "Mortar": (["Mortar=0.1"], ["Mortar=0.9"], BRICK),
    "Profile": (["Profile=Sine"], ["Profile=Box"], METAL),
    "Pitch": (["Pitch=0.3"], ["Pitch=0.7"], METAL),
    "Pattern": (["Pattern=Diamond"], ["Pattern=Crosshatch"], PLATE),
    "Pleats": (["Pleats=8"], ["Pleats=30"], DRAPES),
    "Sag": (["Sag=0.1"], ["Sag=0.9"], DRAPES),
    "Roughness": (["Roughness=0.1"], ["Roughness=0.9"], METAL),
    "Surface File": ([], ["@height"], FILE),
    "File Holds": (["File Holds=Height"], ["File Holds=Photo"], FILE + ["@photo"]),
    "Projector X": (["Projector X=0.4"], ["Projector X=0.6"], DRAPES),
    "Projector Y": (["Projector Y=0.4"], ["Projector Y=0.6"], DRAPES),
    "Throw": (["Throw=0.3"], ["Throw=0.6"], DRAPES),
    "Aim": (["Aim=Lens Shift"], ["Aim=Pointed"], DRAPES + ["Projector X=0.6"]),
    "Lumens": (["Lumens=0.3"], ["Lumens=0.7"], ["Ambient=0.5"]),
    "Black Level": (["Black Level=0"], ["Black Level=1"], []),
    "Ambient": (["Ambient=0"], ["Ambient=0.5"], []),
    "Exposure": (["Exposure=0.4"], ["Exposure=0.6"], []),
    "Viewer X": (["Viewer X=0.4"], ["Viewer X=0.6"], DRAPES),
    "Viewer Y": (["Viewer Y=0.4"], ["Viewer Y=0.6"], DRAPES),
    "Viewer Distance": (["Viewer Distance=0.1"], ["Viewer Distance=0.6"], DRAPES),
    "View": (["View=Picture"], ["View=Light"], []),
    "Mix": (["Mix=0"], ["Mix=1"], []),
}

# The About block: a text line and browser buttons, with no pixel to sweep.
SKIP = {"About", "Project page", "Source on GitHub", "Support the work", "User guide"}


def png_chunk(kind, body):
    return (len(body).to_bytes(4, "big") + kind + body
            + zlib.crc32(kind + body).to_bytes(4, "big"))


def write_png(path, width, height, channels, rows):
    """8-bit grey (1) or RGB (3); rows top first, each a bytes object."""
    raw = b"".join(b"\x00" + row for row in rows)
    colour = 0 if channels == 1 else 2
    data = (b"\x89PNG\r\n\x1a\n"
            + png_chunk(b"IHDR", width.to_bytes(4, "big") + height.to_bytes(4, "big") + bytes([8, colour, 0, 0, 0]))
            + png_chunk(b"IDAT", zlib.compress(raw))
            + png_chunk(b"IEND", b""))
    path.write_bytes(data)


def read_png(path):
    """Enough of PNG for twtest's own writer: 8-bit RGBA, filter 0 rows."""
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    pos, width, height, idat = 8, 0, 0, b""
    while pos < len(data):
        length = int.from_bytes(data[pos:pos + 4], "big")
        kind = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        if kind == b"IHDR":
            width = int.from_bytes(body[0:4], "big")
            height = int.from_bytes(body[4:8], "big")
        elif kind == b"IDAT":
            idat += body
        pos += 12 + length
    raw = zlib.decompress(idat)
    stride = width * 4
    out = bytearray()
    for y in range(height):
        start = y * (stride + 1)
        out += raw[start + 1:start + 1 + stride]
    return bytes(out)


def difference(a, b):
    if len(a) != len(b):
        return 255.0
    return sum(abs(x - y) for x, y in zip(a, b)) / len(a)


def render(twtest, out, settings):
    args = [str(twtest), "--out", str(out), "--size", "320x180"]
    for setting in settings:
        if setting == "@height":
            setting = f"Surface File={HEIGHT_FILE}"
        elif setting == "@photo":
            setting = f"Surface File={PHOTO_FILE}"
        args += ["--set", setting]
    result = subprocess.run(args, capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"twtest failed: {' '.join(args)}\n{result.stderr.strip()}")
    return read_png(out)


def parameters(twtest):
    result = subprocess.run([str(twtest), "--list"], capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"twtest --list failed: {result.stderr.strip()}")
    names = []
    for line in result.stdout.splitlines()[1:]:
        parts = re.split(r"\s{2,}", line.strip())
        if len(parts) >= 3 and parts[0].isdigit():
            names.append(parts[1].strip())
    return names


def sweep_one(twtest, scratch, name, bare):
    low, high, context = SWEEP[name]
    # A file is part of what is swept, not a context to drop.
    context = BASE + ([s for s in context if s.startswith("@")] if bare else context)
    tag = f"p{abs(hash(name))}"
    before = render(twtest, scratch / f"{tag}-a.png", context + low)
    after = render(twtest, scratch / f"{tag}-b.png", context + high)
    return name, difference(before, after)


def main():
    global HEIGHT_FILE, PHOTO_FILE
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", type=pathlib.Path, default=REPO / "build")
    parser.add_argument("--verbose", action="store_true")
    parser.add_argument("--bare", action="store_true")
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--floor", type=float, default=0.05,
                        help="mean 8-bit difference below which a control is 'barely alive'")
    args = parser.parse_args()

    build = args.build if args.build.is_absolute() else REPO / args.build
    twtest = build / "twtest"
    if not twtest.exists():
        print(f"{twtest} not found", file=sys.stderr)
        return 1

    declared = parameters(twtest)
    unknown = [n for n in declared if n not in SWEEP and n not in SKIP]
    if unknown:
        # A new parameter with no sweep is a hole, not a pass.
        print(f"no sweep defined for: {', '.join(unknown)}", file=sys.stderr)
        return 1
    unused = [n for n in SWEEP if n not in declared]
    if unused:
        print(f"the sweep names parameters the plugin does not have: {', '.join(unused)}", file=sys.stderr)
        return 1

    dead, weak = [], []
    with tempfile.TemporaryDirectory() as scratch:
        scratch = pathlib.Path(scratch)
        # Rings for the height map; a two-colour check whose luminance differs
        # from its height's for the photo.
        HEIGHT_FILE = str(scratch / "rings.png")
        PHOTO_FILE = str(scratch / "photo.png")
        w, h = 160, 90
        write_png(pathlib.Path(HEIGHT_FILE), w, h, 1,
                  [bytes(int(127.5 + 127.5 * (((x - w / 2) ** 2 + (y - h / 2) ** 2) ** 0.5 % 16 < 8)) for x in range(w))
                   for y in range(h)])
        write_png(pathlib.Path(PHOTO_FILE), w, h, 3,
                  [b"".join(bytes((200, 60, 40)) if (x // 10 + y // 10) % 2 else bytes((40, 90, 200)) for x in range(w))
                   for y in range(h)])
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
            results = list(pool.map(lambda n: sweep_one(twtest, scratch, n, args.bare), [n for n in declared if n in SWEEP]))
    for name, delta in results:
        if delta == 0.0:
            dead.append(name)
            print(f"  DEAD {name:16s} both ends identical")
        elif delta < args.floor:
            weak.append(name)
            print(f"  WEAK {name:16s} mean delta {delta:.4f}")
        elif args.verbose:
            print(f"  ok   {name:16s} mean delta {delta:.3f}")

    print(f"{len(results)} parameters swept{' (bare: base context only)' if args.bare else ''}, "
          f"{len(dead)} dead, {len(weak)} barely alive")
    if dead or weak:
        if not args.bare:
            print("\nA parameter that changes nothing is usually a uniform name that does not match\n"
                  "the C++, or a setting nothing reads. Both are silent everywhere else.", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
