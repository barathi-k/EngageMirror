# Device frames

EngageMirror draws the mirrored screen inside a picture of the device when it
finds one here, and falls back to a drawn (procedural) frame when it doesn't.
The images are **not** part of this repository: device artwork is generally
not redistributable, so bring your own.

A frame is a PNG with an opaque device body and a **fully transparent screen
area**. Nothing needs measuring; the loader finds the screen from the alpha
channel. Author it portrait, comfortably larger than it will be shown.

File names the app looks for:

| Device | File |
|---|---|
| Any iPad | `ipad_pro.png` |
| iPhone, by model | `iphone_<model>.png`, e.g. `iphone_15.png`, `iphone_16_pro_max.png` (the full mapping is `PhoneSkin()` in [src/device_db.cpp](../src/device_db.cpp)) |

`scripts/fetch-device-frames.py` downloads iPhone frames from
[device-frames-media](https://github.com/jonnyjackson26/device-frames-media)
into this folder for local use. That collection has no stated licence, so
check its terms before you redistribute anything built with them.

Builds copy every `*.png` in this folder into the app (`Resources/assets` on
macOS, `assets\` beside the exe on Windows).
