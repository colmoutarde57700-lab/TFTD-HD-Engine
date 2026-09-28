#pragma once

#include "Options.h"
#include <array>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <sstream>
#include <string>
#include <vector>

namespace OpenXcom
{

enum class HdMaterialProfile : unsigned char
{
	Auto = 0,
	Terrain,
	Organic,
	Rock,
	Wreck,
	StructureExterior,
	StructureInterior,
	CraftExterior,
	CraftInterior,
	Unit,
	Item,
	Effect,
	Neutral,
	Count
};

struct HdMaterialGradeParams
{
	int tintR = 1000;
	int tintG = 1000;
	int tintB = 1000;
	int exposure = 1000;
	int contrast = 1000;
	int saturation = 1000;
};

inline std::string hdMaterialLower(std::string value)
{
	std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c){ return (char)std::tolower(c); });
	return value;
}

inline const char *hdMaterialProfileKey(HdMaterialProfile p)
{
	switch (p)
	{
		case HdMaterialProfile::Terrain: return "terrain";
		case HdMaterialProfile::Organic: return "organic";
		case HdMaterialProfile::Rock: return "rock";
		case HdMaterialProfile::Wreck: return "wreck";
		case HdMaterialProfile::StructureExterior: return "structure_exterior";
		case HdMaterialProfile::StructureInterior: return "structure_interior";
		case HdMaterialProfile::CraftExterior: return "craft_exterior";
		case HdMaterialProfile::CraftInterior: return "craft_interior";
		case HdMaterialProfile::Unit: return "unit";
		case HdMaterialProfile::Item: return "item";
		case HdMaterialProfile::Effect: return "effect";
		case HdMaterialProfile::Neutral: return "neutral";
		default: return "auto";
	}
}

inline const char *hdMaterialProfileLabel(HdMaterialProfile p)
{
	switch (p)
	{
		case HdMaterialProfile::Terrain: return "TERRAIN";
		case HdMaterialProfile::Organic: return "ORGANIC / CORAL";
		case HdMaterialProfile::Rock: return "ROCK";
		case HdMaterialProfile::Wreck: return "WRECK / WOOD";
		case HdMaterialProfile::StructureExterior: return "STRUCTURE EXTERIOR";
		case HdMaterialProfile::StructureInterior: return "STRUCTURE INTERIOR";
		case HdMaterialProfile::CraftExterior: return "CRAFT EXTERIOR";
		case HdMaterialProfile::CraftInterior: return "CRAFT INTERIOR";
		case HdMaterialProfile::Unit: return "UNITS";
		case HdMaterialProfile::Item: return "ITEMS";
		case HdMaterialProfile::Effect: return "EFFECTS";
		case HdMaterialProfile::Neutral: return "NEUTRAL / FALLBACK";
		default: return "AUTO";
	}
}

inline const char *hdMaterialProfileDescription(HdMaterialProfile p)
{
	switch (p)
	{
		case HdMaterialProfile::Terrain: return "General HD ground / terrain surfaces";
		case HdMaterialProfile::Organic: return "Coral, weeds, organic environment";
		case HdMaterialProfile::Rock: return "Rock / mineral environment";
		case HdMaterialProfile::Wreck: return "Wreckage, debris, wood, ship remains";
		case HdMaterialProfile::StructureExterior: return "Exterior map structures";
		case HdMaterialProfile::StructureInterior: return "Covered/interior map structures";
		case HdMaterialProfile::CraftExterior: return "Craft / USO exterior surfaces";
		case HdMaterialProfile::CraftInterior: return "Craft / USO interior surfaces";
		case HdMaterialProfile::Unit: return "Aquanauts, aliens and other units";
		case HdMaterialProfile::Item: return "World items / equipment sprites";
		case HdMaterialProfile::Effect: return "Smoke, impacts, projectiles and effects";
		case HdMaterialProfile::Neutral: return "Unclassified HD world assets";
		default: return "Automatic routing";
	}
}

inline HdMaterialProfile hdMaterialProfileFromString(const std::string &raw)
{
	const std::string v = hdMaterialLower(raw);
	if (v == "terrain" || v == "ground") return HdMaterialProfile::Terrain;
	if (v == "organic" || v == "coral" || v == "vegetation") return HdMaterialProfile::Organic;
	if (v == "rock" || v == "mineral") return HdMaterialProfile::Rock;
	if (v == "wreck" || v == "wreckage" || v == "wood" || v == "debris") return HdMaterialProfile::Wreck;
	if (v == "structure_exterior" || v == "structure-exterior" || v == "exterior") return HdMaterialProfile::StructureExterior;
	if (v == "structure_interior" || v == "structure-interior" || v == "interior") return HdMaterialProfile::StructureInterior;
	if (v == "craft_exterior" || v == "craft-exterior" || v == "ufo_exterior" || v == "uso_exterior") return HdMaterialProfile::CraftExterior;
	if (v == "craft_interior" || v == "craft-interior" || v == "ufo_interior" || v == "uso_interior") return HdMaterialProfile::CraftInterior;
	if (v == "unit" || v == "units") return HdMaterialProfile::Unit;
	if (v == "item" || v == "items" || v == "equipment") return HdMaterialProfile::Item;
	if (v == "effect" || v == "effects" || v == "fx") return HdMaterialProfile::Effect;
	if (v == "neutral" || v == "fixed") return HdMaterialProfile::Neutral;
	return HdMaterialProfile::Auto;
}

inline bool hdMaterialContains(const std::string &s, const char *needle)
{
	return s.find(needle) != std::string::npos;
}

inline HdMaterialProfile hdMaterialProfileForDataset(const std::string &rawName, bool covered)
{
	const std::string n = hdMaterialLower(rawName);
	if (n.empty()) return HdMaterialProfile::Terrain;

	if (hdMaterialContains(n, "coral") || hdMaterialContains(n, "weed") || hdMaterialContains(n, "organic"))
		return HdMaterialProfile::Organic;
	if (hdMaterialContains(n, "rock")) return HdMaterialProfile::Rock;
	if (hdMaterialContains(n, "debris") || hdMaterialContains(n, "wreck") || hdMaterialContains(n, "asunk") || hdMaterialContains(n, "wood"))
		return HdMaterialProfile::Wreck;

	// TFTD craft / USO conventions. Covered craft surfaces are routed to the
	// interior profile when a dataset itself does not already distinguish them.
	if (hdMaterialContains(n, "uint")) return HdMaterialProfile::CraftInterior;
	if (hdMaterialContains(n, "uext") || hdMaterialContains(n, "ufobits") || hdMaterialContains(n, "urbits"))
		return HdMaterialProfile::CraftExterior;
	if (hdMaterialContains(n, "triton") || hdMaterialContains(n, "manta") || hdMaterialContains(n, "leviathan") || hdMaterialContains(n, "hammerhead"))
		return covered ? HdMaterialProfile::CraftInterior : HdMaterialProfile::CraftExterior;

	if (hdMaterialContains(n, "crypt") || hdMaterialContains(n, "xbase") || hdMaterialContains(n, "atlantis") ||
		hdMaterialContains(n, "pyramid") || hdMaterialContains(n, "liner") || hdMaterialContains(n, "cargo"))
		return covered ? HdMaterialProfile::StructureInterior : HdMaterialProfile::StructureExterior;

	if (hdMaterialContains(n, "sand") || hdMaterialContains(n, "seabed") || hdMaterialContains(n, "ground") || hdMaterialContains(n, "blank"))
		return HdMaterialProfile::Terrain;

	return covered ? HdMaterialProfile::StructureInterior : HdMaterialProfile::Terrain;
}

inline HdMaterialProfile hdMaterialProfileForSurfaceSet(const std::string &rawName)
{
	const std::string n = hdMaterialLower(rawName);
	if (hdMaterialContains(n, "smoke") || hdMaterialContains(n, "hit") || hdMaterialContains(n, "projectile") ||
		hdMaterialContains(n, "bullet") || hdMaterialContains(n, "explosion") || n == "x1")
		return HdMaterialProfile::Effect;
	if (hdMaterialContains(n, "bigobs") || hdMaterialContains(n, "floorob") || hdMaterialContains(n, "handob") ||
		hdMaterialContains(n, "smallobs") || hdMaterialContains(n, "item"))
		return HdMaterialProfile::Item;
	return HdMaterialProfile::Neutral;
}

inline HdMaterialProfile hdMaterialProfileForAssetPath(const std::string &rawPath)
{
	const std::string p = hdMaterialLower(rawPath);
	if (hdMaterialContains(p, "/units/") || hdMaterialContains(p, "/unitparts/") || hdMaterialContains(p, "/aquanaut") || hdMaterialContains(p, "/alienunits/"))
		return HdMaterialProfile::Unit;
	if (hdMaterialContains(p, "/items/") || hdMaterialContains(p, "/equipment/")) return HdMaterialProfile::Item;
	if (hdMaterialContains(p, "smoke") || hdMaterialContains(p, "projectile") || hdMaterialContains(p, "explosion") || hdMaterialContains(p, "/hit"))
		return HdMaterialProfile::Effect;

	const std::string marker = "/terrain/";
	const size_t pos = p.find(marker);
	if (pos != std::string::npos)
	{
		const size_t start = pos + marker.size();
		const size_t end = p.find('/', start);
		if (end != std::string::npos && end > start)
			return hdMaterialProfileForDataset(p.substr(start, end - start), false);
	}
	return HdMaterialProfile::Neutral;
}

inline constexpr size_t HdMaterialProfileStorageCount = (size_t)HdMaterialProfile::Count - 1; // excludes Auto
inline constexpr int HdMaterialDepthCount = 4;

inline size_t hdMaterialStorageIndex(HdMaterialProfile p)
{
	if (p == HdMaterialProfile::Auto) p = HdMaterialProfile::Neutral;
	const size_t i = (size_t)p;
	return (i >= 1 && i < (size_t)HdMaterialProfile::Count) ? i - 1 : (size_t)HdMaterialProfile::Neutral - 1;
}

inline bool hdMaterialParamsNeutral(const HdMaterialGradeParams &p)
{
	return p.tintR == 1000 && p.tintG == 1000 && p.tintB == 1000 && p.exposure == 1000 && p.contrast == 1000 && p.saturation == 1000;
}

inline std::array<std::array<HdMaterialGradeParams, HdMaterialDepthCount>, HdMaterialProfileStorageCount> &hdMaterialGradeTable()
{
	static std::array<std::array<HdMaterialGradeParams, HdMaterialDepthCount>, HdMaterialProfileStorageCount> table{};
	return table;
}

inline std::string &hdMaterialGradeLoadedConfig()
{
	static std::string value;
	return value;
}

inline bool &hdMaterialGradeInitialized()
{
	static bool initialized = false;
	return initialized;
}

inline void hdMaterialGradeResetMemory()
{
	auto &table = hdMaterialGradeTable();
	for (auto &profile : table)
		for (auto &depth : profile)
			depth = HdMaterialGradeParams();
}

inline void hdMaterialGradeEnsureLoaded()
{
	if (hdMaterialGradeInitialized() && hdMaterialGradeLoadedConfig() == Options::hdMaterialGradeConfig) return;
	hdMaterialGradeResetMemory();
	hdMaterialGradeInitialized() = true;
	hdMaterialGradeLoadedConfig() = Options::hdMaterialGradeConfig;
	if (Options::hdMaterialGradeConfig.empty()) return;

	std::string data = Options::hdMaterialGradeConfig;
	if (data.compare(0, 3, "v1|") == 0) data.erase(0, 3);
	std::replace(data.begin(), data.end(), ',', ' ');
	std::istringstream in(data);
	auto &table = hdMaterialGradeTable();
	for (size_t profile = 0; profile < HdMaterialProfileStorageCount; ++profile)
	{
		for (int depth = 0; depth < HdMaterialDepthCount; ++depth)
		{
			HdMaterialGradeParams p;
			if (!(in >> p.tintR >> p.tintG >> p.tintB >> p.exposure >> p.contrast >> p.saturation)) return;
			table[profile][depth] = p;
		}
	}
}

inline void hdMaterialGradeSerialize()
{
	auto &table = hdMaterialGradeTable();
	std::ostringstream out;
	out << "v1|";
	bool first = true;
	for (size_t profile = 0; profile < HdMaterialProfileStorageCount; ++profile)
	{
		for (int depth = 0; depth < HdMaterialDepthCount; ++depth)
		{
			const HdMaterialGradeParams &p = table[profile][depth];
			const int values[6] = {p.tintR,p.tintG,p.tintB,p.exposure,p.contrast,p.saturation};
			for (int v : values)
			{
				if (!first) out << ',';
				first = false;
				out << v;
			}
		}
	}
	Options::hdMaterialGradeConfig = out.str();
	hdMaterialGradeLoadedConfig() = Options::hdMaterialGradeConfig;
}

inline HdMaterialGradeParams hdMaterialGradeGet(HdMaterialProfile profile, int depth)
{
	hdMaterialGradeEnsureLoaded();
	depth = std::max(0, std::min(HdMaterialDepthCount - 1, depth));
	return hdMaterialGradeTable()[hdMaterialStorageIndex(profile)][depth];
}

inline void hdMaterialGradeSet(HdMaterialProfile profile, int depth, const HdMaterialGradeParams &params)
{
	hdMaterialGradeEnsureLoaded();
	depth = std::max(0, std::min(HdMaterialDepthCount - 1, depth));
	hdMaterialGradeTable()[hdMaterialStorageIndex(profile)][depth] = params;
	hdMaterialGradeSerialize();
}

inline void hdMaterialGradeResetProfileDepth(HdMaterialProfile profile, int depth)
{
	hdMaterialGradeSet(profile, depth, HdMaterialGradeParams());
}

inline void hdMaterialGradeResetAll()
{
	hdMaterialGradeResetMemory();
	hdMaterialGradeInitialized() = true;
	hdMaterialGradeSerialize();
}

inline float hdMaterialGradeFloat(int permille)
{
	return (float)permille / 1000.0f;
}

} // namespace OpenXcom
