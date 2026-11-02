"""The browser demo against the plugin: the same card, two renderers, one picture.

    python3 demo/tools/crosscheck/crosscheck.py [--twtest build-verify/twtest] [--size 640x360] [--keep DIR]

Each case below is rendered twice, from the harness's look card:

  - by the PLUGIN: `twtest --pipe`, the real plugin class in a GL 4.1 context
    (build it first; tools/verify.sh leaves one in build-verify/);
  - by the PAGE: demo/ served from here and loaded in headless Chrome, through
    window.throwDemo.renderCase, which makes a fresh instance and renders the
    same card with the same settings.

The card is the harness's own (`twtest --film 1` at Mix 0, which hands its
clip back to the bit), fed to both as RGBA8. Both pictures are compared pixel
by pixel. Two cases throw a Surface File: an 8-bit height map and an 8-bit
photo this script writes, given to twtest by path and to the page by URL,
decoded by stb_image there and by the browser here.

The page's GPU half is the plugin's GLSL (check_shaders.py), so what this
mostly tests is the PORT: every mapping, the room in double, each surface's
relief, the metals, the paper's generations, the surface key, HeightFile's
reading of a file, and every uniform. A difference is a port bug, or a
difference between GL 4.1 on this machine and WebGL2 through ANGLE on the same
GPU. Before any picture, the page's table of controls (names, kinds, defaults)
is compared with `twtest --list`.

A case passes when no pixel is more than one level of 255 apart and 99% are
identical. Negative controls -- the page with one control one slider step
(0.001) off, or the paper's Seed one off -- must each fail it, so the
comparison can see a slip of that size.

Measured on an M4 Max (GL 4.1 against Chrome's ANGLE on Metal), at 640x360
and 1280x720: every case within one level everywhere, 99.17% to 100%
identical; Throw one step off is 98.8% identical, every other control 97.9%
or less, or more than one level somewhere.

Not run by tools/verify.sh: it needs a GPU and Chrome. Exit 1 if the table
differs, a uniform the page sets is missing from a program, a case fails or a
negative control passes.
"""
import argparse
import functools
import http.server
import json
import os
import shutil
import subprocess
import sys
import tempfile
import threading

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
DEMO = os.path.abspath(os.path.join(HERE, "..", ".."))
REPO = os.path.abspath(os.path.join(DEMO, ".."))
sys.dont_write_bytecode = True

# name, settings as `twtest --set` takes them (the host's values; options by
# name). "Surface File" names one of FILES; the script fills in the path for
# twtest and the URL for the page.
CASES = [
    ("defaults", {}),
    ("paper-crumpled", {"Surface": "Paper", "Crumple": "1", "Depth": "0.7071", "Viewer X": "0.2"}),
    ("brick-english-deep", {"Bond": "English", "Depth": "1", "Mortar": "0.8", "Viewer X": "0.85"}),
    ("brick-flemish-pointed", {"Bond": "Flemish", "Aim": "Pointed", "Projector X": "0.3", "Throw": "0.35", "Seed": "7"}),
    ("brick-stack-scaled", {"Bond": "Stack", "Scale": "0.8", "Wall Height": "0.3", "Viewer Y": "0.8"}),
    ("corrugated-sine", {"Surface": "Corrugated"}),
    ("corrugated-box-rough", {"Surface": "Corrugated", "Profile": "Box", "Pitch": "0.7", "Roughness": "0.1", "Depth": "0.8"}),
    ("plate-five-bar", {"Surface": "Plate", "Pattern": "Five-Bar", "Depth": "1", "Viewer Y": "0.6"}),
    ("plate-crosshatch-mix", {"Surface": "Plate", "Pattern": "Crosshatch", "Mix": "0.6", "Scale": "0.7"}),
    ("drapes-defaults", {"Surface": "Drapes"}),
    ("drapes-left-sag", {"Surface": "Drapes", "Pleats": "8", "Sag": "0.9", "Viewer X": "0.3", "Viewer Y": "0.6581", "Viewer Distance": "0.458"}),
    ("weatherboard-dim", {"Surface": "Weatherboard", "Depth": "0.8", "Ambient": "0.4", "Lumens": "0.3"}),
    ("tiles-bright", {"Surface": "Tiles", "Exposure": "0.7", "Black Level": "0.6", "Wall Height": "0.35"}),
    ("flat-keystone", {"Surface": "Flat", "Aim": "Pointed", "Projector X": "0.75"}),
    ("view-height-paper", {"Surface": "Paper", "View": "Height"}),
    ("view-normals-drapes", {"Surface": "Drapes", "View": "Normals"}),
    ("view-light-brick", {"View": "Light", "Depth": "1"}),
    ("view-shadow-weatherboard", {"Surface": "Weatherboard", "View": "Shadow", "Depth": "0.8"}),
    ("view-albedo-brick", {"View": "Albedo", "Seed": "42"}),
    ("file-none", {"Surface": "File"}),
    ("file-height", {"Surface": "File", "Surface File": "height.png", "Depth": "0.8", "Viewer X": "0.25"}),
    ("file-photo-tiled", {"Surface": "File", "File Holds": "Photo", "Surface File": "photo.png", "Scale": "0.4"}),
]
# The negative controls: a case rendered by the page with one setting a little
# off, compared with the plugin's render of the case as it should be. Each is
# the size of a plausible port slip; each must fail the comparison.
NEGATIVES = [
    ("defaults", "throw-1-step", {"Throw": "0.459"}, "Throw one slider step (0.001) off"),
    ("defaults", "viewer-1-step", {"Viewer X": "0.7246"}, "Viewer X one step off"),
    ("defaults", "depth-1-step", {"Depth": "0.501"}, "Depth one step off"),
    ("drapes-defaults", "sag-1-step", {"Sag": "0.301"}, "Sag one step off"),
    ("defaults", "lumens-1-step", {"Lumens": "0.6515"}, "Lumens one step off"),
    ("paper-crumpled", "seed-off", {"Seed": "2"}, "the paper's Seed one off"),
]


def parse_size(text):
    w, h = text.lower().split("x")
    return int(w), int(h)


def run_twtest(twtest, args, stdin=None):
    done = subprocess.run([twtest, *args], input=stdin, capture_output=True, check=False)
    if done.returncode != 0:
        sys.exit(f"twtest {' '.join(args)} failed: {done.stderr.decode(errors='replace').strip()}")
    return done.stdout


def write_files(folder, width, height):
    """An 8-bit grey height map and an 8-bit colour photo, not at the frame's raster."""
    from PIL import Image
    w, h = width * 3 // 4, height * 3 // 4
    y, x = np.mgrid[0:h, 0:w].astype(np.float64)
    u, v = (x + 0.5) / w - 0.5, (y + 0.5) / h - 0.5
    bumps = 0.5 + 0.5 * np.cos(2 * np.pi * 5 * u) * np.cos(2 * np.pi * 3 * v)
    disc = (np.hypot(u + 0.2, v - 0.1) < 0.15).astype(np.float64)
    steps = np.floor((u + 0.5) * 6) / 6
    height_map = np.clip(0.55 * bumps + 0.3 * steps + 0.15 * disc, 0, 1)
    Image.fromarray(np.round(height_map * 255).astype(np.uint8), "L").save(os.path.join(folder, "height.png"))
    photo = np.stack([0.5 + 0.5 * np.cos(2 * np.pi * (u * 2 + 0.1)),
                      0.4 + 0.4 * (v + 0.5),
                      np.clip(0.3 + 0.7 * disc + 0.2 * bumps, 0, 1)], axis=2)
    Image.fromarray(np.round(np.clip(photo, 0, 1) * 255).astype(np.uint8), "RGB").save(os.path.join(folder, "photo.png"))


class Handler(http.server.SimpleHTTPRequestHandler):
    """demo/ as it is, plus /xc/ for the job, the card, the files and the page's answers."""

    def __init__(self, *args, state, **kwargs):
        self.state = state
        super().__init__(*args, directory=DEMO, **kwargs)

    def log_message(self, *args):
        pass

    def do_GET(self):
        if self.path == "/xc/job.json":
            return self.reply(json.dumps(self.state["job"]).encode(), "application/json")
        if self.path == "/xc/card.rgba":
            return self.reply(self.state["card"], "application/octet-stream")
        if self.path.startswith("/xc/files/"):
            name = os.path.basename(self.path)
            with open(os.path.join(self.state["files"], name), "rb") as handle:
                return self.reply(handle.read(), "image/png")
        return super().do_GET()

    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        if self.path.startswith("/xc/out/"):
            self.state["page"][self.path[len("/xc/out/"):]] = body
        elif self.path == "/xc/table":
            self.state["table"] = json.loads(body)
        elif self.path == "/xc/missing":
            self.state["missing"] = json.loads(body)
        elif self.path == "/xc/format":
            self.state["format"] = body.decode()
        elif self.path == "/xc/error":
            self.state["error"] = body.decode(errors="replace")
            self.state["finished"].set()
        elif self.path == "/xc/done":
            self.state["finished"].set()
        self.reply(b"ok", "text/plain")

    def reply(self, body, kind):
        self.send_response(200)
        self.send_header("Content-Type", kind)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def compare(a, b, width, height):
    x = np.frombuffer(a, np.uint8).reshape(height, width, 4).astype(np.int16)
    y = np.frombuffer(b, np.uint8).reshape(height, width, 4).astype(np.int16)
    worst = np.abs(x - y).max(axis=2)
    return {
        "exact": float((worst == 0).mean() * 100.0),
        "within1": float((worst <= 1).mean() * 100.0),
        "within4": float((worst <= 4).mean() * 100.0),
        "max": int(worst.max()),
        "mean": float(np.abs(x - y)[..., :3].mean()),
    }


def passes(s, args):
    return s["max"] <= args.max and s["exact"] >= args.exact


def compare_table(twtest, page_table):
    """`twtest --list` against the page's kTable: name, kind, default (4 dp), in order."""
    rows = []
    for line in run_twtest(twtest, ["--list"]).decode().splitlines()[1:]:
        parts = line.split()
        if len(parts) < 4:
            continue
        index, kind, value = int(parts[0]), parts[-2], float(parts[-1])
        rows.append((index, " ".join(parts[1:-2]), kind, value))
    plugin = [r for r in rows if r[2] not in ("text", "event")]
    problems = []
    if len(plugin) != len(page_table):
        problems.append(f"twtest lists {len(plugin)} controls, the page {len(page_table)}")
    for (index, name, kind, value), mine in zip(plugin, page_table):
        if (name, kind) != (mine["name"], mine["kind"]) or abs(value - mine["value"]) > 0.00005:
            problems.append(f"#{index}: twtest says {name} {kind} {value:.4f}, the page "
                            f"{mine['name']} {mine['kind']} {mine['value']:.4f}")
    return len(plugin), problems


def main(argv):
    parser = argparse.ArgumentParser()
    parser.add_argument("--twtest", default=os.path.join(REPO, "build-verify", "twtest"))
    parser.add_argument("--size", default="640x360")
    parser.add_argument("--keep", help="write each case's two pictures here as PNGs")
    parser.add_argument("--max", type=int, default=1,
                        help="the largest difference, in levels of 255, allowed anywhere (default 1)")
    parser.add_argument("--exact", type=float, default=99.0,
                        help="share of pixels, %%, that must be identical (default 99)")
    parser.add_argument("--chrome", default=os.environ.get(
        "CHROME", "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"))
    parser.add_argument("--port", type=int, default=8793)
    args = parser.parse_args(argv)
    width, height = parse_size(args.size)
    if not os.access(args.twtest, os.X_OK):
        sys.exit(f"no twtest at {args.twtest}: build it (cmake -B build-verify && cmake --build build-verify --target twtest)")

    size = f"{width}x{height}"
    frame_bytes = width * height * 4
    card = run_twtest(args.twtest, ["--film", "1", "--size", size, "--set", "Mix=0"])
    if len(card) != frame_bytes:
        sys.exit(f"twtest --film gave {len(card)} bytes, not one frame")

    files = tempfile.mkdtemp(prefix="throw-xc-")
    write_files(files, width, height)

    plugin = {}
    for name, settings in CASES:
        sets = []
        for key, value in settings.items():
            if key == "Surface File":
                value = os.path.join(files, value)
            sets += ["--set", f"{key}={value}"]
        out = run_twtest(args.twtest, ["--pipe", "--size", size, *sets], stdin=card)
        if len(out) != frame_bytes:
            sys.exit(f"twtest --pipe for {name} gave {len(out)} bytes, not one frame")
        plugin[name] = out

    def for_page(settings):
        return {k: (f"/xc/files/{v}" if k == "Surface File" else v) for k, v in settings.items()}

    cases = [{"name": name, "settings": for_page(settings)} for name, settings in CASES]
    for base_name, name, change, _ in NEGATIVES:
        cases.append({"name": name, "settings": for_page({**dict(CASES)[base_name], **change})})
    state = {"job": {"width": width, "height": height, "cases": cases},
             "card": card, "files": files, "page": {}, "table": None, "missing": None,
             "error": None, "finished": threading.Event()}

    server = http.server.ThreadingHTTPServer(("127.0.0.1", args.port), functools.partial(Handler, state=state))
    threading.Thread(target=server.serve_forever, daemon=True).start()
    profile = tempfile.mkdtemp(prefix="throw-chrome-")
    chrome = subprocess.Popen([
        args.chrome, "--headless=new", "--no-first-run", "--no-default-browser-check",
        f"--user-data-dir={profile}", "--use-angle=metal", "--enable-gpu", "--ignore-gpu-blocklist",
        "--window-size=1600,1200", f"http://127.0.0.1:{args.port}/tools/crosscheck/",
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        if not state["finished"].wait(300):
            sys.exit("the page never finished (300 s)")
    finally:
        chrome.terminate()
        try:
            chrome.wait(10)
        except subprocess.TimeoutExpired:
            chrome.kill()
        server.shutdown()
        shutil.rmtree(profile, ignore_errors=True)
    if state["error"]:
        shutil.rmtree(files, ignore_errors=True)
        sys.exit(f"the page failed:\n{state['error']}")

    failures = 0
    count, table_problems = compare_table(args.twtest, state["table"] or [])
    if table_problems:
        failures += len(table_problems)
        print("the page's controls differ from twtest --list:")
        for line in table_problems:
            print(f"  {line}")
    else:
        print(f"controls: all {count} of twtest --list's, same names, kinds and defaults, in order")
    if state["missing"]:
        failures += 1
        print(f"uniforms the page set that the programs do not have: {', '.join(sorted(set(state['missing'])))}")
    print(f"surface format in this browser: {state.get('format', '?')}\n")

    if args.keep:
        from PIL import Image
        os.makedirs(args.keep, exist_ok=True)

    print(f"{size}, the harness's card, one frame\n")
    print(f"{'case':<26} {'==':>9} {'<=1':>9} {'<=4':>9} {'max':>4} {'mean':>7}")
    for name, _ in CASES:
        got = state["page"].get(name)
        if got is None or len(got) != frame_bytes:
            print(f"{name:<26} no picture from the page")
            failures += 1
            continue
        s = compare(plugin[name], got, width, height)
        ok = passes(s, args)
        failures += 0 if ok else 1
        print(f"{name:<26} {s['exact']:8.3f}% {s['within1']:8.3f}% {s['within4']:8.3f}% {s['max']:>4} {s['mean']:7.4f}  {'ok' if ok else 'FAIL'}")
        if args.keep:
            for label, data in (("plugin", plugin[name]), ("page", got)):
                Image.frombytes("RGBA", (width, height), data).save(os.path.join(args.keep, f"{name}-{label}.png"))

    print("\nnegative controls: the page a little off, against the plugin as it should be")
    for base_name, name, _, label in NEGATIVES:
        late = state["page"].get(name)
        s = compare(plugin[base_name], late, width, height) if late else None
        caught = s is not None and not passes(s, args)
        failures += 0 if caught else 1
        print(f"  {label:<38} " + (f"{s['exact']:8.3f}% {s['within1']:8.3f}% {s['max']:>4} {s['mean']:7.4f}  "
                                     f"{'caught' if caught else 'NOT CAUGHT'}" if s else "no picture"))
    print(f"\n{'every case passes, every negative control is caught' if not failures else f'{failures} problem(s)'}"
          f" (pass: no pixel more than {args.max} level(s) apart and {args.exact}% identical)")
    shutil.rmtree(files, ignore_errors=True)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
