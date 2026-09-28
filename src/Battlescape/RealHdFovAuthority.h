#pragma once

namespace OpenXcom
{
class SavedBattleGame;
class BattleUnit;
class Tile;
class RealHdPhysicalGeometry;

// REAL HD decides sight from raw tactical geometry and its own light field.
// The game can publish these results in its existing visibility containers,
// but TileEngine LOS and the old renderer are never consulted here.
class RealHdFovAuthority
{
public:
	static bool sectorContains(const BattleUnit *observer, const Tile *target,
		bool useTurretDirection);
	static bool terrainVisible(const SavedBattleGame *save,
		const RealHdPhysicalGeometry &geometry, const BattleUnit *observer,
		const Tile *target, int maxRange, bool useTurretDirection);
	static bool unitVisible(SavedBattleGame *save,
		const RealHdPhysicalGeometry &geometry, BattleUnit *observer,
		Tile *target, int maxRange, int maxDarkness);
};
}
