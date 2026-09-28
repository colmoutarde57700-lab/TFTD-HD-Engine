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
#include "MedikitView.h"
#include "../Engine/Game.h"
#include "../Mod/Mod.h"
#include "../Mod/RuleInterface.h"
#include "../Engine/SurfaceSet.h"
#include "../Engine/Action.h"
#include "../Engine/Language.h"
#include "../Savegame/BattleUnit.h"
#include "../Interface/Text.h"
#include "../Engine/HdUiImage.h"
#include "../Engine/Options.h"
#include "../Engine/Screen.h"

namespace OpenXcom
{
/**
 * Initializes the Medikit view.
 * @param w The MinikitView width.
 * @param h The MinikitView height.
 * @param x The MinikitView x origin.
 * @param y The MinikitView y origin.
 * @param game Pointer to the core game.
 * @param unit The wounded unit.
 * @param partTxt A pointer to a Text. Will be updated with the selected body part.
 * @param woundTxt A pointer to a Text. Will be updated with the amount of fatal wound.
 */
MedikitView::MedikitView (int w, int h, int x, int y, Game * game, BattleUnit *unit, Text *partTxt, Text *woundTxt) : InteractiveSurface(w, h, x, y), _game(game), _selectedPart(0), _unit(unit), _partTxt(partTxt), _woundTxt(woundTxt)
{
	updateSelectedPart();
	_redraw = true;
}

/**
 * Draws the medikit view.
 */
void MedikitView::draw()
{
	SurfaceSet *set = _game->getMod()->getSurfaceSet("MEDIBITS.DAT");
	int fatal_wound = _unit->getFatalWound((UnitBodyPart)_selectedPart);
	std::ostringstream ss, ss1;
	int green = 0;
	int red = 3;
	if (_game->getMod()->getInterface("medikit", false) && _game->getMod()->getInterface("medikit")->getElementOptional("body"))
	{
		green = _game->getMod()->getInterface("medikit")->getElement("body")->color;
		red = _game->getMod()->getInterface("medikit")->getElement("body")->color2;
	}
	this->lock();
	for (unsigned int i = 0; i < set->getTotalFrames(); i++)
	{
		int wound = _unit->getFatalWound((UnitBodyPart)i);
		Surface * surface = set->getFrame (i);
		int baseColor = wound ? red : green;
		surface->blitNShade(this, 0, 0, 0, false, baseColor);
	}
	this->unlock();

	_redraw = false;
	if (_selectedPart == -1)
	{
		return;
	}
	ss << _game->getLanguage()->getString(PARTS_STRING[_selectedPart]);
	ss1 << fatal_wound;
	_partTxt->setText(ss.str());
	_woundTxt->setText(ss1.str());
}

/**
 * Handles clicks on the medikit view.
 * @param action Pointer to an action.
 * @param state State that the action handlers belong to.
 */
void MedikitView::composeHd(HdCanvas &canvas, HdImageCache &images)
{
	if (!isDisplayVisible()) return;
	int green = 0, red = 3;
	if (_game->getMod()->getInterface("medikit", false))
		if (const auto *body = _game->getMod()->getInterface("medikit")->getElementOptional("body"))
		{
			green = body->color;
			red = body->color2;
		}
	HdCanvas content(getWidth(), getHeight());
	for (int part = 0; part < BODYPART_MAX; ++part)
	{
		const int base = _unit->getFatalWound(static_cast<UnitBodyPart>(part)) ? red : green;
		auto palette = std::make_shared<std::array<HdRgba, 256>>();
		for (size_t i = 0; i < palette->size(); ++i)
			(*palette)[i] = getHdColor(static_cast<Uint8>(i == 0 || base == 0 ? i : ((base - 1) * 16) | (i & 15)));
		hdAppendUiImage(content, images, hdUiFrameDefinition("MEDIBITS.DAT", part, 52, 58),
			{0, 0, 52, 58}, {0, 0, 52, 58}, palette);
	}
	if (_selectedPart >= 0 && _selectedPart < BODYPART_MAX)
	{
		_partTxt->setText(_game->getLanguage()->getString(PARTS_STRING[_selectedPart]));
		_woundTxt->setText(std::to_string(_unit->getFatalWound(static_cast<UnitBodyPart>(_selectedPart))));
	}
	composeHdLayer(canvas, content);
}

void MedikitView::mouseClick (Action *action, State *)
{
	int x = action->getRelativeXMouse() / action->getXScale();
	int y = action->getRelativeYMouse() / action->getYScale();
	if (Options::hdGraphics)
	{
		for (int part = 0; part < BODYPART_MAX; ++part)
			if (hdUiImageHitTest(_game->getScreen()->getHdCanvasImages(),
				hdUiFrameDefinition("MEDIBITS.DAT", part, 52, 58), {double(x), double(y)}))
			{
				_selectedPart = part;
				_redraw = true;
				break;
			}
		return;
	}
	SurfaceSet *set = _game->getMod()->getSurfaceSet("MEDIBITS.DAT");
	for (unsigned int i = 0; i < set->getTotalFrames(); i++)
	{
		Surface * surface = set->getFrame (i);
		if (surface->getPixel(x, y))
		{
			_selectedPart = i;
			_redraw = true;
			break;
		}
	}
}

/**
 * Gets the selected body part.
 * @return The selected body part.
 */
int MedikitView::getSelectedPart() const
{
	return _selectedPart;
}

/**
 * Updates the selected body part.
 * If there is a wounded body part, selects that.
 * Otherwise does not change the selected part.
 */
void MedikitView::updateSelectedPart()
{
	for (int i = 0; i < BODYPART_MAX; ++i)
	{
		if (_unit->getFatalWound((UnitBodyPart)i))
		{
			_selectedPart = i;
			break;
		}
	}
}

}
