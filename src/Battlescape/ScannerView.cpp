/*
 * Copyright 2010-2016 OpenXcom Developers.
 *
 * This file is part of OpenXcom.
 *
 * OpenXcom is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * OpenXcom is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with OpenXcom.  If not, see <http://www.gnu.org/licenses/>.
 */
#include "ScannerView.h"
#include "../Engine/Game.h"
#include "../Engine/SurfaceSet.h"
#include "../Mod/Mod.h"
#include "../Engine/Action.h"
#include "../Savegame/BattleUnit.h"
#include "../Savegame/Tile.h"
#include "../Savegame/SavedGame.h"
#include "../Savegame/SavedBattleGame.h"
#include "../Engine/HdUiImage.h"

namespace OpenXcom
{

/**
 * Initializes the Scanner view.
 * @param w The ScannerView width.
 * @param h The ScannerView height.
 * @param x The ScannerView x origin.
 * @param y The ScannerView y origin.
 * @param game Pointer to the core game.
 * @param unit The current unit.
 */
ScannerView::ScannerView (int w, int h, int x, int y, Game * game, BattleUnit *unit) : InteractiveSurface(w, h, x, y), _game(game), _unit(unit), _frame(0)
{
	refreshContacts();
	_redraw = true;
}

/**
 * Draws the ScannerView view.
 */
void ScannerView::refreshContacts()
{
	// Scanning is a game operation, not a side effect of either renderer.
	// Both presentations consume the same contact snapshot afterwards.
	_contacts.clear();
	_direction = _unit->getDirection();
	for (int x = -9; x < 10; x++)
	{
		for (int y = -9; y < 10; y++)
		{
			for (int z = 0; z < _game->getSavedGame()->getSavedBattle()->getMapSizeZ(); z++)
			{
				Tile *t = _game->getSavedGame()->getSavedBattle()->getTile(Position(x,y,z) + Position(_unit->getPosition().x, _unit->getPosition().y, 0));
				if (t && t->getUnit() && t->getUnit()->getMotionPoints())
				{
					int frame = (t->getUnit()->getMotionPoints() / 5);
					if (frame >= 0)
					{
						t->getUnit()->setScannedTurn(_game->getSavedGame()->getSavedBattle()->getTurn());
						if (frame > 5) frame = 5;
						_contacts.push_back({((9+x)*8)-4, ((9+y)*8)-4, frame});
					}
				}
			}
		}
	}

}

void ScannerView::draw()
{
	SurfaceSet *set = _game->getMod()->getSurfaceSet("DETBLOB.DAT");
	clear();
	lock();
	for (const auto &contact : _contacts)
		set->getFrame(contact.strength + _frame)->blitNShade(this, contact.x, contact.y, 0);
	set->getFrame(7 + _direction)->blitNShade(this, 68, 68, 0);
	unlock();
}

void ScannerView::composeHd(HdCanvas &canvas, HdImageCache &images)
{
	if (!isDisplayVisible()) return;
	HdCanvas content(getWidth(), getHeight());
	auto palette = std::make_shared<std::array<HdRgba, 256>>();
	for (size_t i = 0; i < palette->size(); ++i) (*palette)[i] = getHdColor(static_cast<Uint8>(i));
	const auto append = [&](int frame, int x, int y)
	{
		hdAppendUiImage(content, images, hdUiFrameDefinition("DETBLOB.DAT", frame, 16, 16),
			{0, 0, 16, 16}, {double(x), double(y), 16, 16}, palette);
	};
	for (const auto &contact : _contacts) append(contact.strength + _frame, contact.x, contact.y);
	append(7 + _direction, 68, 68);
	composeHdLayer(canvas, content);
}

/**
 * Handles clicks on the scanner view.
 * @param action Pointer to an action.
 * @param state State that the action handlers belong to.
 */
void ScannerView::mouseClick (Action *, State *)
{
}

/**
 * Updates the scanner animation.
 */
void ScannerView::animate()
{
	refreshContacts();
	_frame++;
	if (_frame > 1)
	{
		_frame = 0;
	}
	_redraw = true;
}

}
