// Explicit inventory composition for the HD renderer. No native image buffers
// or legacy blit/script workers participate in this producer.
#include "Inventory.h"
#include "WarningMessage.h"
#include "../Engine/HdUiImage.h"
#include "../Engine/Exception.h"
#include "../Engine/Game.h"
#include "../Engine/Font.h"
#include "../Engine/Language.h"
#include "../Interface/Text.h"
#include "../Interface/NumberText.h"
#include "../Mod/Mod.h"
#include "../Mod/RuleInterface.h"
#include "../Mod/RuleInventory.h"
#include "../Mod/RuleItem.h"
#include "../Savegame/SavedGame.h"
#include "../Savegame/BattleItem.h"
#include "../Savegame/BattleUnit.h"
#include "../Savegame/Tile.h"
#include <set>
#include <sstream>

namespace OpenXcom
{
void Inventory::composeHd(HdCanvas &canvas, HdImageCache &images)
{
    if (!isDisplayVisible()) return;
    auto *mod = _game->getMod();
    const auto *style = mod->getInterface("inventory");
    const auto *save = _game->getSavedGame()->getSavedBattle();
    HdCanvas content(getWidth(), getHeight()), grid(getWidth(), getHeight()), counts(getWidth(), getHeight());
    const auto border = [&](int x, int y, int width, int height)
    {
        grid.rectangle({double(x), double(y), double(width), double(height)}, getHdColor(style->getElement("grid")->color));
        grid.sourceRectangle({double(x+1), double(y+1), double(width-2), double(height-2)}, {0,0,0,0});
    };
    for (const auto &entry : *mod->getInventories())
    {
        const auto *slot = entry.second;
        if (slot->getType() == INV_SLOT)
            for (const auto &cell : *slot->getSlots())
                border(slot->getX()+RuleInventory::SLOT_W*cell.x, slot->getY()+RuleInventory::SLOT_H*cell.y,
                    RuleInventory::SLOT_W+1, RuleInventory::SLOT_H+1);
        else if (slot->getType() == INV_HAND)
            border(slot->getX(), slot->getY(), RuleInventory::HAND_W*RuleInventory::SLOT_W, RuleInventory::HAND_H*RuleInventory::SLOT_H);
        else if (slot->getType() == INV_GROUND)
            for (int x = slot->getX(); x <= 320; x += RuleInventory::SLOT_W)
                for (int y = slot->getY(); y <= 200; y += RuleInventory::SLOT_H)
                    border(x, y, RuleInventory::SLOT_W+1, RuleInventory::SLOT_H+1);
    }
    content.composite(grid);
    const auto palette = [&](int shade = 0, int base = 0)
    {
        auto colors = std::make_shared<std::array<HdRgba,256>>();
        (*colors)[0] = {0,0,0,0};
        for (int i = 1; i < 256; ++i)
        {
            const auto shifted = static_cast<Uint8>(base ? (i & 15) + shade : i + shade);
            const auto mapped = static_cast<Uint8>(base ?
                ((shifted & 240) ? 15 : ((base-1)*16) | shifted) :
                (((shifted ^ i) & 240) ? 15 : shifted));
            (*colors)[i] = getHdColor(mapped);
        }
        return colors;
    };
    const auto normal = palette();
    const auto itemImage = [&](HdCanvas &target, const BattleItem *item, int x, int y, int selectedFrame = -2)
    {
        const int frame = selectedFrame == -2 ? item->getInventorySpriteFrame(save, _animFrame) : selectedFrame;
        if (frame == -1) return false;
        if (item->hasInventoryPixelProgram())
            throw Exception("[HD INVENTORY ERROR] Pixel program requires an HD material adapter: " + item->getRules()->getType());
        hdAppendUiImage(target, images, hdUiFrameDefinition("BIGOBS.PCK", frame, 32, 48),
            {0,0,32,48}, {double(x),double(y),32,48}, normal);
        return true;
    };
    constexpr int pulse[] = {0,1,2,3,4,3,2,1};
    const int shade = pulse[_animFrame % 8];
    const auto primer = [&](const BattleItem *item, int x, int y)
    {
        if (item->getFuseTimer() >= 0 && item->getRules()->getInventoryWidth() > 0)
            hdAppendUiImage(content, images, hdUiFrameDefinition("SCANG.DAT", 6, 4, 4),
                {0,0,4,4}, {double(x),double(y),4,4}, palette(shade, item->isFuseEnabled() ? 0 : 32));
    };
    const auto number = [&](HdCanvas &target, int value, int x, int y, Uint8 color, bool bordered)
    {
        NumberText label(15,15,x,y);
        label.setPalette(getPalette()); label.setBordered(bordered);
        label.setColor(color); label.setValue(value);
        label.composeHd(target, images);
    };
    if (_selUnit)
    {
        for (const auto *item : *_selUnit->getInventory())
        {
            if (item == _selItem || !item->getSlot()) continue;
            const auto *slot = item->getSlot();
            int x = slot->getX(), y = slot->getY();
            if (slot->getType() == INV_SLOT) { x += item->getSlotX()*RuleInventory::SLOT_W; y += item->getSlotY()*RuleInventory::SLOT_H; }
            else if (slot->getType() == INV_HAND) { x += item->getRules()->getHandSpriteOffX(); y += item->getRules()->getHandSpriteOffY(); }
            else continue;
            if (!itemImage(content, item, x, y)) continue;
            if (slot->getType() == INV_HAND && (item->getRules()->isTwoHanded() || item->getRules()->isBlockingBothHands()))
                number(content, 2, slot->getX()+RuleInventory::HAND_W*RuleInventory::SLOT_W-5,
                    slot->getY()+RuleInventory::HAND_H*RuleInventory::SLOT_H-7,
                    item->getRules()->isBlockingBothHands() ? _twoHandedRed : _twoHandedGreen, false);
            primer(item, x, y);
        }
        std::set<std::pair<int,int>> occupied;
        if (_selUnit->getTile()) for (const auto *item : *_selUnit->getTile()->getInventory())
        {
            const auto *rule = item->getRules();
            const auto *slot = item->getSlot();
            if (item == _selItem || !slot || !rule->getInventoryWidth() || !rule->getInventoryHeight()) continue;
            if (item->getSlotX() < _groundOffset || item->getSlotX() >= _groundOffset+_groundSlotsX) continue;
            const int frame = item->getInventorySpriteFrame(save, _animFrame);
            if (frame == -1) continue;
            if (!occupied.emplace(item->getSlotX(), item->getSlotY()).second) continue;
            const int x = slot->getX()+(item->getSlotX()-_groundOffset)*RuleInventory::SLOT_W;
            const int y = slot->getY()+item->getSlotY()*RuleInventory::SLOT_H;
            itemImage(content, item, x, y, frame);
            primer(item, x, y);
            int wounds = 0;
            if (const auto *unit = item->getUnit())
                if (unit->getStatus() == STATUS_UNCONSCIOUS && unit->indicatorsAreEnabled())
                {
                    wounds = unit->getFatalWounds();
                    const Surface *indicator = _burnIndicator && unit->getFire() > 0 ? _burnIndicator :
                        _woundIndicator && wounds > 0 ? _woundIndicator :
                        _shockIndicator && unit->hasNegativeHealthRegen() ? _shockIndicator : _stunIndicator;
                    if (indicator)
                        hdAppendUiImage(content, images, hdUiScreenDefinition(indicator->getHdResourceId(), indicator->getWidth(), indicator->getHeight()),
                            {0,0,double(indicator->getWidth()),double(indicator->getHeight())},
                            {double(x),double(y),double(indicator->getWidth()),double(indicator->getHeight())}, palette(shade));
                }
            const auto stackNumber = [&](int value, Uint8 color)
            {
                const int nx = slot->getX()+(item->getSlotX()+rule->getInventoryWidth()-_groundOffset)*RuleInventory::SLOT_W-4-(value>9?4:0);
                const int ny = slot->getY()+(item->getSlotY()+rule->getInventoryHeight())*RuleInventory::SLOT_H-6;
                number(counts, value, nx, ny, color, true);
            };
            if (wounds > 0) stackNumber(wounds, style->getElement("numStack")->color2);
            const auto column = _stackLevel.find(item->getSlotX());
            if (column != _stackLevel.end())
            {
                const auto row = column->second.find(item->getSlotY());
                if (row != column->second.end() && row->second > 1) stackNumber(row->second, style->getElement("numStack")->color);
            }
        }
    }
    content.composite(counts);
    Text label(90,9,0,0);
    label.setPalette(getPalette()); label.initText(mod->getFont("FONT_BIG"), mod->getFont("FONT_SMALL"), _game->getLanguage());
    label.setColor(style->getElement("textSlots")->color); label.setHighContrast(true);
    for (const auto &name : mod->getInvsList())
    {
        const auto *slot = mod->getInventory(name, true);
        label.setX(slot->getX()); label.setY(slot->getY()-label.getFont()->getHeight()-label.getFont()->getSpacing());
        std::ostringstream text;
        text << _game->getLanguage()->getString(slot->getId()).arg(1+_groundOffset/_groundSlotsX).arg(1+_xMax/_groundSlotsX);
        if (_hdShowTuCost && _selItem && _selItem->getSlot() != slot) text << ":" << _selItem->getMoveToCost(slot);
        label.setText(text.str()); label.composeHd(content, images);
    }
    if (_selItem)
    {
        HdCanvas held(_selection->getWidth(), _selection->getHeight());
        itemImage(held, _selItem, _selItem->getRules()->getHandSpriteOffX(), _selItem->getRules()->getHandSpriteOffY());
        content.composite(held, {double(_selection->getX()),double(_selection->getY()),1,1});
    }
    _warning->composeHd(content, images);
    composeHdLayer(canvas, content);
}
}
