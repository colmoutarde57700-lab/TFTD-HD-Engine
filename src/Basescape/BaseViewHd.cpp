#include "BaseView.h"
#include "../Engine/HdUiImage.h"
#include "../Engine/Exception.h"
#include "../Interface/Text.h"
#include "../Mod/RuleBaseFacility.h"
#include "../Mod/Texture.h"
#include "../Savegame/Base.h"
#include "../Savegame/BaseFacility.h"
#include "../Savegame/Craft.h"
#include <sstream>

namespace OpenXcom
{
void BaseView::composeHd(HdCanvas &canvas, HdImageCache &images)
{
    if (!isDisplayVisible()) return;
    if (!_base) throw Exception("[HD UI ERROR] Base view has no base");
    HdCanvas content(getWidth(), getHeight());
    auto palette = std::make_shared<std::array<HdRgba, 256>>();
    for (int i = 0; i < 256; ++i) (*palette)[i] = getHdColor(i);
    (*palette)[0] = {0, 0, 0, 0};
    const auto frame = [&](int identity, int x, int y)
    {
        hdAppendUiImage(content, images, hdUiFrameDefinition("BASEBITS.PCK", identity, 32, 40),
            {0, 0, 32, 40}, {double(x), double(y), 32, 40}, palette);
    };
    const int grid = _base->getGlobeTexture() ? _base->getGlobeTexture()->getBaseGridSprite() : 0;
    for (int x = 0; x < BASE_SIZE; ++x)
        for (int y = 0; y < BASE_SIZE; ++y) frame(grid, x * GRID_SIZE, y * GRID_SIZE);

    for (const auto *facility : *_base->getFacilities())
    {
        const auto *rule = facility->getRules();
        const int outline = rule->isSmall() ? 3 : rule->getSizeX() * rule->getSizeY();
        int index = 0;
        for (int y = facility->getY(); y < facility->getY() + rule->getSizeY(); ++y)
            for (int x = facility->getX(); x < facility->getX() + rule->getSizeX(); ++x)
                frame(rule->getSpriteShape() + index++ + (facility->getBuildTime() == 0 ? 0 : outline), x * GRID_SIZE, y * GRID_SIZE);
    }
    // Connectors overlay shells, then facility interiors overlay connectors.
    for (const auto *facility : *_base->getFacilities())
    {
        const auto *rule = facility->getRules();
        if (!facility->isBuiltOrHadPreviousFacility() || rule->connectorsDisabled()) continue;
        const int right = facility->getX() + rule->getSizeX();
        if (right < BASE_SIZE)
            for (int y = facility->getY(); y < facility->getY() + rule->getSizeY(); ++y)
                if (_facilities[right][y] && _facilities[right][y]->isBuiltOrHadPreviousFacility() &&
                    !_facilities[right][y]->getRules()->connectorsDisabled())
                    frame(7, right * GRID_SIZE - GRID_SIZE / 2, y * GRID_SIZE);
        const int bottom = facility->getY() + rule->getSizeY();
        if (bottom < BASE_SIZE)
            for (int x = facility->getX(); x < facility->getX() + rule->getSizeX(); ++x)
                if (_facilities[x][bottom] && _facilities[x][bottom]->isBuiltOrHadPreviousFacility() &&
                    !_facilities[x][bottom]->getRules()->connectorsDisabled())
                    frame(8, x * GRID_SIZE, bottom * GRID_SIZE - GRID_SIZE / 2);
    }
    for (const auto *facility : *_base->getFacilities())
    {
        const auto *rule = facility->getRules();
        if (rule->getSpriteEnabled())
        {
            int index = 0;
            for (int y = facility->getY(); y < facility->getY() + rule->getSizeY(); ++y)
                for (int x = facility->getX(); x < facility->getX() + rule->getSizeX(); ++x)
                    frame(rule->getSpriteFacility() + index++, x * GRID_SIZE, y * GRID_SIZE);
        }
        if (const auto *craft = facility->getCraftForDrawing())
            frame(craft->getSkinSprite() + 33,
                facility->getX() * GRID_SIZE + (rule->getSizeX() - 1) * GRID_SIZE / 2 + 2,
                facility->getY() * GRID_SIZE + (rule->getSizeY() - 1) * GRID_SIZE / 2 - 4);
        if (facility->getBuildTime() > 0 || facility->getDisabled())
        {
            Text label(GRID_SIZE * rule->getSizeX(), 16, facility->getX() * GRID_SIZE,
                facility->getY() * GRID_SIZE + (GRID_SIZE * rule->getSizeY() - 16) / 2);
            label.setPalette(getPalette()); label.initText(_big, _small, _lang);
            label.setBig(); label.setAlign(ALIGN_CENTER); label.setColor(_cellColor);
            std::ostringstream value;
            if (facility->getDisabled()) value << "X";
            else value << facility->getBuildTime();
            if (facility->getIfHadPreviousFacility()) value << "*";
            label.setText(value.str()); label.composeHd(content, images);
        }
        if (facility->getBuildTime() == 0 && rule->getAmmoMax() > 0)
        {
            Text label(GRID_SIZE * rule->getSizeX(), 9, facility->getX() * GRID_SIZE, facility->getY() * GRID_SIZE);
            label.setPalette(getPalette()); label.initText(_big, _small, _lang);
            label.setHighContrast(_highContrast);
            label.setColor(facility->getAmmo() >= rule->getAmmoMax() ? _greenColor :
                facility->getAmmo() <= rule->getAmmoMax() / 2 ? _redColor : _yellowColor);
            label.setText(std::to_string(facility->getAmmo()) + "/" + std::to_string(rule->getAmmoMax()));
            label.composeHd(content, images);
        }
    }
    // Selector geometry comes from input state, never from its native pixels.
    if (_selector && _selSizeX > 0 && _selSizeY > 0 && _selector->isDisplayVisible() && _blink)
    {
        const double x = _selector->getX() - getX(), y = _selector->getY() - getY();
        const double width = _selSizeX * GRID_SIZE, height = _selSizeY * GRID_SIZE;
        const auto color = getHdColor(_selectorColor);
        content.rectangle({x, y, width, 1}, color);
        content.rectangle({x, y + height - 1, width, 1}, color);
        content.rectangle({x, y + 1, 1, height - 2}, color);
        content.rectangle({x + width - 1, y + 1, 1, height - 2}, color);
    }
    composeHdLayer(canvas, content);
}
}
