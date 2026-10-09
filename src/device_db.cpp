#include "device_db.h"
#include "common.h"

#include <cstdlib>
#include <cstring>

namespace {

// Parse "iPhone15,2" -> family=iPhone, major=15, minor=2
struct ModelId {
    Family family = Family::Unknown;
    int major = 0;
    int minor = 0;
};

ModelId ParseModel(const std::string &m) {
    ModelId id;
    const char *prefixes[] = {"iPhone", "iPad", "iPod", "Mac"};
    const Family fams[] = {Family::iPhone, Family::iPad, Family::iPod, Family::Mac};

    size_t off = 0;
    for (int i = 0; i < 4; i++) {
        size_t n = strlen(prefixes[i]);
        if (m.size() >= n && _strnicmp(m.c_str(), prefixes[i], (int)n) == 0) {
            id.family = fams[i];
            off = n;
            break;
        }
    }
    if (id.family == Family::Unknown) return id;

    // Mac identifiers look like "MacBookPro18,3" - skip to the first digit.
    while (off < m.size() && (m[off] < '0' || m[off] > '9')) off++;
    if (off >= m.size()) return id;

    id.major = atoi(m.c_str() + off);
    size_t comma = m.find(',', off);
    if (comma != std::string::npos) id.minor = atoi(m.c_str() + comma + 1);
    return id;
}

// ---- style presets --------------------------------------------------------

DeviceProfile PhoneDynamicIsland() {
    DeviceProfile p;
    p.family = Family::iPhone;
    p.island = Island::DynamicIsland;
    p.homeButton = false;
    p.defaultAspect = 9.0f / 19.5f;
    p.bezelSide = p.bezelTop = p.bezelBottom = 0.036f;
    p.screenRadius = 0.118f;
    p.bodyRadiusPad = 0.018f;
    p.islandWidth = 0.315f;
    p.islandHeight = 0.093f;
    p.islandInset = 0.028f;
    p.bodyColor = 0xFF1F1F22;
    p.railColor = 0xFF4A4A50;
    return p;
}

DeviceProfile PhoneNotch() {
    DeviceProfile p = PhoneDynamicIsland();
    p.island = Island::Notch;
    p.bezelSide = p.bezelTop = p.bezelBottom = 0.042f;
    p.screenRadius = 0.108f;
    p.islandWidth = 0.46f;   // notch is wide and hangs off the top edge
    p.islandHeight = 0.077f;
    p.islandInset = 0.0f;
    p.bodyColor = 0xFF1C1C1E;
    p.railColor = 0xFF43434A;
    return p;
}

DeviceProfile PhoneHomeButton() {
    DeviceProfile p;
    p.family = Family::iPhone;
    p.island = Island::None;
    p.homeButton = true;
    p.defaultAspect = 9.0f / 16.0f;
    p.bezelSide = 0.055f;
    p.bezelTop = 0.175f;
    p.bezelBottom = 0.205f;
    p.screenRadius = 0.0f;
    p.bodyRadiusPad = 0.055f;
    p.homeButtonRadius = 0.075f;
    p.earpieceWidth = 0.175f;
    p.earpieceHeight = 0.020f;
    p.bodyColor = 0xFF202024;
    p.railColor = 0xFF4A4A50;
    return p;
}

DeviceProfile PadModern() {
    DeviceProfile p;
    p.family = Family::iPad;
    p.island = Island::None;
    p.homeButton = false;
    p.defaultAspect = 1668.0f / 2388.0f; // 11" iPad Pro
    p.bezelSide = p.bezelTop = p.bezelBottom = 0.052f;
    p.screenRadius = 0.032f;
    p.bodyRadiusPad = 0.022f;
    p.bodyColor = 0xFF25252A;
    p.railColor = 0xFF4F4F57;
    return p;
}

DeviceProfile PadHomeButton() {
    DeviceProfile p;
    p.family = Family::iPad;
    p.island = Island::None;
    p.homeButton = true;
    p.defaultAspect = 3.0f / 4.0f;
    p.bezelSide = 0.072f;
    p.bezelTop = 0.115f;
    p.bezelBottom = 0.115f;
    p.screenRadius = 0.0f;
    p.bodyRadiusPad = 0.055f;
    p.homeButtonRadius = 0.055f;
    p.earpieceWidth = 0.0f; // iPads have no earpiece slot
    p.bodyColor = 0xFF232328;
    p.railColor = 0xFF4F4F57;
    return p;
}

DeviceProfile MacLike() {
    DeviceProfile p;
    p.family = Family::Mac;
    p.island = Island::None;
    p.homeButton = false;
    p.defaultAspect = 16.0f / 10.0f;
    p.bezelSide = 0.012f;
    p.bezelTop = 0.012f;
    p.bezelBottom = 0.030f;
    p.screenRadius = 0.012f;
    p.bodyRadiusPad = 0.006f;
    p.bodyColor = 0xFF1A1A1C;
    p.railColor = 0xFF444448;
    return p;
}

// ---- marketing names ------------------------------------------------------

const char *PhoneName(int major, int minor) {
    switch (major) {
    case 10:
        return (minor == 3 || minor == 6) ? "iPhone X" : "iPhone 8";
    case 11: return "iPhone XS / XR";
    case 12:
        if (minor == 8) return "iPhone SE (2nd gen)";
        return "iPhone 11";
    case 13: return "iPhone 12";
    case 14:
        if (minor == 6) return "iPhone SE (3rd gen)";
        if (minor == 2 || minor == 3) return "iPhone 13 Pro";
        if (minor == 7 || minor == 8) return "iPhone 14";
        return "iPhone 13";
    case 15:
        if (minor == 2 || minor == 3) return "iPhone 14 Pro";
        return "iPhone 15";
    case 16:
        if (minor == 1 || minor == 2) return "iPhone 15 Pro";
        return "iPhone 16";
    case 17: return "iPhone 16";
    case 18: return "iPhone 17";
    default: break;
    }
    return major >= 19 ? "iPhone" : "iPhone (older)";
}

const char *PadName(int major) {
    if (major >= 16) return "iPad Pro (M4)";
    if (major >= 14) return "iPad";
    if (major >= 13) return "iPad Air / Pro";
    if (major >= 8) return "iPad Pro";
    return "iPad";
}

} // namespace

DeviceProfile ResolveDevice(const std::string &model, const std::string &name) {
    ModelId id = ParseModel(model);
    DeviceProfile p;

    switch (id.family) {
    case Family::iPhone: {
        const bool isSE = (id.major == 12 && id.minor == 8) ||
                          (id.major == 14 && id.minor == 6);
        const bool isX = (id.major == 10 && (id.minor == 3 || id.minor == 6));

        if (id.major >= 15) {
            p = PhoneDynamicIsland();
        } else if ((id.major >= 11 && id.major <= 14 && !isSE) || isX) {
            p = PhoneNotch();
            // The 12/13/14 generation shrank the notch.
            if (id.major >= 13) {
                p.islandWidth = 0.42f;
                p.islandHeight = 0.072f;
            }
        } else {
            p = PhoneHomeButton();
            if (id.major > 0 && id.major <= 7) p.defaultAspect = 9.0f / 16.0f;
        }
        p.displayName = PhoneName(id.major, id.minor);
        break;
    }
    case Family::iPad: {
        // Home button survived on iPad 7/8/9 (iPad7,x / iPad11,x / iPad12,x)
        // and everything older. iPad8,x (2018 Pro) onward went edge-to-edge.
        const bool home = (id.major > 0 && id.major <= 7) || id.major == 11 ||
                          id.major == 12;
        p = home ? PadHomeButton() : PadModern();
        if (!home && id.major >= 13) p.screenRadius = 0.036f;
        p.displayName = PadName(id.major);
        // Every iPad uses the photographic frame; the procedural one never
        // looked convincing at tablet proportions.
        p.skinName = "ipad_pro";
        break;
    }
    case Family::iPod:
        p = PhoneHomeButton();
        p.displayName = "iPod touch";
        break;
    case Family::Mac:
        p = MacLike();
        p.displayName = "Mac";
        break;
    default:
        // Unknown client: a modern iPhone is the safest-looking default.
        p = PhoneDynamicIsland();
        p.displayName = name.empty() ? "AirPlay device" : name;
        break;
    }

    p.modelId = model;
    if (!name.empty() && id.family != Family::Unknown) {
        // Prefer the user-visible device name ("Barathi's iPhone") but keep
        // the marketing name as a suffix when they differ meaningfully.
        p.displayName = name;
    }
    return p;
}
