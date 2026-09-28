#include "HdCraftIndicator.h"
#include "../Engine/HdUiImage.h"
#include "../Engine/HdImage.h"
#include "../Engine/HdAlphaRows.h"
#include "../Savegame/Craft.h"
#include "../Engine/Exception.h"

namespace OpenXcom
{
HdCraftIndicator::HdCraftIndicator(int width, int height, int x, int y, const Craft *craft,
    HdCraftIndicatorPart part, const int *damageColor, const int *shieldMin,
    const int *shieldMax, const int *craftMin) : Surface(width, height, x, y),
    _craft(craft), _part(part), _damageColor(damageColor), _shieldMin(shieldMin),
    _shieldMax(shieldMax), _craftMin(craftMin)
{
}

void HdCraftIndicator::composeHd(HdCanvas &canvas, HdImageCache &images)
{
    if (!isDisplayVisible()) return;
    if (!_craft) throw Exception("[HD UI ERROR] Craft indicator has no craft");
    auto colors = std::make_shared<std::array<HdRgba, 256>>();
    for (int i = 0; i < 256; ++i) (*colors)[i] = getHdColor(i);
    const auto definition = hdUiFrameDefinition("INTICON.PCK", _craft->getSkinSprite() + 11, 32, 40);
    HdCanvas content(getWidth(), getHeight());
    HdRect crop{0, 0, double(getWidth()), double(getHeight())};
    HdImageStyle style;
    if (_part != HdCraftIndicatorPart::Hull)
    {
        double fraction = 0;
        if (_part == HdCraftIndicatorPart::Damage)
            fraction = std::clamp(_craft->getDamagePercentage() / 100.0, 0.0, 1.0);
        else if (_craft->getShieldCapacity() > 0)
            fraction = std::clamp(double(_craft->getShield()) / _craft->getShieldCapacity(), 0.0, 1.0);
        if (fraction == 0) return;
        // Resolve and validate through the same path used for actual drawing.
        HdCanvas selected(getWidth(), getHeight());
        hdAppendUiImage(selected, images, definition, crop, crop, colors);
        if (selected.commands().size() != 1 || selected.commands()[0].op != HdCanvasOp::Image)
            throw Exception("[HD UI ERROR] Craft indicator did not select one PNG");
        const auto &image = images.require(selected.commands()[0].image.path);
        const unsigned right = std::min(image.width, unsigned(std::ceil(crop.w * image.width / definition.width)));
        const unsigned bottom = std::min(image.height, unsigned(std::ceil(crop.h * image.height / definition.height)));
        const auto rows = hdAlphaRows(image.rgba, image.width, image.height, 0, 0, right, bottom);
        const auto range = hdAlphaFraction(rows, fraction, _part == HdCraftIndicatorPart::Shield);
        if (range.begin == range.end) return;
        crop.y = double(range.begin) * definition.height / image.height;
        crop.h = double(range.end - range.begin) * definition.height / image.height;
        style.tint = true;
        if (_part == HdCraftIndicatorPart::Damage)
        {
            style.flatTint = true;
            style.tintColor = getHdColor(static_cast<Uint8>(*_damageColor));
            for (int i = 1; i < 256; ++i) (*colors)[i] = style.tintColor;
        }
        else
        {
            style.tintColor = getHdColor(static_cast<Uint8>(*_shieldMin));
            for (int i = 1; i < 256; ++i)
                (*colors)[i] = getHdColor(static_cast<Uint8>(std::min(*_shieldMax, *_shieldMin + i - *_craftMin)));
        }
    }
    hdAppendUiImage(content, images, definition, crop, crop, colors, false, false, style);
    composeHdLayer(canvas, content);
}
}
