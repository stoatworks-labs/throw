#!/usr/bin/env python3
"""The README's figures, rendered by twtest, the offline harness.

One frame of a Resolume clip thrown at every surface at the defaults, and the
viewer walking past the drapes. Needs a build (./build/twtest), ffmpeg, Pillow,
and Resolume Arena's bundled clips, which is where the frame comes from:

    tools/figures.py                     # docs/surfaces.jpg, parallax.jpg, hero.jpg
    tools/figures.py --film walk.mp4     # and the walk as five seconds of film

The film is not committed: it is for looking at, not for the README.
"""
import argparse
import math
import pathlib
import subprocess
import sys
import tempfile

from PIL import Image, ImageDraw, ImageFont

REPO = pathlib.Path(__file__).resolve().parent.parent
CLIP = "/Applications/Resolume Arena/media/Shop74/IntoTheGlow_02.mov"
AT = "1.67"  # seconds in: the frame slitscan's figures use
W, H = 960, 540


# Controls.cpp's mappings, inverted: the host's 0..1 for a value in metres.
def offset(metres):
    s = max(-1.0, min(1.0, metres / 10.0))
    r = math.sqrt(abs(s))
    return 0.5 + 0.5 * (-r if s < 0 else r)


def distance(metres):
    return math.log(metres) / math.log(50.0)


def font(size):
    for path in ("/System/Library/Fonts/Helvetica.ttc", "/System/Library/Fonts/SFNS.ttf"):
        try:
            return ImageFont.truetype(path, size)
        except OSError:
            pass
    return ImageFont.load_default(size)


def label(img, text):
    d = ImageDraw.Draw(img)
    f = font(26)
    box = d.textbbox((0, 0), text, font=f)
    d.rectangle([0, 0, box[2] + 20, box[3] + 20], fill=(0, 0, 0))
    d.text((10, 10 - box[1] // 2), text, font=f, fill=(255, 255, 255))
    return img


def sheet(tiles, cols, gap=6):
    rows = (len(tiles) + cols - 1) // cols
    w, h = tiles[0].size
    out = Image.new("RGB", (cols * w + (cols - 1) * gap, rows * h + (rows - 1) * gap), (255, 255, 255))
    for i, t in enumerate(tiles):
        out.paste(t, ((i % cols) * (w + gap), (i // cols) * (h + gap)))
    return out


def save(img, name, shrink=True):
    if shrink:
        img = img.resize((img.width * 2 // 3, img.height * 2 // 3), Image.LANCZOS)
    img.save(REPO / "docs" / name, quality=88, optimize=True, progressive=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build", type=pathlib.Path, default=REPO / "build")
    parser.add_argument("--film", type=pathlib.Path, help="also render the walk as an MP4 here")
    args = parser.parse_args()
    twtest = args.build / "twtest"
    work = pathlib.Path(tempfile.mkdtemp(prefix="throw-figures-"))
    clip = work / "clip.png"
    subprocess.run(["ffmpeg", "-v", "error", "-ss", AT, "-i", CLIP, "-frames:v", "1", "-vf", f"scale={W}:{H}", "-y", str(clip)],
                   check=True)

    def render(name, sets):
        out = work / f"{name}.png"
        command = [str(twtest), "--out", str(out), "--size", f"{W}x{H}", "--clip", str(clip)]
        for key, value in sets:
            command += ["--set", f"{key}={value}"]
        result = subprocess.run(command, capture_output=True, text=True)
        if result.returncode != 0:
            sys.exit(f"{name}: {result.stderr}")
        return Image.open(out).convert("RGB")

    (REPO / "docs").mkdir(exist_ok=True)

    # Every surface at the defaults: real size, real depth, the viewer 2 m right
    # of the lens and 0.8 m below the frame's centre, 4 m back.
    surfaces = ["Flat", "Paper", "Brick", "Corrugated", "Plate", "Drapes", "Weatherboard", "Tiles"]
    tiles = [label(Image.open(clip).convert("RGB"), "The clip")]
    tiles += [label(render(f"surface-{s}", [("Surface", s)]), s) for s in surfaces]
    save(sheet(tiles, 3), "surfaces.jpg")

    # The viewer walks past the drapes, level with the lens and as far back. At
    # the lens there is no shadow and no slide; either side, the picture slides
    # over the folds. Below each, the same window at twice the size.
    walk, zoom = [], []
    for m in (-4.0, 0.0, 4.0):
        sets = [("Surface", "Drapes"), ("Viewer X", offset(m)), ("Viewer Y", offset(1.0)), ("Viewer Distance", distance(6.0))]
        frame = render(f"walk-{m:+.0f}", sets)
        name = f"Viewer {abs(m):.0f} m {'left' if m < 0 else 'right'}" if m else "Viewer at the lens"
        zoom.append(label(frame.crop((40, 120, 520, 390)).resize((W, H), Image.LANCZOS), name + ", 2x"))
        walk.append(label(frame, name))
    save(sheet(walk + zoom, 3), "parallax.jpg")

    save(render("hero", [("Surface", "Drapes")]), "hero.jpg", shrink=False)

    if args.film:
        frames = 150
        cues = work / "walk.cues"
        cues.write_text("".join(f"{k} Viewer X {offset(-4.0 + 8.0 * k / (frames - 1)):.6f}\n" for k in range(frames)))
        decode = subprocess.Popen(["ffmpeg", "-v", "error", "-i", CLIP, "-vf", f"scale={W}:{H},fps=30", "-frames:v", str(frames),
                                   "-f", "rawvideo", "-pix_fmt", "rgba", "-"], stdout=subprocess.PIPE)
        throw = subprocess.Popen([str(twtest), "--pipe", "--size", f"{W}x{H}", "--script", str(cues), "--set", "Surface=Drapes",
                                  "--set", f"Viewer Y={offset(1.0)}", "--set", f"Viewer Distance={distance(6.0)}"],
                                 stdin=decode.stdout, stdout=subprocess.PIPE)
        decode.stdout.close()
        encode = subprocess.run(["ffmpeg", "-v", "error", "-f", "rawvideo", "-pix_fmt", "rgba", "-s", f"{W}x{H}", "-r", "30", "-i", "-",
                                 "-c:v", "libx264", "-pix_fmt", "yuv420p", "-crf", "20", "-y", str(args.film)], stdin=throw.stdout)
        if throw.wait() != 0 or decode.wait() != 0 or encode.returncode != 0:
            sys.exit("the film did not render")
    print(f"wrote {REPO / 'docs'}: surfaces.jpg, parallax.jpg, hero.jpg" + (f"; {args.film}" if args.film else ""))


if __name__ == "__main__":
    main()
