#include "RealHdWorldPresentation.h"

#include "../Savegame/Tile.h"
#include "../Mod/MapData.h"

namespace OpenXcom
{

RealHdWorldKnowledge RealHdWorldPresentation::knowledge(const Tile *tile, TilePart part)
{
	if (!tile)
	{
		// World presentation fails closed when no world owner exists. Callers that
		// intentionally draw UI/presentation overlays must use PresentationOverlay
		// instead of smuggling a null tile through the world visibility contract.
		return RealHdWorldKnowledge::Unknown;
	}

	bool discovered = false;
	if (part == O_WESTWALL || part == O_NORTHWALL)
		discovered = tile->isDiscovered(part);
	else
		// O_OBJECT has no independent persisted discovery state in OXCE.  It
		// follows the owning floor, matching the validated Real HD source gate.
		discovered = tile->isDiscovered(O_FLOOR);

	if (!discovered) return RealHdWorldKnowledge::Unknown;
	return tile->getVisible() > 0 ? RealHdWorldKnowledge::VisibleNow : RealHdWorldKnowledge::Known;
}

bool RealHdWorldPresentation::allows(const Tile *tile, TilePart part, RealHdWorldPresentationClass presentationClass)
{
	if (presentationClass == RealHdWorldPresentationClass::PresentationOverlay)
		return true;

	const RealHdWorldKnowledge state = knowledge(tile, part);
	if (presentationClass == RealHdWorldPresentationClass::DynamicWorldEffect)
	{
		// Dynamic effects describe the world *now*. Replaying them from memory
		// would leak current tactical information through smoke, light, foam,
		// wake or any future transient effect. They therefore require current LOS.
		return state == RealHdWorldKnowledge::VisibleNow;
	}

	// Persistent geometry may be presented from legitimate map memory. Unknown
	// geometry remains fully suppressed. This preserves the validated V1A cursor
	// and terrain presentation contract without duplicating discovery rules.
	return state != RealHdWorldKnowledge::Unknown;
}

}
