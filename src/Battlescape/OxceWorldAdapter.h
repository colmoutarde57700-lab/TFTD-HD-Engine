#pragma once

#include <cstdint>

namespace OpenXcom
{

class SavedBattleGame;
struct RemasterWorldState;

/**
 * Transitional producer for RemasterWorldState.
 *
 * OXCE remains the gameplay/world simulation backend for now. This adapter is
 * the only place introduced by this migration step that is allowed to translate
 * OXCE tactical classes into the neutral remaster snapshot. It never reads a
 * Surface, PCK pixel, framebuffer or Legacy draw-order buffer.
 */
class OxceWorldAdapter
{
public:
	static void capture(SavedBattleGame *save, std::uint64_t revision, RemasterWorldState &out);
};

}
