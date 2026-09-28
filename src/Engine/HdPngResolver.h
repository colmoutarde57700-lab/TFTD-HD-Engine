#pragma once

#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace OpenXcom
{

// Asset selection only: no Surface, PCK pixels, framebuffer or gameplay state.
// The existence predicate is supplied by FileMap at runtime and by tests offline.
struct HdPngAsset
{
    std::string path;
    int nativeScale = 16;
};

inline std::string hdPngFrameNumber(int frame, int width)
{
    std::ostringstream out;
    out << std::setw(width) << std::setfill('0') << frame;
    return out.str();
}

template<typename Exists>
HdPngAsset hdResolveUnitPartPng(const Exists &exists, const std::string &configuredRoot,
    int configuredScale, std::string dataset, int frame)
{
    if (frame < 0) return {};
    const auto dot = dataset.find_last_of('.');
    if (dot != std::string::npos) dataset.erase(dot);
    if (dataset.empty()) return {};
    const std::string legacyRoot = "Resources/TFTD_HD/LegacyIndexed/UnitParts";
    const bool configuredLegacy = configuredRoot.find("Resources/TFTD_HD/LegacyIndexed/") == 0;
    std::vector<HdPngAsset> roots;
    if (!configuredRoot.empty() && !configuredLegacy)
        roots.push_back({configuredRoot, configuredScale});
    roots.push_back({"Resources/TFTD_HD/Fixed/UnitParts", 16});
    roots.push_back({"Resources/TFTD_HD/UnitParts", 16});
    if (!configuredRoot.empty() && configuredLegacy)
        roots.push_back({configuredRoot, configuredScale});
    roots.push_back({legacyRoot, 16});
    for (const auto &root : roots)
    {
        const std::string path = root.path + "/" + dataset + "/" + hdPngFrameNumber(frame, 4) + ".png";
        if (exists(path)) return {path, root.nativeScale};
    }
    return {};
}

template<typename Exists>
HdPngAsset hdResolveSurfacePng(const Exists &exists, const std::string &setName, int frame)
{
    if (frame < 0 || setName.empty()) return {};
    // Resolve all spellings within one provider before trying the lower-priority
    // provider. Old 3-digit assets and the complete 4-digit library coexist.
    for (const std::string &root : {std::string("Resources/TFTD_HD/RealHD/SurfaceSets/"),
        std::string("Resources/TFTD_HD/Fixed/SurfaceSets/"),
        std::string("Resources/TFTD_HD/SurfaceSets/"),
        std::string("Resources/TFTD_HD/LegacyIndexed/SurfaceSets/")})
    {
        for (const int width : {3, 4, 1})
        {
            const std::string path = root + setName + "/" + hdPngFrameNumber(frame, width) + ".png";
            if (exists(path)) return {path, 16};
        }
    }
    return {};
}

}
