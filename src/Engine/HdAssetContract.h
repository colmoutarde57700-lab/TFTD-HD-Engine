#pragma once

#include <string>
#include <vector>

namespace OpenXcom
{

// A provider describes an image's provenance, never its file encoding.
// An indexed PNG from LegacyHD is an HD resource, not a Legacy renderer.
enum class HdAssetProvider
{
    RealHd,
    Remastered,
    LegacyHd
};

enum class HdAssetDomain
{
    Terrain,
    UnitPart,
    Equipment,
    Effect,
    Interface,
    Font,
    Cinematic
};

struct HdAssetKey
{
    HdAssetDomain domain = HdAssetDomain::Interface;
    std::string family;
    int frame = 0;
};

struct HdAssetCandidate
{
    HdAssetProvider provider = HdAssetProvider::Remastered;
    std::string path;
    int nativeScale = 16;
};

enum class HdAssetAvailability
{
    Absent,
    Ready,
    Invalid
};

enum class HdAssetFailure
{
    None,
    Missing,
    InvalidResource,
    InvalidRequest
};

struct HdAssetResolution
{
    HdAssetKey key;
    HdAssetCandidate asset;
    HdAssetFailure failure = HdAssetFailure::Missing;
    std::vector<HdAssetCandidate> attempted;
    std::vector<HdAssetCandidate> invalid;

    explicit operator bool() const { return failure == HdAssetFailure::None; }
};

// The caller constructs the ordered list from active mods only. The probe must
// distinguish an absent resource from one that exists but cannot be decoded.
// Invalid replacements remain in the result's diagnostic trail while lower
// HD providers are attempted. The caller must report that trail.
// There is deliberately no callback for drawing native sprites or a framebuffer.
template<typename Probe>
HdAssetResolution hdResolveAsset(const HdAssetKey &key,
    const std::vector<HdAssetCandidate> &candidates, const Probe &probe)
{
    HdAssetResolution result;
    result.key = key;
    if (key.family.empty() || key.frame < 0)
    {
        result.failure = HdAssetFailure::InvalidRequest;
        return result;
    }
    for (const auto &candidate : candidates)
    {
        if (candidate.path.empty() || candidate.nativeScale <= 0)
        {
            result.asset = candidate;
            result.failure = HdAssetFailure::InvalidRequest;
            return result;
        }
        result.attempted.push_back(candidate);
        const auto availability = probe(candidate.path);
        if (availability == HdAssetAvailability::Absent) continue;
        if (availability == HdAssetAvailability::Invalid)
        {
            result.invalid.push_back(candidate);
            result.failure = HdAssetFailure::InvalidResource;
            continue;
        }
        result.asset = candidate;
        result.failure = availability == HdAssetAvailability::Ready
            ? HdAssetFailure::None : HdAssetFailure::InvalidResource;
        return result;
    }
    return result;
}

inline const char *hdAssetProviderName(HdAssetProvider provider)
{
    switch (provider)
    {
    case HdAssetProvider::RealHd: return "REAL_HD";
    case HdAssetProvider::Remastered: return "PNG_REMASTERED";
    case HdAssetProvider::LegacyHd: return "PNG_LEGACY_HD";
    }
    return "UNKNOWN";
}

}
