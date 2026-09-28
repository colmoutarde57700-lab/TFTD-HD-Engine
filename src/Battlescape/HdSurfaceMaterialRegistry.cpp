#include "HdSurfaceMaterialRegistry.h"

#include "../Engine/FileMap.h"
#include "../Engine/Logger.h"
#include "../Engine/Yaml.h"
#include "../Mod/MapDataSet.h"

#include <algorithm>
#include <cctype>

namespace OpenXcom
{
namespace
{

std::string lowerCopy(std::string value)
{
	std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return (char)std::tolower(c); });
	return value;
}

std::string upperCopy(std::string value)
{
	std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return (char)std::toupper(c); });
	return value;
}

unsigned partBit(TilePart part)
{
	return (part >= O_FLOOR && part < O_MAX) ? (1u << (unsigned)part) : 0u;
}

unsigned parseParts(const std::vector<std::string> &parts)
{
	if (parts.empty()) return 0x0Fu;
	unsigned mask = 0;
	for (std::string part : parts)
	{
		part = lowerCopy(part);
		if (part == "floor" || part == "o_floor") mask |= partBit(O_FLOOR);
		else if (part == "westwall" || part == "west_wall" || part == "o_westwall") mask |= partBit(O_WESTWALL);
		else if (part == "northwall" || part == "north_wall" || part == "o_northwall") mask |= partBit(O_NORTHWALL);
		else if (part == "object" || part == "o_object") mask |= partBit(O_OBJECT);
		else if (part == "all" || part == "*") mask |= 0x0Fu;
	}
	return mask;
}

bool mcdMatches(const HdSurfaceMaterialRule &rule, int id)
{
	if (!rule.mcd.empty())
		return std::find(rule.mcd.begin(), rule.mcd.end(), id) != rule.mcd.end();
	if (rule.mcdMin >= 0 || rule.mcdMax >= 0)
	{
		const int lo = rule.mcdMin >= 0 ? rule.mcdMin : 0;
		const int hi = rule.mcdMax >= 0 ? rule.mcdMax : 0x7fffffff;
		return id >= lo && id <= hi;
	}
	return true;
}

std::string materialPathV2(const std::string &dataset, const std::string &material, const char *leaf)
{
	if (dataset.empty() || material.empty()) return std::string();
	return std::string("Resources/TFTD_HD/RealHD/Datasets/") + upperCopy(dataset) + "/Materials/" + lowerCopy(material) + "/" + leaf;
}

std::string legacyMaterialRoot(const std::string &material)
{
	return material.empty() ? std::string() : std::string("Resources/TFTD_HD/Surfaces/Materials/") + lowerCopy(material);
}

}

const std::vector<HdSurfaceMaterialRule> &HdSurfaceMaterialRegistry::rules()
{
	static bool loaded = false;
	static std::vector<HdSurfaceMaterialRule> result;
	if (loaded) return result;
	loaded = true;

	const std::string filename = "Ruleset/TFTD_HD_surface_materials.yml";
	if (!FileMap::fileExists(filename)) return result;

	try
	{
		const YAML::YamlRootNodeReader root = FileMap::getYAML(filename);
		for (const auto &reader : root["hdSurfaceMaterials"].children())
		{
			HdSurfaceMaterialRule rule;
			reader.tryRead("material", rule.material);
			reader.tryRead("dataset", rule.dataset);
			reader.tryRead("mcd", rule.mcd);
			std::vector<int> range;
			reader.tryRead("mcdRange", range);
			std::vector<std::string> parts;
			reader.tryRead("parts", parts);

			rule.material = lowerCopy(rule.material);
			rule.dataset = lowerCopy(rule.dataset);
			if (range.size() >= 2)
			{
				rule.mcdMin = std::min(range[0], range[1]);
				rule.mcdMax = std::max(range[0], range[1]);
			}
			rule.partMask = parseParts(parts);
			if (rule.material.empty() || rule.dataset.empty() || rule.partMask == 0)
				continue;
			result.push_back(rule);
		}
		Log(LOG_INFO) << "[HD SURFACE MATERIAL REGISTRY V2] loaded " << result.size()
			<< " presentation bindings from " << filename
			<< " canonicalRoot=Resources/TFTD_HD/RealHD/Datasets/<DATASET>/Materials/<MATERIAL>/"
			<< " legacyRoot=COMPAT_ONLY";
	}
	catch (const std::exception &e)
	{
		Log(LOG_WARNING) << "[HD SURFACE MATERIAL REGISTRY V1] failed to read " << filename << ": " << e.what();
	}
	return result;
}

std::string HdSurfaceMaterialRegistry::resolve(const MapData *data, TilePart part)
{
	if (!data || !data->getDataset()) return std::string();
	const std::string dataset = lowerCopy(data->getDataset()->getName());
	const int id = data->getDatasetIndex();
	const unsigned bit = partBit(part);
	for (const HdSurfaceMaterialRule &rule : rules())
	{
		if (rule.dataset != dataset || !(rule.partMask & bit) || !mcdMatches(rule, id)) continue;
		return rule.material;
	}
	return std::string();
}

std::string HdSurfaceMaterialRegistry::rootPath(const std::string &dataset, const std::string &material)
{
	if (dataset.empty() || material.empty()) return std::string();
	return std::string("Resources/TFTD_HD/RealHD/Datasets/") + upperCopy(dataset) + "/Materials/" + lowerCopy(material);
}
std::string HdSurfaceMaterialRegistry::topBasePath(const std::string &dataset, const std::string &material) { return materialPathV2(dataset, material, "TOP_BASE.png"); }
std::string HdSurfaceMaterialRegistry::topNormalPath(const std::string &dataset, const std::string &material) { return materialPathV2(dataset, material, "TOP_NORMAL_DX.png"); }
std::string HdSurfaceMaterialRegistry::topRoughnessPath(const std::string &dataset, const std::string &material) { return materialPathV2(dataset, material, "TOP_ROUGHNESS.png"); }
std::string HdSurfaceMaterialRegistry::topAoPath(const std::string &dataset, const std::string &material) { return materialPathV2(dataset, material, "TOP_AO.png"); }
std::string HdSurfaceMaterialRegistry::verticalBasePath(const std::string &dataset, const std::string &material) { return materialPathV2(dataset, material, "VERTICAL_BASE.png"); }
std::string HdSurfaceMaterialRegistry::verticalNormalPath(const std::string &dataset, const std::string &material) { return materialPathV2(dataset, material, "VERTICAL_NORMAL_DX.png"); }
std::string HdSurfaceMaterialRegistry::verticalRoughnessPath(const std::string &dataset, const std::string &material) { return materialPathV2(dataset, material, "VERTICAL_ROUGHNESS.png"); }
std::string HdSurfaceMaterialRegistry::verticalAoPath(const std::string &dataset, const std::string &material) { return materialPathV2(dataset, material, "VERTICAL_AO.png"); }
std::string HdSurfaceMaterialRegistry::legacyRootPath(const std::string &material) { return legacyMaterialRoot(material); }

}
