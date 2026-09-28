#pragma once

#include "Position.h"

namespace OpenXcom
{
class SavedBattleGame;
class BattleUnit;

// Geometry query for the remaster. It reads raw tactical voxel data but never
// calls TileEngine LOS/FOV, Surface, the Legacy painter or its order buffer.
class RealHdOcclusion
{
public:
	static Position eye(const SavedBattleGame *save, const BattleUnit *unit);
	static bool terrainVoxelSolid(const SavedBattleGame *save, const Position &voxel);
	static bool terrainRayClear(const SavedBattleGame *save, const Position &from,
		const Position &to);
};
}
