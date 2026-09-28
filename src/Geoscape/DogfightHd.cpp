#include "DogfightState.h"
#include "../Engine/HdUiPicture.h"
#include "../Engine/InteractiveSurface.h"
#include "../Interface/Text.h"
#include "../Savegame/Craft.h"
#include "../Savegame/CraftWeapon.h"
#include "../Mod/RuleCraftWeapon.h"
#include "../Engine/Game.h"
#include "../Engine/Screen.h"
#include "../Mod/Mod.h"
#include "../Mod/RuleInterface.h"
#include "../Mod/RuleUfo.h"
#include "../Savegame/Ufo.h"

namespace OpenXcom
{
void DogfightState::refreshHdPreview()
{
    const auto *ui = _game->getMod()->getInterface("dogfight");
    const auto *top = ui->getElement("previewTop");
    const auto *bottom = ui->getElement("previewBot");
    const auto *middle = ui->getElement("previewMid");
    const auto panel = hdUiScreenDefinition("INTERWIN.DAT", 160, 600);
    HdUiPicture picture(_preview->getWidth(), _preview->getHeight());
    picture.fill(picture.bounds(), 15);
    picture.image(panel, {0, double(top->y), double(_window->getWidth()), double(top->h)},
        {0, 0, double(_window->getWidth()), double(top->h)});
    picture.image(panel, {0, double(bottom->y), double(_window->getWidth()), double(bottom->h)},
        {0, double(_window->getHeight() - bottom->h), double(_window->getWidth()), double(bottom->h)});
    const auto &custom = _ufo->getRules()->getModSprite();
    if (custom.empty())
        picture.image(panel, {0, double(middle->y + middle->h * _ufo->getRules()->getSprite()),
            double(_window->getWidth()), double(middle->h)},
            {double(top->x), double(top->h), double(_window->getWidth()), double(middle->h)});
    else
    {
        const auto art = hdUiNaturalScreenDefinition(custom, _game->getScreen()->getHdCanvasImages());
        picture.image(art, {0, 0, art.width, art.height}, {double(top->x), double(top->h), art.width, art.height});
    }
    _preview->setHdPicture(picture);
}

void DogfightState::refreshHdWeapon(int slot, bool enabled)
{
    auto *weapon = _weapon[slot];
    auto *range = _range[slot];
    HdUiPicture icon(weapon->getWidth(), weapon->getHeight());
    HdUiPicture meter(range->getWidth(), range->getHeight());
    const auto *item = _craft->getWeapons()->at(slot);
    _txtAmmo[slot]->setColor(static_cast<Uint8>(_hdAmmoBaseColors[slot] + (enabled ? 0 : _colors[DISABLED_AMMO])));
    if (item && item->getRules()->getSprite() >= 0)
    {
        HdImageStyle style;
        if (!enabled) style.light = 0.45;
        const auto mapping = std::make_shared<const HdUiIndexMap>(hdUiOffsetPalette(enabled ? 0 : _colors[DISABLED_WEAPON]));
        icon.image(hdUiFrameDefinition("INTICON.PCK", item->getRules()->getSprite() + 5, 32, 40),
            {0, 0, 32, 40}, {0, 0, 32, 40}, style, mapping);
        if (item->getRules()->getAmmoMax() > 0 || item->getRules()->getTractorBeamPower() > 0)
        {
            const int offset = 2 * (slot / 2 + 1);
            const int x1 = slot % 2 == 0 ? offset : 0;
            const int x2 = slot % 2 == 0 ? 0 : 20 - offset;
            const int rangeY = range->getHeight() - item->getRules()->getRange();
            const int connectY = weapon->getHeight() / 2 + weapon->getY() - range->getY();
            const auto color = static_cast<Uint8>(_colors[RANGE_METER] + (enabled ? 0 : _colors[DISABLED_RANGE]));
            for (int x = x1; x <= x1 + 20 - offset; x += 2)
                meter.fill({double(x), double(rangeY), 1, 1}, color);
            // Equal endpoints retain the original connector at y=0.
            const int minY = rangeY == connectY ? 0 : std::min(rangeY, connectY);
            const int maxY = rangeY == connectY ? 0 : std::max(rangeY, connectY);
            meter.fill({double(x1 + x2), double(minY), 1, double(maxY - minY + 1)}, color);
            meter.fill({double(x2), double(connectY), double(offset + 1), 1}, color);
        }
    }
    weapon->setHdPicture(icon);
    range->setHdPicture(meter);
}
}
