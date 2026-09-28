#pragma once

#include <cstdint>

namespace OpenXcom
{

class Tile;
enum TilePart : int;

/**
 * Common Real HD world-presentation contract.
 *
 * OXCE remains authoritative for gameplay knowledge (discovery / current
 * visibility).  This adapter translates that information into presentation
 * semantics once, before any renderer-specific pass decides how to draw.
 * No shader/effect is allowed to invent its own "discovered" exception.
 */
enum class RealHdWorldKnowledge : std::uint8_t
{
	Unknown = 0,
	Known = 1,
	VisibleNow = 2
};

enum class RealHdWorldPresentationClass : std::uint8_t
{
	PersistentGeometry = 0,
	DynamicWorldEffect = 1,
	PresentationOverlay = 2
};

class RealHdWorldPresentation
{
public:
	/// Translate one OXCE tile part into the common Real HD knowledge state.
	static RealHdWorldKnowledge knowledge(const Tile *tile, TilePart part);

	/// Central admission gate for world presentation.
	static bool allows(const Tile *tile, TilePart part, RealHdWorldPresentationClass presentationClass);
};

}
