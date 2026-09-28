/*
 * REAL HD LIGHTING AUTHORITY V2
 */
#include "RealHdLightingAuthority.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <string>
#include <vector>

#include "TileEngine.h"
#include "Pathfinding.h"
#include "RealHdPerceptionAuthority.h"
#include "RealHdOcclusion.h"
#include "RealHdPhysicalGeometry.h"
#include "../Engine/Logger.h"
#include "../Engine/Options.h"
#include "../Savegame/SavedBattleGame.h"
#include "../Savegame/BattleUnit.h"
#include "../Savegame/Tile.h"
#include "../Mod/Armor.h"

namespace OpenXcom
{
namespace RealHdLightingAuthority
{

namespace
{
constexpr float DEG_TO_RAD = 0.01745329251994329577f;

float clamp01(float value)
{
	return std::max(0.0f, std::min(1.0f, value));
}

float smooth01(float value)
{
	const float t = clamp01(value);
	return t * t * (3.0f - 2.0f * t);
}

bool isMagneticIon(const BattleUnit *unit)
{
	return unit && unit->getArmor() && unit->getArmor()->getType() == "STR_MAGNETIC_ION_ARMOR_UC";
}

void logProfileOnce(const BattleUnit *unit, const HelmetLightProfile &profile)
{
	if (!unit || !unit->getArmor()) return;
	static std::set<std::string> logged;
	const std::string armor = unit->getArmor()->getType();
	if (!logged.insert(armor).second) return;
	Log(LOG_INFO) << "[REAL HD HELMET LIGHT PROFILE V2][PROFILE] armor=" << armor
		<< " coreHalfDeg=" << profile.coreHalfDeg
		<< " outerHalfDeg=" << profile.outerHalfDeg
		<< " magneticLateral=" << (profile.magneticLateral ? 1 : 0)
		<< " lateralStartDeg=" << profile.magneticLateralStartDeg
		<< " physicalIntensityIndependentOfColor=YES"
		<< " rasterLegacyAuthority=NONE";
}
}

HelmetLightTuning helmetLightTuning()
{
	HelmetLightTuning tuning;
	tuning.intensity = std::max(0.0f, Options::hdHelmetLightIntensityPermille / 1000.0f);
	tuning.rangeTiles = std::max(0.0f, Options::hdHelmetLightRangeMilliTiles / 1000.0f);
	tuning.coreHalfDeg = std::max(0.0f, Options::hdHelmetLightCoreHalfAngleMilliDeg / 1000.0f);
	tuning.outerHalfDeg = std::max(tuning.coreHalfDeg + 0.001f,
		Options::hdHelmetLightOuterHalfAngleMilliDeg / 1000.0f);

	// Compatibility with options.cfg written by the original V2 lighting test.
	tuning.migratedV2Defaults = Options::hdHelmetLightRangeMilliTiles == 9000 &&
		Options::hdHelmetLightOuterHalfAngleMilliDeg == 90000;
	if (tuning.migratedV2Defaults)
	{
		tuning.rangeTiles = 13.0f;
		tuning.outerHalfDeg = 75.0f;
	}
	return tuning;
}

HelmetLightProfile helmetLightProfile(const BattleUnit *unit)
{
	const HelmetLightTuning tuning = helmetLightTuning();
	HelmetLightProfile profile;
	profile.coreHalfDeg = tuning.coreHalfDeg;
	profile.outerHalfDeg = tuning.outerHalfDeg;

	if (RealHdPerceptionAuthority::enabledFor(unit))
	{
		const float fovHalf = RealHdPerceptionAuthority::halfAngleDeg(unit);
		const float haloWidth = std::max(0.0f, tuning.outerHalfDeg - tuning.coreHalfDeg);
		if (isMagneticIon(unit))
		{
			// Magnetic Ion keeps the Ion frontal lamp family, then dedicated lateral
			// emitters/cap sensors carry the last 75->90 degrees with a colder tint.
			const float ionHalf = std::max(0.0f, std::min(90.0f, Options::hdFovIonHalfAngleMilliDeg / 1000.0f));
			profile.coreHalfDeg = std::min(fovHalf, ionHalf);
			profile.outerHalfDeg = fovHalf;
			profile.magneticLateral = profile.outerHalfDeg > profile.coreHalfDeg + 0.001f;
			profile.magneticLateralStartDeg = profile.coreHalfDeg;
		}
		else
		{
			// Base/Aquaplastique/Ion: the strong beam follows the actual perception
			// cone; only a presentation halo continues outside it, never detection.
			profile.coreHalfDeg = fovHalf;
			profile.outerHalfDeg = std::min(90.0f, fovHalf + haloWidth);
		}
	}

	profile.coreHalfDeg = std::max(0.0f, std::min(90.0f, profile.coreHalfDeg));
	profile.outerHalfDeg = std::max(profile.coreHalfDeg + 0.001f, std::min(90.0f, profile.outerHalfDeg));
	logProfileOnce(unit, profile);
	return profile;
}

HelmetLightSample helmetLightSampleAt(SavedBattleGame *save, TileEngine *tileEngine,
	const BattleUnit *unit, const Position &target,
	const RealHdPhysicalGeometry *geometry)
{
	HelmetLightSample sample;
	if (!save || !tileEngine || !unit || unit->isOut() || unit->getFaction() != FACTION_PLAYER || !unit->getArmor())
		return sample;
	if (!save->getTogglePersonalLight())
		return sample;

	const int authoredPersonalLight = std::max(0, unit->getArmor()->getPersonalLightFriend());
	if (authoredPersonalLight <= 0)
		return sample;

	const HelmetLightTuning tuning = helmetLightTuning();
	const HelmetLightProfile profile = helmetLightProfile(unit);
	const float equipmentScale = std::max(0.15f, std::min(2.0f, authoredPersonalLight / 15.0f));
	const float range = tuning.rangeTiles * equipmentScale;
	if (range <= 0.01f || tuning.intensity <= 0.0f)
		return sample;

	Position forward;
	Pathfinding::directionToVector(unit->getDirection() & 7, &forward);
	const float fwdLen = std::sqrt((float)(forward.x * forward.x + forward.y * forward.y));
	if (fwdLen <= 0.0f)
		return sample;
	const float fwdX = forward.x / fwdLen;
	const float fwdY = forward.y / fwdLen;
	const Position source = unit->getPosition();

	const float dx = (target.x + 0.5f) - (source.x + 0.5f);
	const float dy = (target.y + 0.5f) - (source.y + 0.5f);
	const float dz = (float)(target.z - source.z) * 1.35f;
	const float horizontal = std::sqrt(dx * dx + dy * dy);
	const float distance = std::sqrt(horizontal * horizontal + dz * dz);
	if (distance > range)
		return sample;

	float angleDeg = 0.0f;
	float angular = 1.0f;
	if (horizontal > 0.001f)
	{
		const float dot = std::max(-1.0f, std::min(1.0f,
			(dx / horizontal) * fwdX + (dy / horizontal) * fwdY));
		angleDeg = std::acos(dot) / DEG_TO_RAD;
		if (angleDeg >= profile.outerHalfDeg)
			return sample;
		if (angleDeg > profile.coreHalfDeg)
		{
			float t = (profile.outerHalfDeg - angleDeg) /
				std::max(0.001f, profile.outerHalfDeg - profile.coreHalfDeg);
			angular = smooth01(t);
			angular *= angular; // dark peripheral spill / lateral falloff.
		}
	}

	float radial = clamp01(1.0f - distance / range);
	radial = std::pow(radial, 0.75f);
	const float contribution = clamp01(tuning.intensity * equipmentScale * angular * radial);
	if (contribution <= 0.001f)
		return sample;
	if (!geometry && tileEngine) geometry = tileEngine->getRealHdPhysicalGeometry();

	// Same physical occlusion authority used by the renderer. This is geometry,
	// not perception: no FOV/visible/HUD/raster sample is consulted here.
	if (target != source)
	{
		const Tile *targetTile = save->getTile(target);
		if (!targetTile) return sample;
		Position sourceEye = RealHdOcclusion::eye(save, unit);
		if (geometry)
		{
			const Tile *sourceTile = save->getTile(source);
			if (!sourceTile) return sample;
			sourceEye = source.toVoxel() + Position(8, 8,
				-sourceTile->getTerrainLevel() + unit->getHeight() +
				unit->getFloatHeight() - 1);
			if (unit->isBigUnit()) sourceEye += Position(8, 8, 1);
			const RealHdTilePhysics *above = geometry->tile(
				source.x, source.y, source.z + 1);
			if (above && !above->supports.empty() &&
				sourceEye.z >= (source.z + 1) * Position::TileZ)
				sourceEye.z = (source.z + 1) * Position::TileZ - 1;
		}
		const Position targetVoxel = target.toVoxel() + Position(8, 8,
			-targetTile->getTerrainLevel() + 4);
		const bool clear = geometry ? geometry->rayClear(
			{double(sourceEye.x), double(sourceEye.y), double(sourceEye.z)},
			{double(targetVoxel.x), double(targetVoxel.y), double(targetVoxel.z)},
			BlockLight) : RealHdOcclusion::terrainRayClear(save, sourceEye, targetVoxel);
		if (!clear)
			return sample;
	}

	float tintR = 1.0f, tintG = 1.0f, tintB = 1.0f;
	if (profile.magneticLateral && angleDeg > profile.magneticLateralStartDeg)
	{
		const float lateral = smooth01((angleDeg - profile.magneticLateralStartDeg) /
			std::max(0.001f, profile.outerHalfDeg - profile.magneticLateralStartDeg));
		const float targetR = std::max(0.0f, Options::hdHelmetMagneticLateralTintRPermille / 1000.0f);
		const float targetG = std::max(0.0f, Options::hdHelmetMagneticLateralTintGPermille / 1000.0f);
		const float targetB = std::max(0.0f, Options::hdHelmetMagneticLateralTintBPermille / 1000.0f);
		tintR += (targetR - 1.0f) * lateral;
		tintG += (targetG - 1.0f) * lateral;
		tintB += (targetB - 1.0f) * lateral;
	}

	sample.intensity = contribution;
	sample.r = contribution * tintR;
	sample.g = contribution * tintG;
	sample.b = contribution * tintB;
	return sample;
}

float helmetLightContributionAt(SavedBattleGame *save, TileEngine *tileEngine,
	const BattleUnit *unit, const Position &target)
{
	return helmetLightSampleAt(save, tileEngine, unit, target).intensity;
}

float helmetLightAt(SavedBattleGame *save, TileEngine *tileEngine, const Position &target)
{
	if (!Options::hdGraphics || !save || !tileEngine || !save->getTogglePersonalLight())
		return 0.0f;

	float light = 0.0f;
	for (BattleUnit *unit : *save->getUnits())
	{
		light += helmetLightContributionAt(save, tileEngine, unit, target);
		if (light >= 1.0f)
			return 1.0f;
	}
	return clamp01(light);
}

float perceptionLightAt(SavedBattleGame *save, TileEngine *tileEngine, const Tile *tile)
{
	if (!save || !tileEngine || !tile)
		return 0.0f;

	const int semanticLightLevel = std::max(tile->getLight(LL_AMBIENT),
		std::max(tile->getLight(LL_FIRE), tile->getLight(LL_ITEMS)));
	const float semanticLight = clamp01(semanticLightLevel / 15.0f);
	const float helmetLight = helmetLightAt(save, tileEngine, tile->getPosition());
	return clamp01(semanticLight + helmetLight);
}

bool targetIsDarkForObserver(SavedBattleGame *save, TileEngine *tileEngine,
	const Tile *tile, const BattleUnit *observer, int maxDarknessToSeeUnits)
{
	if (!tile)
		return true;

	// Absolute Legacy/OXCE compatibility gate. Hostile/neutral observers and
	// REAL HD OFF execute the exact pre-existing shade test, including LL_UNITS.
	if (!Options::hdGraphics || !observer || observer->getFaction() != FACTION_PLAYER)
		return tile->getShade() > maxDarknessToSeeUnits;

	const float requiredLight = std::max(0.0f, std::min(1.0f,
		(15.0f - (float)maxDarknessToSeeUnits) / 15.0f));
	return perceptionLightAt(save, tileEngine, tile) + 1e-6f < requiredLight;
}

}
}
