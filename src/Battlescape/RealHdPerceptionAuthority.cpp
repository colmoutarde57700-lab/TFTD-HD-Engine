/*
 * REAL HD PERCEPTION AUTHORITY V1
 */
#include "RealHdPerceptionAuthority.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <string>

#include "Pathfinding.h"
#include "Position.h"
#include "../Engine/Logger.h"
#include "../Engine/Options.h"
#include "../Savegame/BattleUnit.h"
#include "../Mod/Armor.h"

namespace OpenXcom
{
namespace RealHdPerceptionAuthority
{

namespace
{
constexpr float DEG_TO_RAD = 0.01745329251994329577f;

bool isStockProfileArmor(const std::string &armorType)
{
	return armorType == "STR_NONE_UC" ||
		armorType == "STR_PLASTIC_AQUA_ARMOR_UC" ||
		armorType == "STR_ION_ARMOR_UC" ||
		armorType == "STR_MAGNETIC_ION_ARMOR_UC";
}

float configuredHalfAngleForArmor(const std::string &armorType, bool &stockProfile)
{
	stockProfile = true;
	if (armorType == "STR_NONE_UC")
		return Options::hdFovBaseHalfAngleMilliDeg / 1000.0f;
	if (armorType == "STR_PLASTIC_AQUA_ARMOR_UC")
		return Options::hdFovPlasticAquaHalfAngleMilliDeg / 1000.0f;
	if (armorType == "STR_ION_ARMOR_UC")
		return Options::hdFovIonHalfAngleMilliDeg / 1000.0f;
	if (armorType == "STR_MAGNETIC_ION_ARMOR_UC")
		return Options::hdFovMagneticIonHalfAngleMilliDeg / 1000.0f;

	stockProfile = false;
	return 45.0f; // exact upstream/OXCE total sector for unknown/modded armors.
}

void logProfileOnce(const std::string &armorType, float halfAngle, bool stockProfile)
{
	static std::set<std::string> logged;
	const std::string key = armorType + (stockProfile ? ":stock" : ":fallback");
	if (!logged.insert(key).second)
		return;

	Log(LOG_INFO) << "[REAL HD FOV ARMOR V1][PROFILE] armor=" << armorType
		<< " halfAngleDeg=" << halfAngle
		<< " totalFovDeg=" << (halfAngle * 2.0f)
		<< " source=" << (stockProfile ? "CONFIGURABLE_STOCK_PROFILE" : "UPSTREAM_90_FALLBACK")
		<< " helmetCoreCoupling=FOV_DERIVED detectionAuthority=REAL_HD_SCENE";
}
}

bool enabledFor(const BattleUnit *unit)
{
	return Options::hdGraphics && unit && !unit->isOut() &&
		unit->getFaction() == FACTION_PLAYER && unit->getArmor() &&
		isStockProfileArmor(unit->getArmor()->getType());
}

float halfAngleDeg(const BattleUnit *unit)
{
	if (!unit || !unit->getArmor())
		return 45.0f;

	bool stockProfile = false;
	float angle = configuredHalfAngleForArmor(unit->getArmor()->getType(), stockProfile);
	// No REAL HD armor is ever allowed to become rear-looking automatically.
	angle = std::max(0.0f, std::min(90.0f, angle));
	logProfileOnce(unit->getArmor()->getType(), angle, stockProfile);
	return angle;
}

float angleDeg(const BattleUnit *unit, const Position &target, bool useTurretDirection)
{
	if (!unit || !unit->getArmor())
		return 180.0f;

	const int direction = (useTurretDirection ? unit->getTurretDirection() : unit->getDirection()) & 7;
	Position forward;
	Pathfinding::directionToVector(direction, &forward);
	const float forwardLength = std::sqrt((float)(forward.x * forward.x + forward.y * forward.y));
	if (forwardLength <= 0.0f)
		return 180.0f;
	const float forwardX = forward.x / forwardLength;
	const float forwardY = forward.y / forwardLength;

	float best = 180.0f;
	const int unitSize = unit->getArmor()->getSize();
	for (int x = 0; x < unitSize; ++x)
	{
		for (int y = 0; y < unitSize; ++y)
		{
			const float dx = (float)(target.x - (unit->getPosition().x + x));
			const float dy = (float)(target.y - (unit->getPosition().y + y));
			const float distance = std::sqrt(dx * dx + dy * dy);
			if (distance <= 0.0001f)
				return 0.0f;
			const float dot = std::max(-1.0f, std::min(1.0f,
				(dx / distance) * forwardX + (dy / distance) * forwardY));
			best = std::min(best, std::acos(dot) / DEG_TO_RAD);
		}
	}
	return best;
}

bool contains(const BattleUnit *unit, const Position &target, bool useTurretDirection)
{
	if (!unit || !unit->getArmor())
		return false;
	bool stockProfile = false;
	const float half = std::clamp(configuredHalfAngleForArmor(
		unit->getArmor()->getType(), stockProfile), 0.0f, 90.0f);
	const float cosine = std::cos(half * DEG_TO_RAD);
	Position forward;
	Pathfinding::directionToVector(
		(useTurretDirection ? unit->getTurretDirection() : unit->getDirection()) & 7,
		&forward);
	const float forwardLengthSq = float(forward.x * forward.x + forward.y * forward.y);
	if (forwardLengthSq <= 0.0f) return false;
	const int size = unit->getArmor()->getSize();
	for (int x = 0; x < size; ++x)
		for (int y = 0; y < size; ++y)
		{
			const float dx = float(target.x - unit->getPosition().x - x);
			const float dy = float(target.y - unit->getPosition().y - y);
			const float distanceSq = dx * dx + dy * dy;
			if (distanceSq <= 0.0f) return true;
			const float projection = dx * forward.x + dy * forward.y;
			if (projection >= 0.0f && projection * projection + 1e-4f >=
				cosine * cosine * forwardLengthSq * distanceSq) return true;
		}
	return false;
}

}
}
