#include "HdFacadeProfileRegistry.h"

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

bool mcdMatches(const HdFacadeProfileRule &rule, int id)
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

}

HdFacadeProfile HdFacadeProfileRegistry::profileFromString(const std::string &name)
{
	const std::string profile = lowerCopy(name);
	if (profile == "straight") return HdFacadeProfile::Straight;
	if (profile == "diagonal") return HdFacadeProfile::Diagonal;
	if (profile == "rounded_convex" || profile == "roundedconvex") return HdFacadeProfile::RoundedConvex;
	if (profile == "rounded_concave" || profile == "roundedconcave") return HdFacadeProfile::RoundedConcave;
	if (profile == "oval_segment" || profile == "ovalsegment") return HdFacadeProfile::OvalSegment;
	return HdFacadeProfile::None;
}

const char *HdFacadeProfileRegistry::profileName(HdFacadeProfile profile)
{
	switch (profile)
	{
	case HdFacadeProfile::Straight: return "straight";
	case HdFacadeProfile::Diagonal: return "diagonal";
	case HdFacadeProfile::RoundedConvex: return "rounded_convex";
	case HdFacadeProfile::RoundedConcave: return "rounded_concave";
	case HdFacadeProfile::OvalSegment: return "oval_segment";
	default: return "none";
	}
}

const std::vector<HdFacadeProfileRule> &HdFacadeProfileRegistry::rules()
{
	static bool loaded = false;
	static std::vector<HdFacadeProfileRule> result;
	if (loaded) return result;
	loaded = true;

	const std::string filename = "Ruleset/TFTD_HD_facade_profiles.yml";
	if (!FileMap::fileExists(filename)) return result;

	try
	{
		const YAML::YamlRootNodeReader root = FileMap::getYAML(filename);
		for (const auto &reader : root["hdFacadeProfiles"].children())
		{
			HdFacadeProfileRule rule;
			std::string profile;
			reader.tryRead("dataset", rule.dataset);
			reader.tryRead("profile", profile);
			reader.tryRead("asset", rule.asset);
			reader.tryRead("mcd", rule.mcd);
			reader.tryRead("bulgePermille", rule.bulgePermille);
			reader.tryRead("radiusPermille", rule.radiusPermille);
			std::vector<int> range;
			reader.tryRead("mcdRange", range);

			rule.dataset = lowerCopy(rule.dataset);
			rule.profile = profileFromString(profile);
			if (range.size() >= 2)
			{
				rule.mcdMin = std::min(range[0], range[1]);
				rule.mcdMax = std::max(range[0], range[1]);
			}
			rule.bulgePermille = std::max(0, rule.bulgePermille);
			rule.radiusPermille = std::max(1, rule.radiusPermille);
			if (rule.dataset.empty() || rule.profile == HdFacadeProfile::None) continue;
			result.push_back(rule);
		}
		Log(LOG_INFO) << "[HD FACADE PROFILE REGISTRY V1] loaded " << result.size()
			<< " presentation bindings from " << filename;
	}
	catch (const std::exception &e)
	{
		Log(LOG_WARNING) << "[HD FACADE PROFILE REGISTRY V1] failed to read " << filename << ": " << e.what();
	}
	return result;
}

HdFacadeProfileBinding HdFacadeProfileRegistry::resolve(const MapData *data)
{
	HdFacadeProfileBinding result;
	if (!data || !data->getDataset()) return result;
	const std::string dataset = lowerCopy(data->getDataset()->getName());
	const int id = data->getDatasetIndex();
	for (const HdFacadeProfileRule &rule : rules())
	{
		if (rule.dataset != dataset || !mcdMatches(rule, id)) continue;
		result.profile = rule.profile;
		result.asset = rule.asset;
		result.bulgePermille = rule.bulgePermille;
		result.radiusPermille = rule.radiusPermille;
		result.valid = true;
		return result;
	}
	return result;
}

}
