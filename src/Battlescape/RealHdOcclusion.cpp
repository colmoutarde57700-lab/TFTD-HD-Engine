#include "RealHdOcclusion.h"

#include <algorithm>
#include <cstdlib>

#include "../Mod/MapData.h"
#include "../Mod/Mod.h"
#include "../Savegame/BattleUnit.h"
#include "../Savegame/SavedBattleGame.h"
#include "../Savegame/Tile.h"

namespace OpenXcom
{
Position RealHdOcclusion::eye(const SavedBattleGame *save, const BattleUnit *unit)
{
	if (!save || !unit) return Position();
	const Position tilePos = unit->getPosition();
	const Tile *tile = save->getTile(tilePos);
	if (!tile) return Position();
	Position result = tilePos.toVoxel() + Position(8, 8,
		-tile->getTerrainLevel() + unit->getHeight() + unit->getFloatHeight() - 1);
	if (unit->isBigUnit()) result += Position(8, 8, 1);
	const Tile *above = save->getTile(tilePos + Position(0, 0, 1));
	if (result.z >= (tilePos.z + 1) * Position::TileZ && (!above || !above->hasNoFloor(save)))
		result.z = (tilePos.z + 1) * Position::TileZ - 1;
	return result;
}

bool RealHdOcclusion::terrainVoxelSolid(const SavedBattleGame *save, const Position &voxel)
{
	if (!save || voxel.x < 0 || voxel.y < 0 || voxel.z < 0) return true;
	const Tile *tile = save->getTile(voxel.toTile());
	if (!tile) return true;
	const Mod *mod = save->getMod();
	const std::vector<Uint16> *loft = mod ? mod->getVoxelData() : nullptr;
	if (!loft) return true;
	for (int part = O_FLOOR; part <= O_OBJECT; ++part)
	{
		const TilePart kind = static_cast<TilePart>(part);
		if ((kind == O_WESTWALL || kind == O_NORTHWALL) && tile->isUfoDoorOpen(kind)) continue;
		const MapData *data = tile->getMapData(kind);
		if (!data) continue;
		const int loftId = data->getLoftID((voxel.z % Position::TileZ) / 2);
		const size_t index = static_cast<size_t>(loftId) * 16u + static_cast<size_t>(voxel.y % Position::TileXY);
		if (loftId < 0 || index >= loft->size()) continue;
		const int bit = Position::TileXY - 1 - voxel.x % Position::TileXY;
		if (((*loft)[index] & (1u << bit)) != 0) return true;
	}
	return false;
}

bool RealHdOcclusion::terrainRayClear(const SavedBattleGame *save, const Position &from,
	const Position &to)
{
	if (!save) return false;
	Position a = from, b = to;
	bool swapXY = std::abs(b.y - a.y) > std::abs(b.x - a.x);
	if (swapXY) { std::swap(a.x, a.y); std::swap(b.x, b.y); }
	bool swapXZ = std::abs(b.z - a.z) > std::abs(b.x - a.x);
	if (swapXZ) { std::swap(a.x, a.z); std::swap(b.x, b.z); }
	const int dx = std::abs(b.x - a.x), dy = std::abs(b.y - a.y), dz = std::abs(b.z - a.z);
	const int stepX = a.x <= b.x ? 1 : -1;
	const int stepY = a.y <= b.y ? 1 : -1;
	const int stepZ = a.z <= b.z ? 1 : -1;
	int y = a.y, z = a.z, driftXY = dx / 2, driftXZ = dx / 2;
	for (int x = a.x;; x += stepX)
	{
		Position point(x, y, z);
		if (swapXZ) std::swap(point.x, point.z);
		if (swapXY) std::swap(point.x, point.y);
		if (point != from && point != to && terrainVoxelSolid(save, point)) return false;
		if (x == b.x) break;
		driftXY -= dy;
		driftXZ -= dz;
		if (driftXY < 0)
		{
			y += stepY;
			driftXY += dx;
			Position diagonal(x, y, z);
			if (swapXZ) std::swap(diagonal.x, diagonal.z);
			if (swapXY) std::swap(diagonal.x, diagonal.y);
			if (diagonal != from && diagonal != to && terrainVoxelSolid(save, diagonal)) return false;
		}
		if (driftXZ < 0)
		{
			z += stepZ;
			driftXZ += dx;
			Position diagonal(x, y, z);
			if (swapXZ) std::swap(diagonal.x, diagonal.z);
			if (swapXY) std::swap(diagonal.x, diagonal.y);
			if (diagonal != from && diagonal != to && terrainVoxelSolid(save, diagonal)) return false;
		}
	}
	return true;
}
}
