#pragma once
/*
 * REAL HD PERCEPTION AUTHORITY V1.
 *
 * Owns the player-facing horizontal perception sector used by REAL HD.  The
 * authority is deliberately independent from lighting and rendering: it only
 * answers whether a world position lies inside the observer's equipment-driven
 * view sector.  OXCE voxel LOS, range, smoke and visibility rules remain the
 * gameplay authority after this angular gate.
 */

namespace OpenXcom
{

class BattleUnit;
class Position;

namespace RealHdPerceptionAuthority
{

// True only for living stock-TFTD Aquanauts while REAL HD is enabled.
// Unknown/modded player armors and everyone else use the exact upstream/OXCE
// sector implementation until an explicit profile is added.
bool enabledFor(const BattleUnit *unit);

// Configurable horizontal half-angle in degrees for this unit's armor.
// Unknown/modded player armors deliberately fall back to 45 degrees so they
// retain the upstream 90-degree total sector until explicitly profiled.
float halfAngleDeg(const BattleUnit *unit);

// Smallest horizontal angle in degrees between the observer's body/turret axis
// and the target, accounting for multi-tile observer footprints.
float angleDeg(const BattleUnit *unit, const Position &target, bool useTurretDirection);

// Equipment-driven sector test.  Multi-tile units are tested from every tile,
// matching BattleUnit::checkViewSector's historical ownership.
bool contains(const BattleUnit *unit, const Position &target, bool useTurretDirection);

}
}
