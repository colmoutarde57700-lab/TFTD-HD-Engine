/*
 * TFTD HD Geoscape mode ownership helper.
 *
 * This is intentionally a MODE predicate, not a per-frame GPU-readiness test.
 * When the authored HD Geoscape provider is installed and HD graphics are
 * enabled, HD code owns globe CAMERA INPUT continuously. A transient GPU frame
 * fallback must never wake the Legacy camera handlers back up underneath it.
 *
 * If the HD provider is absent or HD graphics are disabled, callers fall back
 * to the untouched Legacy OpenXcom/OXCE behaviour.
 */
#ifndef OPENXCOM_GEOSCAPEHDMODE_H
#define OPENXCOM_GEOSCAPEHDMODE_H

#include "../Engine/Options.h"
#include "../Engine/FileMap.h"

namespace OpenXcom
{

// Presentation-only emergency scope. Camera/input ownership is unchanged.
inline bool &geoscapeNativePresentationRequested()
{
    static thread_local bool enabled = false;
    return enabled;
}
class GeoscapeNativePresentationScope
{
    bool previous;
public:
    GeoscapeNativePresentationScope() : previous(geoscapeNativePresentationRequested())
        { geoscapeNativePresentationRequested() = true; }
    ~GeoscapeNativePresentationScope() { geoscapeNativePresentationRequested() = previous; }
};

inline const char *geoscapeHdEarthMeshPath()
{
    return "Resources/TFTD_HD/RealHD/Geoscape/earth_terrain_and_sea_map.hdmesh";
}

inline bool geoscapeHdModeActive()
{
    return Options::hdGraphics && FileMap::fileExists(geoscapeHdEarthMeshPath());
}

}

#endif
