# EngageMirror

An AirPlay screen-mirroring receiver for **macOS** and **Windows** that draws
your iPhone or iPad screen **inside a matching device frame**: correct aspect
ratio, correct bezels, and it flips between portrait and landscape the moment
you turn the real device.

Free software under the [GNU GPL v3](LICENSE), built on
[UxPlay](https://github.com/FDH2/UxPlay). Works with AirPlay; not affiliated
with or endorsed by Apple.

```
iPhone / iPad ──AirPlay──▶ libairplay (UxPlay core) ──▶ FFmpeg + VideoToolbox ──▶ Metal frame shader      (macOS)
                                                    └──▶ FFmpeg + D3D11VA      ──▶ D3D11 + DirectComposition (Windows)
                                                         (zero-copy NV12 on both)
```

---

## What it does

| Requirement | How it is met |
|---|---|
| Open-source AirPlay mirroring core | [UxPlay](https://github.com/FDH2/UxPlay) 1.74 `libairplay` (protocol only — no GStreamer) |
| Discovery | macOS: the system's mDNSResponder. Windows: UxPlay 1.74's built-in mDNS responder (`lib/mdnsd`), so no Apple Bonjour install is needed |
| Device frame around the video | Photographic PNG frames for iPhones and iPads when present (see [Device frames](#device-frames)); otherwise a procedural SDF frame with chassis, bezels, notch / Dynamic Island and camera glint |
| Correct aspect ratio | The screen rectangle is derived from the **live stream dimensions**, never from a lookup table — video is never stretched |
| Auto rotate | The whole device pivots through the turn, like the real thing — see [Rotation](#rotation) |
| Efficient | Hardware decode straight into a GPU texture that the shader samples directly, so the video never touches system memory. Idle costs nothing: no timers or polling, and the audio device runs only during a session |

## Requirements

- **macOS:** 12 Monterey or later, Apple Silicon
- **Windows:** 10 1809+ / 11 (needs DirectComposition and D3D11) and a GPU
  with H.264 decode (falls back to software otherwise)
- iPhone/iPad on the **same network** as the computer

## Running

**macOS:** open the DMG, drag Engage Mirror to Applications and launch it. When
macOS asks whether Engage Mirror may find devices on your local network, click
**Allow**: without it your iPhone can't see the Mac. If you missed the prompt,
turn it on under System Settings → Privacy & Security → Local Network. The
receiver appears as "*Your Mac's name* (Engage Mirror)", so it doesn't collide
with macOS's own AirPlay Receiver.

**Windows:** launch `engagemirror.exe`.

Then, on the iPhone or iPad, open Control Centre → **Screen Mirroring** and
pick the entry.

### Window controls

| Action | Control |
|---|---|
| Move | Drag anywhere on the frame |
| Resize | Mouse wheel, or `+` / `-` |
| Reset size | `0` |
| Flip landscape orientation | `L` |
| Rotate (preview aid — with a live client the real stream overrules it) | `R` |
| Mute | `M` |
| Menu (also has About, with licence and source link) | Right-click |
| Quit | `Esc` (macOS: also ⌘Q) |

### Command line

```
engagemirror.exe [--name <text>] [--size <WxH>] [--fps <n>] [--scale <f>] [--h265]
              [--device <model>] [--preview <WxH>]
# macOS:
"/Applications/Engage Mirror.app/Contents/MacOS/EngageMirror" [same options]
```

- `--name`: what appears in the AirPlay list (default: the computer's name)
- `--size` — display size advertised to the client, default `1920x1080`.
  **Useful for iPads:** a 4:3 iPad may choose to send a 16:9 stream to a
  16:9 receiver. `--size 1440x1080` asks for 4:3 instead.
- `--fps` — advertised refresh rate / max FPS (default 60)
- `--scale` — starting window scale, 0.3–3.0
- `--h265` — advertise H.265 (only needed for 4K sources)
- `--device` / `--preview` — show a device frame with nothing connected, for
  checking a skin. E.g. `--device iPad13,4 --preview 2732x2048` renders the
  iPad skin in landscape.

## Building on macOS

Needs Xcode (or the Command Line Tools), CMake and Ninja
(`brew install cmake ninja pkg-config`). Python 3 with Pillow only if you
regenerate the icon.

```bash
# 1. OpenSSL, libplist and a decoders-only FFmpeg, built static from pinned,
#    checksummed release tarballs into vendor/macos-deps/ (a few minutes)
scripts/build-deps-macos.sh

# 2. optional: device frames for your own builds (see Device frames)
python3 scripts/fetch-device-frames.py

# 3. build -> build-macos/EngageMirror.app (ships as "Engage Mirror.app")
cmake -S . -B build-macos -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build-macos

# 4. or all of it plus signing, a DMG and notarization
scripts/package-macos.sh                 # -> dist-macos/EngageMirror-<version>.dmg
scripts/package-macos.sh --no-notarize   # sign + DMG only
```

`package-macos.sh` signs with the first "Developer ID Application"
certificate in your keychain (`SIGN_IDENTITY` overrides it) and notarizes
with the notarytool keychain profile in `NOTARY_PROFILE`. Create one with
`xcrun notarytool store-credentials <profile> --apple-id <id> --team-id <team>`.

The app links everything statically except system frameworks, so the bundle
holds a single executable.

## Building on Windows

Needs [MSYS2](https://www.msys2.org/) at `C:\msys64`.

```bash
# 1. toolchain + libraries
/c/msys64/usr/bin/pacman -S --needed --noconfirm \
  mingw-w64-ucrt-x86_64-{gcc,cmake,ninja,pkgconf,openssl,libplist,ffmpeg}

# 2. build
bash scripts/build.sh

# 3. optional but recommended: a stripped FFmpeg, ~115 MB -> a few MB
bash scripts/build-ffmpeg-mini.sh
bash scripts/build.sh          # reconfigure against it

# 4. standalone folder with every DLL
bash scripts/package.sh        # -> dist/
```

`scripts/build-ffmpeg-mini.sh` builds FFmpeg with `--disable-everything` plus
only the H.264/HEVC/AAC/ALAC decoders and the D3D11VA hwaccel. The stock MSYS2
package works fine but drags in x264, x265, AV1, pango, cairo and librsvg.

## How it works

**Protocol.** `vendor/UxPlay` is a copy of UxPlay 1.74 (commit `2ece579`)
with one Windows-only change to `lib/mdnsd/mdnsd.c`, marked in the file and
described in [patches/](patches). Only `lib/` is compiled (the
RTSP/RAOP/FairPlay/mirroring core) as a static `libairplay`. UxPlay's GStreamer
renderer is not built; `src/airplay_server.cpp` implements the callback surface
instead. That is what keeps the binary at ~1.5 MB rather than a GStreamer runtime.

**Discovery.** On Windows, `lib/mdnsd` is UxPlay 1.74's own mDNS responder.
The executable has no import of Apple's `dnssd.dll`, and it co-binds UDP 5353
with `SO_REUSEADDR`, so it works whether or not Bonjour happens to be
installed. On macOS, UxPlay's `lib/dns_sd` backend registers with the system
mDNSResponder. macOS 15+ silently withholds that registration until the user
grants Local Network access, and a registration made before the grant never
recovers, so [app_mac.mm](src/app_mac.mm) watches the permission with a
Network.framework browser and re-advertises the moment it is granted.

**Video.** `src/video_decoder.cpp` shares the app's `ID3D11Device` with FFmpeg
through `AVD3D11VADeviceContext`, and sets
`BindFlags = D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE` on the decoder's
surface pool. Decoded frames arrive as a slice of a D3D11 texture array; the
renderer creates `R8_UNORM` / `R8G8_UNORM` views onto that slice and samples
NV12 directly. No copies, no CPU colour conversion.

D3D11VA rounds decode surfaces up to a multiple of 16, so an 810×1080 stream
lives in an 816×1088 texture. The renderer scales its uv by picture/surface and
clamps a texel short of the edge; sampling the full 0→1 range instead puts a
bright green stripe of decoder padding down the right of the picture (zeroed
NV12 is green through the BT.709 matrix), and only for stream widths that aren't
already 16-aligned — 1440 is, 810 isn't, which is why it showed in portrait
only. The startup log names the surface whenever it is padded:

```
video: decode surface 816x1088 for a 810x1080 picture
```

`--uvtest` checks this without needing a device: it displays a synthetic white
810×1080 picture inside a real 816×1088 NV12 surface whose padding is zeroed.
Any green at the picture's edge means the renderer is sampling past it.

On macOS, FFmpeg's VideoToolbox hwaccel hands back `CVPixelBuffer`s, which
[video_decoder.cpp](src/video_decoder.cpp) wraps as `R8Unorm` / `RG8Unorm`
Metal textures through a `CVMetalTextureCache`, which is still zero-copy. Each
frame's buffers are kept alive in the command buffer's completion handler until
the GPU has finished sampling them. The same uv clamp applies, and `--uvtest`
works on both platforms.

**Audio.** Mirroring audio is AAC-ELD, which Windows' Media Foundation cannot
decode, which is why FFmpeg handles audio too. Output is WASAPI shared mode
(Windows) or the default-output AudioUnit (macOS) at 44.1 kHz, converted by
the OS, so the app never resamples. The device is only started while a client
is connected.

**Idle.** Nothing runs between events. The macOS frame loop ticks when a
frame is decoded or state changes, and runs at frame rate only while something
animates. The Windows message loop waits without a timeout. With no device
connected the process sits at 0% CPU with no timer wakeups, and App Nap is
only held off during a session.

**The frame.** [renderer.cpp](src/renderer.cpp) draws the device in one
fullscreen pass onto a per-pixel-transparent surface: a DirectComposition swap
chain on Windows, a non-opaque `CAMetalLayer` on macOS
([renderer_mac.mm](src/renderer_mac.mm) is a line-for-line MSL port of the
HLSL). With a photographic frame the pass lays down a black backing, the
video, then the PNG over the top; the PNG's transparent screen area is what
the video shows through. Without one, the frame is signed-distance-field
geometry: vector maths, so it is sharp at any size and needs no assets.
Styling (bezel thickness, corner radius, notch vs Dynamic Island, home button)
comes from [device_db.cpp](src/device_db.cpp), keyed on the model identifier
the client reports (`iPhone16,1`, `iPad11,3`, …).

### Rotation

The chassis is a rigid body that **pivots** through the turn rather than
snapping between two layouts. All chassis geometry lives in *device space* —
the device's own portrait frame — and the shader rotates the sample point into
it, so an arbitrary angle costs nothing and the PNG skin needs no rotated
variants.

The video is the opposite: it is always drawn **upright in window space** and
clipped to the (rotating) panel, because content has to stay readable however
the device is held. That is what a real device does when its UI counter-rotates.

Three things make it smooth, and each was a distinct source of the stutter this
replaced:

1. **The window never changes size.** It is a square, sized once from the
   device's long edge plus the shadow, so it fits either orientation. Nothing
   calls `SetWindowPos` or `ResizeBuffers` during a pivot. This matters more
   than it sounds: a DirectComposition swap chain is pinned to the window's
   client origin at 1:1, so for the frame or two between the HWND changing size
   and the next `Present`, the surface and the window disagree — the device
   gets cropped at the bottom-right and shoved off-centre. That was the "the
   whole app is cropped, then it comes back" glitch.

   A square that fits at 0° and 90° is still too small at 45°, where a *w*×*h*
   device needs (*w*+*h*)/√2 in both axes. Rather than grow the window,
   `SweepScale` shrinks the chassis by just enough to stay inside — exactly 1
   at either end, dipping ~20 % mid-turn for a near-square iPad and barely 3 %
   for a phone. It reads as the device tipping away as it turns.

2. **The picture goes down before the chassis moves.** By the time the pivot
   starts, the stream has *already* switched orientation, so the very next
   decoded frame is landscape content that would be letterboxed into a
   still-portrait panel and sliced by the diagonal panel edge. That was the
   "the screen rotates before the device does" flash. Now the old picture fades
   out over 110 ms and frames decoded during the turn are parked in
   `pending_` — decoded, referenced, but not shown — and released when the
   chassis settles, fading up over 170 ms. A 1.5 s timeout covers a stream that
   never comes back.

3. **The stream's shape is treated as a noisy signal.** See below — this is the
   one that mattered most.

### The stream lies to you around a turn

There are two independent sources for "what shape is the stream": the
`video_report_size` callback, which fires with the new SPS/PPS, and the
dimensions of each decoded frame. Around a rotation they disagree, repeatedly.
Frames encoded before the SPS change keep arriving at the old size for a few
tens of milliseconds after the report, and this iPad also spends a moment at a
different aspect before settling. Acting on each report in turn is what made
the picture flick back and forth before it settled — every stale frame started
its own counter-rotation.

So the shape is filtered rather than obeyed. A proposal has to hold steady for
`kConfirmSeconds` (180 ms) before the UI acts on it, and — crucially —
**agreement with what is already on screen cancels a pending change**, which is
what makes it a confirmation filter rather than a plain delay. One stale frame
is enough to veto a premature report. A flip additionally has to wait out
`kOrientHold` (500 ms) after the previous turn settled. Frames that don't match
the accepted shape are counted and dropped rather than drawn, so the panel
never shows a stretched or wrongly-letterboxed picture. `SameShape` compares
orientation plus aspect with a 1.5 % tolerance, so 1920×1080 and the
macroblock-padded 1920×1088 don't read as a change and fight each other.

Confirmation would normally mean 180 ms frozen on a stale frame, so the fade is
driven declaratively instead of by timers: anything meaning *what we hold is
not what the device is showing* — a proposed flip, a turn in progress, waiting
on new-orientation content — pulls the panel's target brightness to zero, and
it eases back up on its own when that clears. The fade therefore begins the
instant a flip is **proposed**, not when it is accepted; if the proposal turns
out to be a blip it is simply withdrawn and the panel comes back up.

It logs what it decided, which is the fastest way to see what a device actually
sends:

```
stream proposes 1440x1080 (showing 810x1080)
stream proposes 1440x1080 (showing 810x1080)     <- a stale 810x1080 frame vetoed it; re-armed
stream shape 1440x1080 accepted - rotating (5 frames held back)
```

The contact shadow's falloff reaches exactly zero (with zero slope) at
`shadowRadius` past the body edge, and the window margin is derived from that
same number — so the shadow can never be clipped into a hard line by the window
boundary, at any angle. Its tail then only changes by one 8-bit level per pixel,
which on a flat background still reads as a thin contour where the shadow
starts, so the alpha carries a ±½-level hash dither that puts the edge below the
eye's floor.

Because the window is always larger than the device, `WM_NCHITTEST` returns
`HTTRANSPARENT` outside the chassis: clicks pass through to whatever is behind,
and only the device itself is draggable.

One genuine protocol limitation: AirPlay reports only the stream's dimensions,
not *which* way the device was turned, so landscape-left and landscape-right are
indistinguishable. EngageMirror assumes the device's top edge went to the left —
press `L` if it went the other way (that animates too, as a 180° flip).

## Device frames

**Device artwork is not included in this repository.** Pictures of real
devices are generally not redistributable, so bring your own; builds bundle
whatever is in `assets/`. Without them every device gets the procedural frame.

| Device | File in `assets/` |
|---|---|
| Any iPad | `ipad_pro.png` |
| iPhone | `iphone_<model>.png`, e.g. `iphone_15.png`, `iphone_16_pro_max.png`. The full mapping is `PhoneSkin()` in [device_db.cpp](src/device_db.cpp) |

Each iPhone model maps to the frame whose screen has the same pixel
dimensions, so the cutout fits the stream exactly. `scripts/fetch-device-frames.py`
downloads matching iPhone frames from
[device-frames-media](https://github.com/jonnyjackson26/device-frames-media)
for your own builds. That collection states no licence, so check its terms
before distributing anything that contains them.

### Dropping in a new skin

Save a PNG with an **opaque device body and a fully transparent screen area**
into `assets/`, then point a profile at it (`p.skinName = "my_device";` for
`assets/my_device.png`). Nothing else needs measuring. [skin.cpp](src/skin.cpp)
derives everything from the alpha channel at load time:

- the body extent (where most rows and columns end, so side buttons don't
  count),
- the screen rectangle (the transparent region enclosed by the body on all four
  sides, which is what separates it from the transparent margin outside),
- the body and screen corner radii, measured along the 45° diagonal so
  continuous-curvature corners are matched. They clip the drop shadow and the
  video.

Author the PNG **portrait**; landscape is handled by rotating the texture
lookup. Make it comfortably larger than it will ever be drawn — the loader
builds a full mip chain, so a 2290×2960 source stays crisp at 900px.

Check the result without a device attached:

```
engagemirror.exe --device iPad13,4 --preview 2048x2732     # portrait
engagemirror.exe --device iPad13,4 --preview 2732x2048     # landscape
EngageMirror.app/Contents/MacOS/EngageMirror --device iPhone16,2 --uvtest
```

If the file is missing or has no transparent interior, the app logs a warning
and falls back to the procedural frame.

### Aspect handling with a skin

A skin fixes the screen's shape (an iPad Pro cutout is 2048×2732, exactly
3:4). When the incoming stream matches, it fills the cutout exactly.
When it does not — an iPad that decides to send 16:9 — the video is
**letterboxed inside the cutout against a black backing rather than stretched**,
so the picture is never distorted.

## Licensing and source code

**EngageMirror is GPLv3** (or later), because it links UxPlay's `libairplay`
(GPLv3). Anything you distribute that includes it must be GPLv3 too. See
[LICENSE](LICENSE).

| Component | Licence | Linked |
|---|---|---|
| EngageMirror (`src/`) | GPL-3.0-or-later | |
| UxPlay 1.74 `libairplay` | GPL-3.0 (parts LGPL-2.1+) | static |
| UxPlay `mdnsd` (Windows) / `dns_sd` (macOS) | LGPL-2.1+ | static |
| `playfair` (FairPlay handshake) | GPL-3.0, see `vendor/UxPlay/lib/playfair/LICENSE.md` | static |
| `llhttp` | MIT | static |
| FFmpeg 7.1.1 | LGPL-2.1+ (configured without `--enable-gpl`) | macOS static, Windows DLLs |
| OpenSSL 3.x (libcrypto) | Apache-2.0 | macOS static, Windows DLL |
| libplist | LGPL-2.1+ | macOS static, Windows DLL |

How this repository meets the licences:

- **Complete corresponding source.** UxPlay is vendored in
  [vendor/UxPlay](vendor/UxPlay), not a submodule, so every tag and every
  GitHub source archive is complete. The one modification is marked and dated
  in the file and recorded in [patches/](patches). Each release also carries
  the exact OpenSSL, libplist and FFmpeg source tarballs its binaries were
  built from, and the scripts that build them
  ([build-deps-macos.sh](scripts/build-deps-macos.sh) pins and checksums them).
- **Licence texts ship with the binaries.** The macOS app carries them in
  `Engage Mirror.app/Contents/Resources/licenses/`, together with
  [THIRD-PARTY-NOTICES.txt](macos/THIRD-PARTY-NOTICES.txt); the Windows
  installer includes `LICENSE`.
- **Legal notices in the app.** Right-click → About (and the macOS app menu)
  shows the copyright, the GPL, the absence of warranty and where to get the
  source (GPLv3 §5(d)).
- **LGPL relinking.** The LGPL libraries are linked statically on macOS.
  Because the whole program's source and build scripts are available, you can
  rebuild it against modified versions of them.
- **Signing does not lock you out.** The macOS release is signed and
  notarized, but macOS runs builds you sign yourself (or ad hoc), so there is
  no installation restriction on modified versions.

If you distribute builds yourself, give recipients the same: this source (at
the matching tag) and the third-party tarballs from the corresponding release.

*AirPlay, iPhone, iPad and Mac are trademarks of Apple Inc. EngageMirror is
not affiliated with or endorsed by Apple.*

## Troubleshooting

**The computer does not appear on the iPhone.** Both devices must be on the
same subnet. On macOS, Engage Mirror needs Local Network access (System
Settings → Privacy & Security → Local Network). On Windows, Windows Firewall
must allow `engagemirror.exe` on the *private* network profile. Verify the
advertisement is live:

```bash
dns-sd -B _airplay._tcp          # macOS
python scripts/mdns_probe.py     # Windows: should list _airplay._tcp -> YOURPC
```

**An iPad shows a 16:9 screen.** The iPad chose to send 16:9. Start with
`--size 1440x1080` to request 4:3. EngageMirror always renders the stream at its
true aspect rather than stretching it to the frame.

**Video is soft when the window is small.** Expected and handled — the shader
supersamples 2×2 when minifying by more than 1.25×. Scroll up to enlarge.

**`libairplay: Accepted ... ` then nothing.** Another AirPlay receiver
(UxPlay, AirServer, Reflector) may hold the ports. Close it and restart.
