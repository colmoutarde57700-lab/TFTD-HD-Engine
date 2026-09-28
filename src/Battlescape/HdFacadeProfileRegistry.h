#pragma once

#include <string>
#include <vector>
#include "../Mod/MapData.h"

namespace OpenXcom
{

enum class HdFacadeProfile
{
	None = 0,
	Straight,
	Diagonal,
	RoundedConvex,
	RoundedConcave,
	OvalSegment
};

struct HdFacadeProfileRule
{
	std::string dataset;
	std::vector<int> mcd;
	int mcdMin = -1;
	int mcdMax = -1;
	HdFacadeProfile profile = HdFacadeProfile::None;
	std::string asset;
	int bulgePermille = 1000;
	int radiusPermille = 1000;
};

struct HdFacadeProfileBinding
{
	HdFacadeProfile profile = HdFacadeProfile::None;
	std::string asset;
	int bulgePermille = 1000;
	int radiusPermille = 1000;
	bool valid = false;
};

/**
 * Presentation-only facade profile lookup. It never changes the logical wall
 * stored in MAP/MCD and therefore never changes collision, pathfinding or LOS.
 */
class HdFacadeProfileRegistry
{
public:
	static const std::vector<HdFacadeProfileRule> &rules();
	static HdFacadeProfileBinding resolve(const MapData *data);
	static HdFacadeProfile profileFromString(const std::string &name);
	static const char *profileName(HdFacadeProfile profile);
};

}
