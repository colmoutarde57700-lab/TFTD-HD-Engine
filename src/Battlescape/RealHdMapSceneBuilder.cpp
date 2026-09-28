#include "RealHdMapSceneBuilder.h"

#include <cstdint>
#include <string>
#include <vector>

#include "RealHdPhysicalGeometry.h"
#include "../Mod/MapData.h"
#include "../Mod/MapDataSet.h"
#include "../Mod/Mod.h"
#include "../Savegame/SavedBattleGame.h"
#include "../Savegame/Tile.h"

namespace OpenXcom
{
namespace
{
constexpr std::uint8_t SOLID_USES =
	BlockSight | BlockLight | BlockProjectiles | BlockMovement;

void addDiagonal(RealHdTilePhysics &physics, int orientation)
{
	const double x = physics.x * 16.0;
	const double y = physics.y * 16.0;
	const double z = physics.z * 24.0;
	const bool neSw = orientation == 2;
	const RealHdPoint3 lowerA{x, y + (neSw ? 16.0 : 0.0), z};
	const RealHdPoint3 lowerB{x + 16.0, y + (neSw ? 0.0 : 16.0), z};
	const RealHdPoint3 upperA{lowerA.x, lowerA.y, z + 24.0};
	const RealHdPoint3 upperB{lowerB.x, lowerB.y, z + 24.0};
	physics.barriers.push_back({lowerA, lowerB, upperA,
		RealHdBarrierKind::DiagonalWall, SOLID_USES});
	physics.barriers.push_back({lowerB, upperB, upperA,
		RealHdBarrierKind::DiagonalWall, SOLID_USES});
}

void addLoft(const MapData *data, TilePart kind, const std::vector<Uint16> &loft,
	RealHdTilePhysics &physics)
{
	// A real floor closes the vertical aperture even when the historical MCD
	// Stop_LOS flag is unset. Other parts use the resolved MCD sight semantic.
	const bool blocksSight = kind == O_FLOOR || data->getBlock(DT_NONE) > 0;
	if (blocksSight && physics.sightVoxelRows.empty())
		physics.sightVoxelRows.resize(24 * 16, 0);
	for (int localZ = 0; localZ < 24; ++localZ)
	{
		const int loftId = data->getLoftID(localZ / 2);
		if (loftId < 0) continue;
		const std::size_t base = std::size_t(loftId) * 16;
		if (base + 16 > loft.size()) continue;
		for (int localY = 0; localY < 16; ++localY)
		{
			const std::size_t row = std::size_t(localZ) * 16 + localY;
			physics.solidVoxelRows[row] |= loft[base + localY];
			if (blocksSight) physics.sightVoxelRows[row] |= loft[base + localY];
		}
		physics.hasVoxelVolume = true;
	}
}
}

bool RealHdMapSceneBuilder::build(const SavedBattleGame *save,
	RealHdPhysicalGeometry &scene)
{
	scene.clear();
	if (!save || !save->getMod()) return false;
	const std::vector<Uint16> *loft = save->getMod()->getVoxelData();
	if (!loft) return false;
	const int width = save->getMapSizeX();
	const int height = save->getMapSizeY();
	const int levels = save->getMapSizeZ();
	if (width <= 0 || height <= 0 || levels <= 0) return false;
	for (int z = 0; z < levels; ++z)
		for (int y = 0; y < height; ++y)
			for (int x = 0; x < width; ++x)
			{
			const Tile *tile = save->getTile(Position(x, y, z));
			if (!tile) { scene.clear(); return false; }
			RealHdTilePhysics physics;
			physics.x = x;
			physics.y = y;
			physics.z = z;
			physics.voxelUses = SOLID_USES;
			const MapData *object = tile->getMapData(O_OBJECT);
			const int bigWall = object ? object->getBigWall() : 0;
			physics.forbidsStanding = bigWall >= 1 && bigWall <= 3;
			const MapData *floor = tile->getMapData(O_FLOOR);
			if (floor && !floor->isNoFloor())
			{
				RealHdSupportSurface support;
				const MapDataSet *dataset = floor->getDataset();
				support.surfaceId = dataset ? dataset->getName() + ":" +
					std::to_string(floor->getDatasetIndex()) : "UNKNOWN_FLOOR";
				support.height = z * 24.0 - floor->getTerrainLevel();
				support.supportsWalking = floor->getTUCost(MT_WALK) < 255;
				physics.supports.push_back(std::move(support));
			}
			for (int part = O_FLOOR; part < O_MAX; ++part)
			{
				const TilePart kind = static_cast<TilePart>(part);
				const MapData *data = tile->getMapData(kind);
				if (!data) continue;
				// An explicitly absent upper floor leaves the vertical sight
				// aperture open. Never retain a stale floor LOFT as an occluder.
				if (kind == O_FLOOR && data->isNoFloor()) continue;
				if (kind == O_OBJECT && (bigWall == 2 || bigWall == 3))
				{
					// The MCD diagonal is the physical wall. Its LOFT is often a
					// broad legacy proxy and must not fill the clear half of this cell.
					addDiagonal(physics, bigWall);
					continue;
				}
				if ((kind == O_NORTHWALL || kind == O_WESTWALL)
					&& tile->isUfoDoorOpen(kind)) continue;
				addLoft(data, kind, *loft, physics);
			}
			scene.addTile(std::move(physics));
			}
	return scene.seal(width, height, levels);
}
}
