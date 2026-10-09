#!/usr/bin/env python3
"""Download iPhone device frames into assets/ for a local build.

Source: https://github.com/jonnyjackson26/device-frames-media, whose frames
have an opaque body and a transparent screen - exactly what src/skin.cpp
expects. The files are NOT committed (that collection states no licence); they
end up only in installers you build yourself. See assets/README.md.

    python3 scripts/fetch-device-frames.py            # skip files already present
    python3 scripts/fetch-device-frames.py --force    # re-download everything
"""
import json
import pathlib
import sys
import urllib.request

INDEX = ("https://raw.githubusercontent.com/jonnyjackson26/device-frames-media/"
         "main/device-frames-output/index.json")
# Dark finishes first: the frame should recede behind the picture. Variants
# with a baked-in shadow are skipped because the renderer draws its own.
PREFERRED = ["space-black", "black-titanium", "black", "midnight", "graphite",
             "space-grey", "space-gray", "deep-blue", "grey", "silver", "white"]

assets = pathlib.Path(__file__).resolve().parent.parent / "assets"
force = "--force" in sys.argv[1:]


def pick(variants):
    plain = [v for v in variants if "shadow" not in v]
    for want in PREFERRED:
        if want in plain:
            return want
    return sorted(plain)[0] if plain else None


def main():
    with urllib.request.urlopen(INDEX, timeout=30) as r:
        index = json.load(r)

    assets.mkdir(exist_ok=True)
    for model, variants in sorted(index["apple-iphone"].items()):
        variant = pick(variants)
        if not variant:
            continue
        out = assets / f"iphone_{model.replace('-', '_')}.png"
        if out.exists() and not force:
            print(f"have  {out.name}")
            continue
        url = variants[variant]["frame"]
        tmp = out.with_suffix(".part")
        with urllib.request.urlopen(url, timeout=60) as r, open(tmp, "wb") as f:
            f.write(r.read())
        tmp.replace(out)
        print(f"got   {out.name}  ({variant})")


if __name__ == "__main__":
    main()
