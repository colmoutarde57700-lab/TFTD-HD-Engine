#pragma once
/*
 * REAL HD LIGHTING AUTHORITY.
 *
 * Shared, renderer-independent physical-light facts for REAL HD. This bridge
 * consumes tactical light facts while preventing the
 * historical radial LL_UNITS personal-light field from becoming a hidden
 * graphical or player-perception authority when the REAL HD pipeline is on.
 */

namespace OpenXcom
{

class SavedBattleGame;
class TileEngine;
class BattleUnit;
class Tile;
class Position;
class RealHdPhysicalGeometry;

namespace RealHdLightingAuthority
{

struct HelmetLightTuning
{
	float intensity = 1.0f;
	float rangeTiles = 13.0f;
	// These two values are the base/fallback profile and define the stock halo
	// extension width. Stock REAL HD armors derive their core from their FOV.
	float coreHalfDeg = 45.0f;
	float outerHalfDeg = 75.0f;
	bool migratedV2Defaults = false;
};

struct HelmetLightProfile
{
	float coreHalfDeg = 45.0f;
	float outerHalfDeg = 75.0f;
	bool magneticLateral = false;
	float magneticLateralStartDeg = 75.0f;
};

// RGB values are additive light energy, not a detection colour semantic.
// intensity is the scalar physical energy used by perception/range rules.
struct HelmetLightSample
{
	float intensity = 0.0f;
	float r = 0.0f;
	float g = 0.0f;
	float b = 0.0f;
};

HelmetLightTuning helmetLightTuning();
HelmetLightProfile helmetLightProfile(const BattleUnit *unit);

// Normalized physical helmet-light contribution from one Aquanaut. Uses body
// direction in this stage and REAL HD physical geometry for occlusion. It never
// consults BattleUnit::visible, HUD state or a Legacy raster.
HelmetLightSample helmetLightSampleAt(SavedBattleGame *save, TileEngine *tileEngine,
	const BattleUnit *unit, const Position &target,
	const RealHdPhysicalGeometry *geometry = nullptr);
float helmetLightContributionAt(SavedBattleGame *save, TileEngine *tileEngine,
	const BattleUnit *unit, const Position &target);

// Sum of all active player helmet lights at a tile, clamped to [0..1].
float helmetLightAt(SavedBattleGame *save, TileEngine *tileEngine, const Position &target);

// Physical light used by player perception in REAL HD. Ambient/fire/items are
// legitimate OXCE semantic inputs. LL_UNITS is intentionally excluded and is
// replaced by the directional helmet-light authority above. Depth luminance
// and LUT/color grading are presentation-only and therefore excluded here.
float perceptionLightAt(SavedBattleGame *save, TileEngine *tileEngine, const Tile *tile);

// Preserve the exact upstream darkness path outside REAL HD player perception.
bool targetIsDarkForObserver(SavedBattleGame *save, TileEngine *tileEngine,
	const Tile *tile, const BattleUnit *observer, int maxDarknessToSeeUnits);

}
}
