#include "BedrockRender.h"

#include "../Savegame/SavedBattleGame.h"
#include "../Savegame/Tile.h"
#include "../Mod/MapDataSet.h"
#include "../Mod/RuleTerrain.h"
#include "../Engine/Logger.h"
#include "../Engine/FileMap.h"

#include <algorithm>
#include <sstream>

namespace OpenXcom
{
namespace
{

bool datasetNamed(const MapData *data, const char *upper, const char *lower)
{
	if (!data || !data->getDataset()) return false;
	const std::string &name = data->getDataset()->getName();
	return name == upper || name == lower;
}

bool isSandDataset(const MapData *data)
{
	return datasetNamed(data, "SAND", "sand");
}

bool isDebrisDataset(const MapData *data)
{
	return datasetNamed(data, "DEBRIS", "debris");
}

// STRICT BEDROCK OWNERSHIP CONTRACT V2.
// Graphical files, PCK frame numbers, PNG paths and colour content NEVER decide
// BEDROCK ownership. The renderer consumes stable logical MCD identities only:
//   * every logical record in SAND;
//   * DEBRIS MCD 32..49, which are the logical records behind PCK 057..066.
// Everything else remains a normal local visual even if it contains baked sand.
bool isBedrockSandMapData(const MapData *data)
{
	if (!data) return false;
	if (isSandDataset(data)) return true;
	if (isDebrisDataset(data))
	{
		const int id = data->getDatasetIndex();
		return id >= 32 && id <= 49;
	}
	return false;
}

BedrockMaterial semanticForDatasetName(const std::string &name)
{
	// Activation is terrain-semantic only. A terrain that declares SAND enables
	// BEDROCK_SAND; no other dataset can arm it and no graphical asset is queried.
	if (name == "SAND" || name == "sand") return BedrockMaterial::Sand;
	return BedrockMaterial::None;
}

// Logical MCD geometry contract. The profile number is selected from the stable
// MCD record identity, never from the sprite currently assigned to that record.
// This preserves the already validated slope topology while allowing PNG/PCK
// artwork to be replaced, deleted or completely redesigned without changing the
// BEDROCK mesh.
static const int BedrockCornerProfiles[22][4] =
{
	{  0, -8,  0,  0 }, // profile 00
	{  0, -8, -8,  0 }, // profile 01
	{  0,  0, -8,  0 }, // profile 02
	{  0,  0, -8, -8 }, // profile 03
	{  0,  0,  0, -8 }, // profile 04
	{ -8, -8,  0,  0 }, // profile 05
	{ -8,  0,  0,  0 }, // profile 06
	{ -8,  0,  0, -8 }, // profile 07
	{ -8, -8, -8, -8 }, // profile 08
	{-16,-16,-16,-16 }, // profile 09
	{-24,-24,-24,-24 }, // profile 10
	{ -8, -8, -8,  0 }, // profile 11
	{ -8,  0, -8, -8 }, // profile 12
	{ -8, -8,  0, -8 }, // profile 13
	{  0, -8, -8, -8 }, // profile 14
	{  0,  0,  0,  0 }, // profile 15
	{  0,  0,  0,  0 }, // profile 16
	{  0,  0,  0,  0 }, // profile 17
	{  0,  0,  0,  0 }, // profile 18
	{ -8, -8, -8, -8 }, // profile 19
	{  0,  0,  0,  0 }, // profile 20
	{  0,  0,  0,  0 }  // profile 21
};

// SAND has 20 MCD records. Their logical identities map to the 22 validated
// presentation profiles; profiles 09/10 are represented by the DEBRIS dune
// records below rather than separate SAND MCD records.
static const int SandMcdToProfile[20] =
{
	0, 1, 2, 3, 4, 5, 6, 7, 8,
	11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21
};

int profileForLogicalMapData(const MapData *data, bool &semanticDebris)
{
	semanticDebris = false;
	if (!data) return -1;
	const int id = data->getDatasetIndex();
	if (isSandDataset(data))
	{
		if (id >= 0 && id < (int)(sizeof(SandMcdToProfile) / sizeof(SandMcdToProfile[0])))
			return SandMcdToProfile[id];
		return -1;
	}
	if (isDebrisDataset(data) && id >= 32 && id <= 49)
	{
		semanticDebris = true;
		if (id == 32) return 9;
		if (id == 33) return 10;
		// MCD 34..41 and 42..49 are the two logical height variants of profiles 00..07.
		return (id - 34) & 7;
	}
	return -1;
}

bool fillProfileForMapData(const MapData *data, BedrockCellGeometry &out)
{
	bool semanticDebris = false;
	const int profile = profileForLogicalMapData(data, semanticDebris);
	if (profile < 0 || profile > 21) return false;

	// P_Level/getYOffset is logical MCD presentation geometry. It distinguishes the
	// two DEBRIS 59..66 logical height variants without consulting their sprites.
	const int yShift = -data->getYOffset();
	out.top = BedrockCornerProfiles[profile][0] + yShift;
	out.right = BedrockCornerProfiles[profile][1] + yShift;
	out.bottom = BedrockCornerProfiles[profile][2] + yShift;
	out.left = BedrockCornerProfiles[profile][3] + yShift;
	out.explicitGeometry = true;
	out.semanticDebris = semanticDebris;
	out.sourceMcdId = data->getDatasetIndex();
	return true;
}

// REAL HD DATASET PROVIDER V1.
// Both TFTD_REAL_HD_TEXTURES and TFTD_REAL_HD_DEBUG expose the SAME virtual
// resource namespace. FileMap/VFS priority chooses the active provider; the
// executable never branches on a mod id or display name. During migration the
// historical BEDROCK runtime remains a compatibility fallback.
std::string realHdDatasetRoot(const char *dataset)
{
	if (!dataset || !*dataset) return std::string();
	return std::string("Resources/TFTD_HD/RealHD/Datasets/") + dataset + "/";
}

std::string legacyBedrockMaterialRoot(const char *dataset)
{
	if (!dataset || !*dataset) return std::string();
	return std::string("Resources/TFTD_HD/Bedrock/Materials/") + dataset + "/";
}

std::string resolveDatasetAsset(const char *dataset, const std::string &canonicalRelative, const std::string &legacyRelative)
{
	if (!dataset || !*dataset) return std::string();
	const std::string canonical = realHdDatasetRoot(dataset) + canonicalRelative;
	if (FileMap::fileExists(canonical)) return canonical;
	if (!legacyRelative.empty())
	{
		const std::string legacy = legacyBedrockMaterialRoot(dataset) + legacyRelative;
		if (FileMap::fileExists(legacy)) return legacy;
	}
	// Return the canonical path even when absent so logs and future provider
	// diagnostics always advertise the new contract rather than the retired path.
	return canonical;
}

std::string resolveDatasetEffectAsset(const char *dataset, const std::string &canonicalRelative, const std::string &legacyPath)
{
	if (!dataset || !*dataset) return std::string();
	const std::string canonical = realHdDatasetRoot(dataset) + canonicalRelative;
	if (FileMap::fileExists(canonical)) return canonical;
	if (!legacyPath.empty() && FileMap::fileExists(legacyPath)) return legacyPath;
	return canonical;
}

std::string resolvedProviderFullPath(const std::string &virtualPath)
{
	if (virtualPath.empty() || !FileMap::fileExists(virtualPath)) return std::string("<missing>");
	const FileMap::FileRecord *provider = FileMap::at(virtualPath);
	return provider ? provider->fullpath : std::string("<unknown>");
}

}

BedrockMaterial BedrockRenderPolicy::resolve(const SavedBattleGame *save)
{
	if (!save) return BedrockMaterial::None;

	// V3 CONTRACT: the value stored in SavedBattleGame is semantic eligibility,
	// not unconditional presentation ownership. This distinction is essential for
	// partially migrated mod stacks: SAND may remain semantically BEDROCK-capable
	// while the provider carrying its MaterialSet is disabled or absent. In that
	// case BEDROCK must release presentation ownership so the normal HD resolver
	// (and ultimately Legacy) can draw the original tile instead of leaving a hole.
	BedrockMaterial semantic = BedrockMaterial::None;
	if (save->getBedrockRenderMaterial() == "SAND")
		semantic = BedrockMaterial::Sand;
	if (semantic == BedrockMaterial::None)
		return BedrockMaterial::None;

	static std::string unavailableSemantic;
	const char *key = semanticKey(semantic);
	if (!materialSetAvailable(semantic))
	{
		if (unavailableSemantic != key)
		{
			Log(LOG_WARNING) << "[BEDROCK MATERIAL RESOLVER V3][PRESENTATION-RELEASE] semantic=" << key
				<< " materialSet=MISSING semanticEligibility=PRESERVED"
				<< " legacyFallback=ALLOWED normalHdResolver=ALLOWED";
			unavailableSemantic = key;
		}
		return BedrockMaterial::None;
	}

	if (unavailableSemantic == key)
	{
		Log(LOG_INFO) << "[BEDROCK MATERIAL RESOLVER V3][PRESENTATION-RESTORE] semantic=" << key
			<< " materialSet=READY semanticEligibility=PRESERVED presentationOwnership=BEDROCK";
		unavailableSemantic.clear();
	}
	return semantic;
}

bool BedrockRenderPolicy::armFromTerrain(SavedBattleGame *save, RuleTerrain *terrain, const char *source)
{
	if (!save || !terrain) return false;
	if (!save->getBedrockRenderMaterial().empty()) return false;

	for (MapDataSet *set : *terrain->getMapDataSets())
	{
		if (!set) continue;
		const BedrockMaterial semantic = semanticForDatasetName(set->getName());
		if (semantic == BedrockMaterial::None) continue;
		const char *key = semanticKey(semantic);
		const bool materialReady = materialSetAvailable(semantic);
		// Persist semantic eligibility independently from presentation availability.
		// If the MaterialSet is missing, resolve() deliberately returns None so the
		// regular HD/Legacy renderer retains presentation ownership. Keeping the
		// semantic key lets BEDROCK resume automatically if its provider is re-enabled.
		save->setBedrockRenderMaterial(key);
		if (!materialReady)
		{
			Log(LOG_WARNING) << "[BEDROCK MATERIAL RESOLVER V3][ARM-MISSING-MATERIAL] terrain=" << terrain->getName()
				<< " source=" << (source ? source : "unknown")
				<< " semantic=" << key
				<< " semanticEligibility=PRESERVED presentationOwnership=RELEASED"
				<< " legacyFallback=ALLOWED normalHdResolver=ALLOWED";
		}
		const std::string resolvedTop = assetPath(semantic);
		const std::string resolvedVertical = verticalAssetPath(semantic);
		Log(LOG_INFO) << "[BEDROCK MATERIAL RESOLVER V3][ARM] terrain=" << terrain->getName()
			<< " source=" << (source ? source : "unknown")
			<< " semanticDataset=" << set->getName() << " materialSet=" << key
			<< " semanticEligibility=PRESERVED"
			<< " presentationOwnership=" << (materialReady ? "BEDROCK" : "RELEASED")
			<< " top=" << resolvedTop
			<< " topProvider=" << resolvedProviderFullPath(resolvedTop)
			<< " vertical=" << resolvedVertical
			<< " verticalProvider=" << resolvedProviderFullPath(resolvedVertical);

		std::ostringstream nonOwned;
		bool firstNonOwned = true;
		size_t sandRecords = 0, debrisRecords = 0;
		for (MapDataSet *auditSet : *terrain->getMapDataSets())
		{
			if (!auditSet) continue;
			if (auditSet->getName() == "SAND" || auditSet->getName() == "sand")
			{
				sandRecords = auditSet->getSize();
				continue;
			}
			if (auditSet->getName() == "DEBRIS" || auditSet->getName() == "debris")
			{
				debrisRecords = auditSet->getSize();
				continue;
			}
			if (!firstNonOwned) nonOwned << ",";
			nonOwned << auditSet->getName();
			firstNonOwned = false;
		}
		Log(LOG_INFO) << "[BEDROCK STRICT OWNERSHIP V2][CONTRACT] terrain=" << terrain->getName()
			<< " SAND_MCD=ALL(" << sandRecords << ")"
			<< " DEBRIS_MCD=32..49(datasetSize=" << debrisRecords << ")"
			<< " graphicalClassification=DISABLED"
			<< " nonOwnedDatasets=" << (firstNonOwned ? std::string("<none>") : nonOwned.str());
		return true;
	}
	return false;
}

bool BedrockRenderPolicy::materialSetAvailable(BedrockMaterial material)
{
	if (material == BedrockMaterial::None) return false;
	const std::string top = assetPath(material);
	const std::string vertical = verticalAssetPath(material);
	return !top.empty() && !vertical.empty() && FileMap::fileExists(top) && FileMap::fileExists(vertical);
}

bool BedrockRenderPolicy::verticalMaterialAvailable(BedrockMaterial material)
{
	const std::string vertical = verticalAssetPath(material);
	return !vertical.empty() && FileMap::fileExists(vertical);
}

bool BedrockRenderPolicy::ownsMapData(BedrockMaterial material, const MapData *data)
{
	return material == BedrockMaterial::Sand && isBedrockSandMapData(data);
}

bool BedrockRenderPolicy::ownsTilePart(BedrockMaterial material, const Tile *tile, TilePart part)
{
	if (!tile) return false;
	return ownsMapData(material, tile->getMapData(part));
}

bool BedrockRenderPolicy::hasSurface(BedrockMaterial material, const Tile *tile)
{
	if (!tile || material != BedrockMaterial::Sand) return false;
	// Only logical SAND and DEBRIS MCD 32..49 can create BEDROCK coverage.
	return ownsTilePart(material, tile, O_FLOOR) || ownsTilePart(material, tile, O_OBJECT);
}

int BedrockRenderPolicy::sourceTerrainLevel(BedrockMaterial material, const Tile *tile)
{
	if (!tile || material != BedrockMaterial::Sand) return 0;
	bool found = false;
	int level = 0;
	for (TilePart part : {O_FLOOR, O_OBJECT})
	{
		MapData *data = tile->getMapData(part);
		if (!ownsMapData(material, data)) continue;
		const int partLevel = data->getTerrainLevel();
		level = found ? std::min(level, partLevel) : partLevel;
		found = true;
	}
	return found ? level : 0;
}

int BedrockRenderPolicy::effectiveTerrainLevel(BedrockMaterial material, const Tile *tile)
{
	// Intentionally identical to sourceTerrainLevel. Local CORAL/CRLJ1/ROCKS/
	// craft/etc. may affect OXCE gameplay height but can never deform BEDROCK.
	return sourceTerrainLevel(material, tile);
}

BedrockCellGeometry BedrockRenderPolicy::cellGeometry(BedrockMaterial material, const Tile *tile)
{
	BedrockCellGeometry out;
	if (!tile || material != BedrockMaterial::Sand || !hasSurface(material, tile)) return out;

	// Owned OBJECT terrain has priority over the base floor (dunes/slopes on sand).
	// Classification and profile selection are both based on logical MCD identity.
	if (MapData *object = tile->getMapData(O_OBJECT))
	{
		if (ownsMapData(material, object) && fillProfileForMapData(object, out)) return out;
	}
	if (MapData *floor = tile->getMapData(O_FLOOR))
	{
		if (ownsMapData(material, floor) && fillProfileForMapData(floor, out)) return out;
	}

	// A patched/modded SAND record unknown to the profile table remains BEDROCK-owned
	// but falls back to its own logical MCD terrain level. Never consult local objects
	// and never re-enable a Legacy/HD tile sprite as a graphical fallback.
	const int h = sourceTerrainLevel(material, tile);
	out.top = out.right = out.bottom = out.left = h;
	return out;
}

std::string BedrockRenderPolicy::assetPath(BedrockMaterial material)
{
	const char *key = semanticKey(material);
	if (!key || !*key) return std::string();
	return resolveDatasetAsset(key, "Materials/TOP_BASE.png", "TOP_BASE.png");
}

std::string BedrockRenderPolicy::normalPath(BedrockMaterial material)
{
	const char *key = semanticKey(material);
	if (!key || !*key) return std::string();
	return resolveDatasetAsset(key, "Materials/TOP_NORMAL_DX.png", "TOP_NORMAL_DX.png");
}

std::string BedrockRenderPolicy::roughnessPath(BedrockMaterial material)
{
	const char *key = semanticKey(material);
	if (!key || !*key) return std::string();
	return resolveDatasetAsset(key, "Materials/TOP_ROUGHNESS.png", "TOP_ROUGHNESS.png");
}

std::string BedrockRenderPolicy::aoPath(BedrockMaterial material)
{
	const char *key = semanticKey(material);
	if (!key || !*key) return std::string();
	return resolveDatasetAsset(key, "Materials/TOP_AO.png", "TOP_AO.png");
}

std::string BedrockRenderPolicy::craterCoreMaskPath(BedrockMaterial material)
{
	const char *key = semanticKey(material);
	if (!key || !*key) return std::string();
	return resolveDatasetAsset(key, "Materials/CRATER_CORE_MASK.png", "CRATER_CORE_MASK.png");
}

std::string BedrockRenderPolicy::craterRimMaskPath(BedrockMaterial material)
{
	const char *key = semanticKey(material);
	if (!key || !*key) return std::string();
	return resolveDatasetAsset(key, "Materials/CRATER_RIM_MASK.png", "CRATER_RIM_MASK.png");
}

std::string BedrockRenderPolicy::blastHaloMaskPath(BedrockMaterial material)
{
	const char *key = semanticKey(material);
	if (!key || !*key) return std::string();
	return resolveDatasetAsset(key, "Materials/BLAST_HALO_MASK.png", "BLAST_HALO_MASK.png");
}

namespace
{
char impactVariantLetter(int variant)
{
	return (char)('A' + std::max(0, std::min(6, variant)));
}

char tripletVariantLetter(int variant)
{
	return (char)('A' + std::max(0, std::min(2, variant)));
}
}

std::string BedrockRenderPolicy::blastCoreVariantPath(BedrockMaterial material, int variant)
{
	const char *key = semanticKey(material);
	if (!key || !*key) return std::string();
	return resolveDatasetAsset(key, std::string("Materials/BlastSets/CORE_") + impactVariantLetter(variant) + ".png", std::string("BlastSets/CORE_") + impactVariantLetter(variant) + ".png");
}

std::string BedrockRenderPolicy::blastRimVariantPath(BedrockMaterial material, int variant)
{
	const char *key = semanticKey(material);
	if (!key || !*key) return std::string();
	return resolveDatasetAsset(key, std::string("Materials/BlastSets/RIM_") + impactVariantLetter(variant) + ".png", std::string("BlastSets/RIM_") + impactVariantLetter(variant) + ".png");
}

std::string BedrockRenderPolicy::blastHaloVariantPath(BedrockMaterial material, int variant)
{
	const char *key = semanticKey(material);
	if (!key || !*key) return std::string();
	return resolveDatasetAsset(key, std::string("Materials/BlastSets/HALO_") + impactVariantLetter(variant) + ".png", std::string("BlastSets/HALO_") + impactVariantLetter(variant) + ".png");
}

std::string BedrockRenderPolicy::weaponImpactVariantPath(BedrockMaterial material, int variant)
{
	const char *key = semanticKey(material);
	if (!key || !*key) return std::string();
	return resolveDatasetAsset(key, std::string("Materials/WeaponImpacts/IMPACT_") + impactVariantLetter(variant) + ".png", std::string("WeaponImpacts/IMPACT_") + impactVariantLetter(variant) + ".png");
}


std::string BedrockRenderPolicy::weaponImpactCoreVariantPath(BedrockMaterial material, int variant)
{
	const char *key = semanticKey(material);
	if (!key || !*key) return std::string();
	return resolveDatasetAsset(key, std::string("Materials/WeaponImpacts/IMPACT_CORE_") + tripletVariantLetter(variant) + ".png", std::string("WeaponImpacts/IMPACT_CORE_") + tripletVariantLetter(variant) + ".png");
}

std::string BedrockRenderPolicy::weaponImpactRimVariantPath(BedrockMaterial material, int variant)
{
	const char *key = semanticKey(material);
	if (!key || !*key) return std::string();
	return resolveDatasetAsset(key, std::string("Materials/WeaponImpacts/IMPACT_RIM_") + tripletVariantLetter(variant) + ".png", std::string("WeaponImpacts/IMPACT_RIM_") + tripletVariantLetter(variant) + ".png");
}

std::string BedrockRenderPolicy::weaponImpactHaloVariantPath(BedrockMaterial material, int variant)
{
	const char *key = semanticKey(material);
	if (!key || !*key) return std::string();
	return resolveDatasetAsset(key, std::string("Materials/WeaponImpacts/IMPACT_HALO_") + tripletVariantLetter(variant) + ".png", std::string("WeaponImpacts/IMPACT_HALO_") + tripletVariantLetter(variant) + ".png");
}

std::string BedrockRenderPolicy::weaponDustVariantPath(BedrockMaterial material, int variant)
{
	if (material == BedrockMaterial::None) return std::string();
	const char letter = (char)('A' + std::max(0, std::min(3, variant)));
	const char *key = semanticKey(material);
	if (!key || !*key) return std::string();
	return resolveDatasetEffectAsset(key, std::string("Effects/Dust/Weapon/WEAPON_DUST_") + letter + ".png",
		std::string("Resources/TFTD_HD/Bedrock/Effects/Dust/Weapon/WEAPON_DUST_") + letter + ".png");
}

std::string BedrockRenderPolicy::blastDustVariantPath(BedrockMaterial material, int variant)
{
	if (material == BedrockMaterial::None) return std::string();
	const char letter = (char)('A' + std::max(0, std::min(4, variant)));
	const char *key = semanticKey(material);
	if (!key || !*key) return std::string();
	return resolveDatasetEffectAsset(key, std::string("Effects/Dust/Blast/BLAST_DUST_") + letter + ".png",
		std::string("Resources/TFTD_HD/Bedrock/Effects/Dust/Blast/BLAST_DUST_") + letter + ".png");
}

std::string BedrockRenderPolicy::craterMaskPath(BedrockMaterial material)
{
	const char *key = semanticKey(material);
	if (!key || !*key) return std::string();
	return resolveDatasetAsset(key, "Materials/CRATER_MASK.png", "CRATER_MASK.png");
}

std::string BedrockRenderPolicy::verticalAssetPath(BedrockMaterial material)
{
	const char *key = semanticKey(material);
	if (!key || !*key) return std::string();
	return resolveDatasetAsset(key, "Materials/VERTICAL_BASE.png", "VERTICAL_BASE.png");
}

std::string BedrockRenderPolicy::verticalNormalPath(BedrockMaterial material)
{
	const char *key = semanticKey(material);
	if (!key || !*key) return std::string();
	return resolveDatasetAsset(key, "Materials/VERTICAL_NORMAL_DX.png", "VERTICAL_NORMAL_DX.png");
}

std::string BedrockRenderPolicy::verticalRoughnessPath(BedrockMaterial material)
{
	const char *key = semanticKey(material);
	if (!key || !*key) return std::string();
	return resolveDatasetAsset(key, "Materials/VERTICAL_ROUGHNESS.png", "VERTICAL_ROUGHNESS.png");
}

std::string BedrockRenderPolicy::verticalAoPath(BedrockMaterial material)
{
	const char *key = semanticKey(material);
	if (!key || !*key) return std::string();
	return resolveDatasetAsset(key, "Materials/VERTICAL_AO.png", "VERTICAL_AO.png");
}

const char *BedrockRenderPolicy::semanticKey(BedrockMaterial material)
{
	switch (material)
	{
		case BedrockMaterial::Sand: return "SAND";
		default: return "";
	}
}

const char *BedrockRenderPolicy::name(BedrockMaterial material)
{
	switch (material)
	{
		case BedrockMaterial::Sand: return "BEDROCK_SAND";
		default: return "NONE";
	}
}

}
