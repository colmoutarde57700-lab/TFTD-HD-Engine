#pragma once

#include "Position.h"
#include "../Mod/MapData.h"

namespace OpenXcom
{

class BattleUnit;
class SavedBattleGame;
class Tile;

/**
 * REAL HD VISIBILITY SOLVER V1A — audit-only geometric surface LOS.
 *
 * V1A deliberately owns no gameplay and no presentation decisions.  It traces
 * from one aquanaut eye to one candidate top surface and reports whether the
 * segment is geometrically occluded by the currently modelled Real-HD terrain:
 * BEDROCK top/step geometry plus ordinary flat floors.  Walls, objects, doors,
 * smoke, range and view cone remain outside this first audit stage.
 */
struct RealHdVisibilityAuditResult
{
	bool supported = false;
	bool visible = false;
	bool targetBedrock = false;
	bool hit = false;
	Position hitTile;
	int testedTriangles = 0;
};

class RealHdVisibilitySolver
{
public:
	static RealHdVisibilityAuditResult auditSurfaceLos(const SavedBattleGame *save, const BattleUnit *observer, const Tile *target, TilePart part);
};

}
