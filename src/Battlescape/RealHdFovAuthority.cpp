#include "RealHdFovAuthority.h"

#include <algorithm>
#include <cmath>

#include "RealHdPhysicalGeometry.h"
#include "RealHdLightingAuthority.h"
#include "RealHdPerceptionAuthority.h"
#include "../Mod/Armor.h"
#include "../Savegame/BattleUnit.h"
#include "../Savegame/SavedBattleGame.h"
#include "../Savegame/Tile.h"

namespace OpenXcom
{
namespace
{
RealHdPoint3 point(const Position &p)
{
	return {double(p.x), double(p.y), double(p.z)};
}

bool hasRealHdFloor(const RealHdPhysicalGeometry &geometry, int x, int y, int z)
{
	const RealHdTilePhysics *cell = geometry.tile(x, y, z);
	return cell && !cell->supports.empty();
}

Position eye(const SavedBattleGame *save, const RealHdPhysicalGeometry &geometry,
	const BattleUnit *unit)
{
	const Position p = unit->getPosition();
	const Tile *tile = save->getTile(p);
	if (!tile) return Position();
	Position result = p.toVoxel() + Position(8, 8,
		-tile->getTerrainLevel() + unit->getHeight() + unit->getFloatHeight() - 1);
	if (unit->isBigUnit()) result += Position(8, 8, 1);
	if (result.z >= (p.z + 1) * Position::TileZ &&
		hasRealHdFloor(geometry, p.x, p.y, p.z + 1))
		result.z = (p.z + 1) * Position::TileZ - 1;
	return result;
}

bool targetIsDark(SavedBattleGame *save,
	const RealHdPhysicalGeometry &geometry, const Tile *target,
	int maxDarkness)
{
	if (!target) return true;
	const int semanticLevel = std::max(target->getLight(LL_AMBIENT),
		std::max(target->getLight(LL_FIRE), target->getLight(LL_ITEMS)));
	float light = std::clamp(semanticLevel / 15.0f, 0.0f, 1.0f);
	if (save->getTogglePersonalLight())
		for (const BattleUnit *source : *save->getUnits())
		{
			light += RealHdLightingAuthority::helmetLightSampleAt(
				save, save->getTileEngine(),
				source, target->getPosition(), &geometry).intensity;
			if (light >= 1.0f) break;
		}
	const float required = std::clamp((15.0f - maxDarkness) / 15.0f,
		0.0f, 1.0f);
	return light + 1e-6f < required;
}
}

bool RealHdFovAuthority::sectorContains(const BattleUnit *observer, const Tile *target,
	bool useTurretDirection)
{
	return observer && target &&
		RealHdPerceptionAuthority::contains(observer, target->getPosition(), useTurretDirection);
}

bool RealHdFovAuthority::terrainVisible(const SavedBattleGame *save,
	const RealHdPhysicalGeometry &geometry,
	const BattleUnit *observer, const Tile *target, int maxRange, bool useTurretDirection)
{
	if (!save || !observer || !target || observer->isOut()) return false;
	if (!sectorContains(observer, target, useTurretDirection)) return false;
	if (Position::distance2dSq(observer->getPosition(), target->getPosition()) > maxRange * maxRange)
		return false;
	const Position observerEye = eye(save, geometry, observer);
	const Position p = target->getPosition();
	const Position center = p.toVoxel() + Position(8, 8,
		-target->getTerrainLevel() +
		(hasRealHdFloor(geometry, p.x, p.y, p.z) ? 2 : 12));
	const RealHdPoint3 source = point(observerEye);
	if (geometry.rayClear(source, point(center), BlockSight)) return true;
	// One blocked centre ray cannot make a complete diamond disappear behind
	// a coral branch or other perforated geometry. Probe the interior of the
	// same target tile; all samples still obey the physical sight channel.
	for (const Position offset : {Position(-5, -5, 0), Position(5, -5, 0),
		Position(-5, 5, 0), Position(5, 5, 0)})
		if (geometry.rayClear(source, point(center + offset), BlockSight))
			return true;
	// Discovery describes the visible tile volume, not only its floor skin.
	// From below an upstairs landing, all five low samples can hit the lip of
	// that landing even though the air immediately above it is in clear LOS.
	// Probe its interior at half tile height as well. These are ordinary rays:
	// intervening floors, walls and closed doors still block them. Unit detection
	// retains its separate body, range and lighting tests below.
	if (p.z > observer->getPosition().z && hasRealHdFloor(geometry, p.x, p.y, p.z))
	{
		const Position interior = center + Position(0, 0, 10);
		for (const Position offset : {Position(0, 0, 0), Position(-5, -5, 0),
			Position(5, -5, 0), Position(-5, 5, 0), Position(5, 5, 0)})
			if (geometry.rayClear(source, point(interior + offset), BlockSight))
				return true;
	}
	return false;
}

bool RealHdFovAuthority::unitVisible(SavedBattleGame *save,
	const RealHdPhysicalGeometry &geometry, BattleUnit *observer,
	Tile *target, int maxRange, int maxDarkness)
{
	if (!save || !observer || !target || !target->getUnit() || observer->isOut()) return false;
	BattleUnit *other = target->getUnit();
	if (other->isOut()) return false;
	if (!sectorContains(observer, target, false)) return false;
	const int distanceSq = Position::distance2dSq(observer->getPosition(), target->getPosition());
	if (distanceSq > maxRange * maxRange) return false;
	int psiRange = observer->getPsiVision();
	if (psiRange > 0 && other->getArmor() && !other->getArmor()->getFearImmune())
	{
		const int camouflage = other->getArmor()->getPsiCamouflage();
		if (camouflage > 0) psiRange = std::min(psiRange, camouflage);
		else if (camouflage < 0) psiRange = std::max(0, psiRange + camouflage);
		if (distanceSq <= psiRange * psiRange) return true;
	}

	const bool dark = targetIsDark(save, geometry, target, maxDarkness);
	const int rangeTiles = std::min(maxRange, dark
		? observer->getMaxViewDistanceAtDark(other)
		: observer->getMaxViewDistanceAtDay(other));
	if (distanceSq > rangeTiles * rangeTiles) return false;

	const Position observerEye = eye(save, geometry, observer);
	const Position p = target->getPosition();
	const Position body = p.toVoxel() + Position(8, 8,
		-target->getTerrainLevel() + other->getFloatHeight() + std::max(3, other->getHeight() / 2));
	if (!geometry.rayClear(point(observerEye), point(body), BlockSight)) return false;

	// Preserve the historical range/smoke rule while the geometry authority is
	// exclusively REAL HD. Sample the ray in tile-sized steps, not every pixel.
	const float dx = float(body.x - observerEye.x), dy = float(body.y - observerEye.y), dz = float(body.z - observerEye.z);
	const float voxelDistance = std::sqrt(dx*dx + dy*dy + dz*dz);
	const int samples = std::max(1, int(std::ceil(voxelDistance / Position::TileXY)));
	float smoke = 0.0f, fireSmoke = 0.0f, nearSmoke = 0.0f, nearFireSmoke = 0.0f;
	for (int i = 0; i <= samples; ++i)
	{
		const float t = float(i) / samples;
		const Position v(int(observerEye.x + dx*t), int(observerEye.y + dy*t), int(observerEye.z + dz*t));
		const Tile *stepTile = save->getTile(v.toTile());
		if (!stepTile) return false;
		const float contribution = float(stepTile->getSmoke()) * voxelDistance / (samples + 1);
		if (stepTile->getFire() == 0)
		{
			smoke += contribution;
			if (voxelDistance * t < Position::TileXY * 2) nearSmoke += contribution;
		}
		else
		{
			fireSmoke += contribution;
			if (voxelDistance * t < Position::TileXY * 2) nearFireSmoke += contribution;
		}
	}
	const int smokeFactor = 100 - std::clamp(observer->getVisibilityThroughSmoke(), 0, 100);
	const int fireFactor = 100 - std::clamp(observer->getVisibilityThroughFire(), 0, 100);
	const int maxVoxelRange = rangeTiles * Position::TileXY + Position::TileXY / 4;
	const int quality = maxVoxelRange - int(voxelDistance) - int(
		((smoke - nearSmoke / 2) * smokeFactor + (fireSmoke - nearFireSmoke / 2) * fireFactor)
		* maxVoxelRange / (3.0f * 20.0f * 100.0f * Position::TileXY));
	ModScript::VisibilityUnit::Output arg{quality, quality,
		ScriptTag<BattleUnitVisibility>::getNullTag()};
	ModScript::VisibilityUnit::Worker worker{observer, other, target,
		int(voxelDistance), maxVoxelRange, rangeTiles,
		int(smoke), int(fireSmoke), int(nearSmoke), int(nearFireSmoke)};
	worker.execute(observer->getArmor()->getScript<ModScript::VisibilityUnit>(), arg);
	return arg.getFirst() > 0;
}
}
