#pragma once

namespace OpenXcom
{
class SavedBattleGame;
class RealHdPhysicalGeometry;

// Translates the resolved MAP/MCD tiles into REAL HD physical geometry.
// This does not repair maps, infer helper walls or call OXCE visibility code.
class RealHdMapSceneBuilder
{
public:
	static bool build(const SavedBattleGame *save, RealHdPhysicalGeometry &scene);
};
}
