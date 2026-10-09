#!/usr/bin/env python3
"""Builds macos/EngageMirror.icns from maclogo.png (needs Pillow and iconutil).

The artwork sits on a full rounded-square tile on Apple's icon grid (an 824px
squircle-ish rect inside a 1024px canvas), so macOS shows it as-is instead of
boxing a non-conforming shape into its grey fallback tile.

    python3 scripts/make-macos-icon.py [dark|light]
"""
import pathlib
import subprocess
import sys
import tempfile

from PIL import Image, ImageDraw, ImageFilter

ROOT = pathlib.Path(__file__).resolve().parent.parent
THEMES = {
    # top colour, bottom colour
    "light": ((248, 247, 255), (221, 216, 248)),
    "dark": ((40, 30, 92), (12, 11, 34)),
}


def tile(theme):
    size, box, radius = 1024, 824, 185
    top, bottom = THEMES[theme]
    grad = Image.new("RGBA", (box, box))
    px = grad.load()
    for y in range(box):
        t = y / (box - 1)
        c = tuple(round(top[i] + (bottom[i] - top[i]) * t) for i in range(3)) + (255,)
        for x in range(box):
            px[x, y] = c
    mask = Image.new("L", (box, box), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, box - 1, box - 1), radius, fill=255)

    canvas = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    # Soft drop shadow under the tile, as on Apple's own icons.
    shadow = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    shadow.paste((0, 0, 0, 90), (100, 112, 100 + box, 112 + box), mask)
    canvas.alpha_composite(shadow.filter(ImageFilter.GaussianBlur(14)))
    canvas.paste(grad, (100, 100), mask)

    art = Image.open(ROOT / "maclogo.png").convert("RGBA")
    w = 700
    art = art.resize((w, round(art.height * w / art.width)), Image.LANCZOS)
    canvas.alpha_composite(art, ((size - art.width) // 2, (size - art.height) // 2 + 6))
    return canvas


def main():
    theme = sys.argv[1] if len(sys.argv) > 1 else "dark"
    icon = tile(theme)
    with tempfile.TemporaryDirectory() as tmp:
        iconset = pathlib.Path(tmp) / "EngageMirror.iconset"
        iconset.mkdir()
        for pt in (16, 32, 128, 256, 512):
            for scale in (1, 2):
                px = pt * scale
                name = f"icon_{pt}x{pt}{'@2x' if scale == 2 else ''}.png"
                icon.resize((px, px), Image.LANCZOS).save(iconset / name)
        out = ROOT / "macos" / "EngageMirror.icns"
        subprocess.run(["iconutil", "-c", "icns", str(iconset), "-o", str(out)], check=True)
    print(f"wrote {out}")


if __name__ == "__main__":
    main()
