// AirMirror - maps an AirPlay client's model identifier to a device frame style.
//
// Design note: geometry that must be *exact* (the screen's aspect ratio) is
// always taken from the live video stream, never from this table. The table
// only supplies styling - bezel thickness, corner rounding, notch vs Dynamic
// Island, home button, body colour - so an unrecognised device still renders
// with a correct-aspect screen inside a plausible frame.
#pragma once

#include <cstdint>
#include <string>

enum class Family { Unknown, iPhone, iPad, iPod, Mac };

enum class Island {
    None,          // no cutout (home-button era, iPads)
    Notch,         // iPhone X .. 14 / 14 Plus
    DynamicIsland, // iPhone 14 Pro and later
};

struct DeviceProfile {
    std::string displayName = "iPhone";
    std::string modelId;
    Family family = Family::iPhone;
    Island island = Island::DynamicIsland;
    bool homeButton = false;

    // Fallback portrait aspect (width / height) used only until the first
    // video frame tells us the real one.
    float defaultAspect = 9.0f / 19.5f;

    // All ratios below are relative to the screen's SHORT edge (the screen
    // width when the device is held in portrait).
    float bezelSide = 0.038f;
    float bezelTop = 0.038f;
    float bezelBottom = 0.038f;
    float screenRadius = 0.115f; // screen corner radius
    float bodyRadiusPad = 0.02f; // body radius = screenRadius + bezel + this

    // Cutout metrics (ratios of screen short edge). Ignored when island==None.
    float islandWidth = 0.315f;
    float islandHeight = 0.093f;
    float islandInset = 0.028f; // gap between screen top edge and cutout

    // Home-button era hardware.
    float homeButtonRadius = 0.145f; // of screen short edge
    float earpieceWidth = 0.175f;
    float earpieceHeight = 0.021f;

    uint32_t bodyColor = 0xFF1C1C1E;   // frame / bezel fill (ARGB)
    uint32_t railColor = 0xFF3A3A3C;   // outer rim highlight

    // When set, "assets/<skinName>.png" is used instead of the procedural
    // frame. Falls back to procedural if the file is missing.
    std::string skinName;
};

// model may be empty or unrecognised; streamAspect (w/h, portrait-normalised)
// may be <= 0 if not yet known. Never fails - always returns something sane.
DeviceProfile ResolveDevice(const std::string &model, const std::string &name);
