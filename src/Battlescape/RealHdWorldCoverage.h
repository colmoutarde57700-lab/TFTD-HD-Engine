#pragma once

#include <SDL.h>
#include <vector>

#include "../Mod/MapData.h"
#include "../Engine/HdGpuBackend.h"

namespace OpenXcom
{

class Camera;
class Tile;

/**
 * Semantic world-coverage builder for the Real HD Battlescape compositor.
 *
 * This deliberately consumes OXCE's voxel/LOFT geometry rather than the
 * Legacy screen painter order.  OXCE remains authoritative for physical world
 * shape; D3D11 owns the resulting pixel/fragment depth representation.
 *
 * The generated mesh is depth-only. It never decides colour/material and it
 * never attempts to infer geometry from a PCK/PNG silhouette.
 */
class RealHdWorldCoverage
{
public:
	/// Append the camera-facing exterior of one terrain TilePart to a depth mesh.
	/// Returns the number of emitted quads.
	static unsigned appendTerrainPart(const Camera *camera, const Tile *tile, TilePart part,
		const std::vector<Uint16> &voxelData, std::vector<HdGpuWorldCoverageVertex> &vertices);
};

}
