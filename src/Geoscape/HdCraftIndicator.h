#pragma once
#include "../Engine/Surface.h"

namespace OpenXcom
{
class Craft;
enum class HdCraftIndicatorPart { Hull, Damage, Shield };

// A presenter of game facts. It neither calls a native sprite compositor nor
// borrows the native craft Surface as a silhouette mask.
class HdCraftIndicator : public Surface
{
    const Craft *_craft;
    HdCraftIndicatorPart _part;
    const int *_damageColor;
    const int *_shieldMin;
    const int *_shieldMax;
    const int *_craftMin;
public:
    HdCraftIndicator(int width, int height, int x, int y, const Craft *craft,
        HdCraftIndicatorPart part, const int *damageColor, const int *shieldMin,
        const int *shieldMax, const int *craftMin);
    void composeHd(HdCanvas &canvas, HdImageCache &images) override;
};
}
