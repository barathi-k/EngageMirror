// EngageMirror - PNG device skins.
//
// A skin is a device photo/render whose screen area is fully transparent. The
// loader works out the screen rectangle from the alpha channel itself, so
// dropping in a new PNG needs no hand-measured metadata: any image with an
// opaque body and a transparent hole in the middle just works.
#pragma once

#include "common.h"
#include "gpu.h"

#include <string>

struct Skin {
    bool valid = false;
    std::string name;

    int imgW = 0, imgH = 0;

    // Opaque body extent, in image pixels.
    int bodyX0 = 0, bodyY0 = 0, bodyX1 = 0, bodyY1 = 0;
    // Transparent screen cutout, in image pixels.
    int scrX0 = 0, scrY0 = 0, scrX1 = 0, scrY1 = 0;
    // Body corner radius in image pixels (used for the drop shadow).
    float cornerRadius = 0.0f;
    // Screen cutout corner radius in image pixels: the dark backing and the
    // video are clipped to it, so neither shows past a thin bezel's corner.
    float screenRadius = 0.0f;

    TexView srv;

    int ScreenW() const { return scrX1 - scrX0 + 1; }
    int ScreenH() const { return scrY1 - scrY0 + 1; }
    float ScreenAspect() const {
        return ScreenH() ? (float)ScreenW() / (float)ScreenH() : 1.0f;
    }
};

// Looks for "<name>.png" in the assets folder (beside the exe on Windows, in
// the bundle's Resources on macOS), then ./assets.
// Returns false and leaves skin.valid == false when the file is missing or
// has no transparent interior; callers fall back to the procedural frame.
bool LoadSkin(Gpu &gpu, const std::string &name, Skin &out);
