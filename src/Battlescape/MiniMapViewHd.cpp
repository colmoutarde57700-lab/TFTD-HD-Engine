#include "MiniMapView.h"
#include "Camera.h"
#include "Pathfinding.h"
#include "../Engine/Game.h"
#include "../Engine/Options.h"
#include "../Engine/HdUiImage.h"
#include "../Mod/Armor.h"
#include "../Mod/MapData.h"
#include "../Mod/RuleItem.h"
#include "../Savegame/SavedBattleGame.h"
#include "../Savegame/Tile.h"
#include "../Savegame/BattleUnit.h"
#include "../Savegame/BattleItem.h"

namespace OpenXcom
{
void MiniMapView::composeHd(HdCanvas &canvas, HdImageCache &images)
{
    if (!isDisplayVisible()) return;
    // Capture presentation facts before resource resolution. No native image,
    // frame buffer, or pixel-derived visibility enters this list.
    struct Marker { int frame, x, y, shade; bool red; };
    std::vector<Marker> markers;
    const auto center = _camera->getCenterPosition();
    const int startX = center.x - (getWidth() / 4) / 2;
    const int startY = center.y - (getHeight() / 4) / 2;
    bool outside = _game->isAltPressed(true);
    if (Options::isPasswordCorrect()) outside = !outside;
    for (int level = 0; level <= center.z; ++level)
        for (int y = 0; y < getHeight(); y += 4)
            for (int x = 0; x < getWidth(); x += 4)
            {
                auto *tile = _battleGame->getTile(Position(startX + x / 4, startY + y / 4, level));
                if (!tile)
                {
                    if (outside) markers.push_back({_emptySpaceIndex, x, y, 0, false});
                    continue;
                }
                for (int part = O_FLOOR; part < O_MAX; ++part)
                    if (const auto *data = tile->getMapData(static_cast<TilePart>(part)))
                        if (data->getMiniMapIndex())
                            markers.push_back({data->getMiniMapIndex() + 35, x, y,
                                tile->isDiscovered(O_FLOOR) ? std::min(7, tile->getShade()) : 16, false});
                if (const auto *unit = tile->getUnit())
                    if (unit->getVisible() || _battleGame->getBughuntMode() || _battleGame->getDebugMode())
                    {
                        const int size = unit->getArmor()->getSize();
                        const int part = (tile->getPosition().y - unit->getPosition().y) * size +
                            tile->getPosition().x - unit->getPosition().x;
                        markers.push_back({unit->getMiniMapSpriteIndex() + part + _frame * size * size,
                            x, y, 0, size > 1 && unit->getFaction() == FACTION_NEUTRAL});
                    }
                if (tile->isDiscovered(O_FLOOR))
                {
                    bool any = false, primed = false;
                    for (const auto *item : *tile->getInventory())
                        if (!item->getRules()->isHiddenOnMinimap())
                        {
                            any = true;
                            if (item->getFuseTimer() >= 0) { primed = true; break; }
                        }
                    if (any) markers.push_back({9 + _frame, x, y, 0, primed});
                }
            }
    HdCanvas content(getWidth(), getHeight());
    content.rectangle(content.bounds(), getHdColor(15));
    std::array<std::shared_ptr<const std::array<HdRgba, 256>>, 18> palettes{};
    for (const auto &marker : markers)
    {
        const int shade = std::max(0, std::min(16, marker.shade));
        const int key = marker.red ? 17 : shade;
        if (!palettes[key])
        {
            auto colors = std::make_shared<std::array<HdRgba, 256>>();
            (*colors)[0] = {0, 0, 0, 0};
            for (int i = 1; i < 256; ++i)
            {
                const auto shifted = static_cast<Uint8>(i + shade);
                const auto mapped = marker.red ? static_cast<Uint8>((Pathfinding::red - 1) * 16 + (i & 15)) :
                    static_cast<Uint8>(((shifted ^ i) & 240) ? 15 : shifted);
                (*colors)[i] = getHdColor(mapped);
            }
            palettes[key] = colors;
        }
        HdImageStyle style;
        style.light = (16 - shade) / 16.0;
        style.tint = marker.red;
        style.tintColor = getHdColor(static_cast<Uint8>((Pathfinding::red - 1) * 16));
        hdAppendUiImage(content, images, hdUiFrameDefinition("SCANG.DAT", marker.frame, 4, 4),
            {0, 0, 4, 4}, {double(marker.x), double(marker.y), 4, 4}, palettes[key], false, false, style);
    }
    const double cx = getWidth() / 2 - 1, cy = getHeight() / 2 - 1;
    const auto color = getHdColor(1 + _frame * 3);
    // Three logical pixels per diagonal retain the original center marker.
    for (int i = 2; i <= 4; ++i)
        for (int sx : {-1, 1}) for (int sy : {-1, 1})
            content.rectangle({cx + sx * i, cy + sy * i, 1, 1}, color);
    composeHdLayer(canvas, content);
}
}
