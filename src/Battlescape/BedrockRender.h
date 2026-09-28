#pragma once

#include <string>
#include "../Mod/MapData.h"

namespace OpenXcom
{

class SavedBattleGame;
class RuleTerrain;
class Tile;

/** Presentation-only primary terrain semantic. */
enum class BedrockMaterial
{
	None = 0,
	Sand
};

/**
 * Presentation geometry for one logical cell.
 * Values are vertical offsets in Legacy logical pixels (negative = visually up).
 * The four values address the diamond corners in screen order.
 */
struct BedrockCellGeometry
{
	int top = 0;
	int right = 0;
	int bottom = 0;
	int left = 0;
	bool explicitGeometry = false;
	bool semanticDebris = false;
	int sourceMcdId = -1;
};

/**
 * BEDROCK never changes OXCE gameplay semantics. It transfers graphical ownership
 * of selected terrain representations to a continuous primary-terrain renderer.
 * Coverage, presentation geometry and local visuals are deliberately separate.
 *
 * STRICT OWNERSHIP CONTRACT V2 + PRESENTATION FALLBACK V3:
 * BEDROCK eligibility is decided only from logical OXCE/MCD identity. Graphical
 * files (PCK/PNG), colours and sprite frame contents never participate in that
 * semantic decision. Presentation ownership is acquired only while the required
 * MaterialSet is actually available; otherwise normal HD/Legacy rendering owns
 * the tile. SAND is fully eligible; DEBRIS MCD 32..49 are the only extra records.
 */
class BedrockRenderPolicy
{
public:
	static BedrockMaterial resolve(const SavedBattleGame *save);

	/**
	 * Legacy semantic adapter activation. The terrain is scanned for supported
	 * semantic datasets and semantic eligibility is persisted independently from
	 * resource availability. resolve() grants BEDROCK presentation ownership only
	 * while the complete minimum MaterialSet exists in the active VFS; otherwise
	 * the normal HD resolver / Legacy path keeps graphical ownership.
	 */
	static bool armFromTerrain(SavedBattleGame *save, RuleTerrain *terrain, const char *source);

	/** True only when the complete minimum MaterialSet exists in the VFS. */
	static bool materialSetAvailable(BedrockMaterial material);
	static bool verticalMaterialAvailable(BedrockMaterial material);

	/** True when BEDROCK owns the visual representation of this MapData. */
	static bool ownsMapData(BedrockMaterial material, const MapData *data);
	static bool ownsTilePart(BedrockMaterial material, const Tile *tile, TilePart part);

	/** True when this logical cell participates in the primary BEDROCK substrate. */
	static bool hasSurface(BedrockMaterial material, const Tile *tile);

	/** Scalar level derived only from BEDROCK-owned logical MCD parts. */
	static int sourceTerrainLevel(BedrockMaterial material, const Tile *tile);
	/** BEDROCK-effective level; deliberately excludes non-owned local objects. */
	static int effectiveTerrainLevel(BedrockMaterial material, const Tile *tile);

	/** Explicit REAL-HD presentation profile where known; scalar gameplay fallback otherwise. */
	static BedrockCellGeometry cellGeometry(BedrockMaterial material, const Tile *tile);

	/** MaterialSet paths. Convention is generic; semantic adapters decide ownership. */
	static std::string assetPath(BedrockMaterial material);
	static std::string normalPath(BedrockMaterial material);
	static std::string roughnessPath(BedrockMaterial material);
	static std::string aoPath(BedrockMaterial material);
	/** HD blast-impact authoring masks. They are aligned in one canvas and sampled into one merged world-space field. */
	static std::string craterCoreMaskPath(BedrockMaterial material);
	static std::string craterRimMaskPath(BedrockMaterial material);
	static std::string blastHaloMaskPath(BedrockMaterial material);
	/** V5 authored variant pools, A..G. Missing variants fall back to the first available set at runtime. */
	static std::string blastCoreVariantPath(BedrockMaterial material, int variant);
	static std::string blastRimVariantPath(BedrockMaterial material, int variant);
	static std::string blastHaloVariantPath(BedrockMaterial material, int variant);
	static std::string weaponImpactVariantPath(BedrockMaterial material, int variant);
	static std::string weaponImpactCoreVariantPath(BedrockMaterial material, int variant);
	static std::string weaponImpactRimVariantPath(BedrockMaterial material, int variant);
	static std::string weaponImpactHaloVariantPath(BedrockMaterial material, int variant);
	/** Transient authored dust sprites. These are presentation assets only; triggering is derived from crater/impact events. */
	static std::string weaponDustVariantPath(BedrockMaterial material, int variant);
	static std::string blastDustVariantPath(BedrockMaterial material, int variant);
	/** Compatibility fallback for V1-V3 packs; still sampled by the HD pipeline, never by Legacy rendering. */
	static std::string craterMaskPath(BedrockMaterial material);
	static std::string verticalAssetPath(BedrockMaterial material);
	static std::string verticalNormalPath(BedrockMaterial material);
	static std::string verticalRoughnessPath(BedrockMaterial material);
	static std::string verticalAoPath(BedrockMaterial material);
	static const char *semanticKey(BedrockMaterial material);
	static const char *name(BedrockMaterial material);
};

}
