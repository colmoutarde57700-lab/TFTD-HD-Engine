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
#include "State.h"
#include "HdGpuBackend.h"
#include <algorithm>
#include <climits>
#include <cstring>
#include "InteractiveSurface.h"
#include "Game.h"
#include "Screen.h"
#include "Surface.h"
#include "Language.h"
#include "LocalizedText.h"
#include "Palette.h"
#include "Options.h"
#include "../Engine/Sound.h"
#include "../Engine/Collections.h"
#include "../Mod/Mod.h"
#include "../Interface/Window.h"
#include "../Interface/TextButton.h"
#include "../Interface/TextEdit.h"
#include "../Interface/TextList.h"
#include "../Interface/BattlescapeButton.h"
#include "../Interface/ComboBox.h"
#include "../Interface/Cursor.h"
#include "../Interface/FpsCounter.h"
#include "../Savegame/SavedBattleGame.h"
#include "../Mod/RuleInterface.h"

namespace OpenXcom
{

/// Initializes static member
Game* State::_game = 0;

/**
 * Initializes a brand new state with no child elements.
 * By default states are full-screen.
 * @param game Pointer to the core game.
 */
State::State() : _screen(true), _soundPlayed(false), _modal(0), _ruleInterface(0), _ruleInterfaceParent(0), _customSound(nullptr), _presentationScale(1)
{
	// initialize palette to all black
	memset(_palette, 0, sizeof(_palette));
	_cursorColor = _game->getCursor()->getColor();
}

/**
 * Deletes all the child elements contained in the state.
 */
State::~State()
{
	for (auto &pair : _hdPhysicalUiCache)
	{
		if (pair.second.surface) SDL_FreeSurface(pair.second.surface);
	}
	_hdPhysicalUiCache.clear();

	// Surfaces are deleted in reverse order of adding, same like local variables
	for (auto* surface : Collections::reverse(Collections::range(_surfacesOwned)))
	{
		delete surface;
	}
}

void State::applyPresentationScaleToSurface(Surface *surface)
{
	if (!surface) return;
	const int scale = std::max(1, _presentationScale);

	// Scale ordinary menu/state surfaces as ONE coherent composition around
	// the logical screen centre.  The previous per-surface top/centre/bottom
	// anchoring distorted complex 320x200 layouts: e.g. bottom buttons in the
	// Mods/New Battle screens could move upward into their lists at x2.
	// BattlescapeState applies its own edge-aware HUD anchors afterwards, so
	// keeping the generic State transform centred does not affect the tactical
	// HUD behaviour.
	const int anchorX = Options::baseXResolution / 2;
	const int anchorY = Options::baseYResolution / 2;
	surface->setDisplayScale(scale, anchorX, anchorY);
}

void State::refreshPresentationScaleAnchors()
{
	for (auto *surface : _surfaces) applyPresentationScaleToSurface(surface);
}

void State::setPresentationScale(int scale)
{
	_presentationScale = std::max(1, scale);
	refreshPresentationScaleAnchors();
}

/**
 * Set interface data from the ruleset, also sets the palette for the state.
 * @param category Name of the interface set.
 * @param alterPal Should we swap out the backpal colors?
 * @param battleGame Should we use battlescape palette? (this only applies to options screens)
 */
void State::setInterface(const std::string& category, bool alterPal, SavedBattleGame *battleGame)
{
	// HD/UI architecture: logical coordinates remain 320x200-era values while
	// presentation scale is selected independently by screen family.
	// A popup opened FROM Battlescape belongs to the Battlescape UI family,
	// including the in-battle Options screens. This must take precedence over
	// the generic "options" category or those popups remain microscopic while
	// the tactical HUD is scaled.
	if (battleGame)
	{
		_presentationScale = Options::getBattleUiScale();
	}
	else if (category.find("options") != std::string::npos || category.find("controls") != std::string::npos || category.find("advanced") != std::string::npos)
	{
		_presentationScale = 1;
	}
	else if (category.find("soldier") != std::string::npos || category.find("Armor") != std::string::npos || category.find("armor") != std::string::npos || category.find("craftEquipment") != std::string::npos || category == "inventory")
	{
		_presentationScale = Options::getAquanautUiScale();
	}
	else
	{
		// Strategic windows (Basescape, Ufopaedia, Research, Manufacture,
		// Sell/Sack, facility placement, etc.) already live on the Geoscape
		// logical canvas selected by geoscapeScale. Applying geoUiScale here
		// scales them a second time and breaks both layout and input geometry.
		// geoUiScale is therefore reserved for GeoscapeState's own sidebar,
		// which applies it explicitly in applyHdUiPresentationScale().
		_presentationScale = 1;
	}

	// Presentation scaling must not enlarge a classic 320x200 interface beyond
	// the current logical canvas. On a 320x200 canvas a requested x2 menu would
	// otherwise be pushed mostly off-screen. Larger logical resolutions still
	// allow x2/x3/x4 independently as intended.
	const int fitX = std::max(1, Options::baseXResolution / Screen::ORIGINAL_WIDTH);
	const int fitY = std::max(1, Options::baseYResolution / Screen::ORIGINAL_HEIGHT);
	_presentationScale = std::min(_presentationScale, std::max(1, std::min(fitX, fitY)));

	int backPal = -1;
	std::string pal = "PAL_GEOSCAPE";

	_ruleInterface = _game->getMod()->getInterface(category);
	if (_ruleInterface)
	{
		_ruleInterfaceParent = _game->getMod()->getInterface(_ruleInterface->getParent());
		pal = _ruleInterface->getPalette();
		const Element *element = _ruleInterface->getElementOptional("palette");
		if (_ruleInterfaceParent)
		{
			if (!element)
			{
				element = _ruleInterfaceParent->getElementOptional("palette");
			}
			if (pal.empty())
			{
				pal = _ruleInterfaceParent->getPalette();
			}
		}
		if (element)
		{
			int color = alterPal ? element->color2 : element->color;
			if (color != INT_MAX)
			{
				backPal = color;
			}
		}
	}
	if (battleGame)
	{
		battleGame->setPaletteByDepth(this);
	}
	else if (pal.empty())
	{
		pal = "PAL_GEOSCAPE";
		setStandardPalette(pal, backPal);
	}
	else
	{
		setStandardPalette(pal, backPal);
	}
}

/**
 * Set window background from the ruleset.
 * @param window Window handle.
 * @param s ID of the interface ruleset entry.
 */
void State::setWindowBackground(Window *window, const std::string &s)
{
	auto& bgImageName = _game->getMod()->getInterface(s)->getBackgroundImage(_game->getMod(), _game->getSavedGame());
	setWindowBackgroundImage(window, bgImageName);
}

/**
 * Set window background by image name (instead of by interface name).
 * @param window Window handle.
 * @param s ID of the image.
 */
void State::setWindowBackgroundImage(Window* window, const std::string& bgImageName)
{
	const auto* bgImage = _game->getMod()->getSurface(bgImageName);
	window->setBackground(bgImage);
	if (window && !bgImageName.empty())
	{
		_hdExplicitSurfacePath[window] = "Resources/TFTD_HD/UI/Backgrounds/" + bgImageName + ".png";
	}
}

/**
 *  Add a optional child element but it will not be displayed.
 */
void State::preAdd(Surface *surface)
{
	//TODO: O(n^2) but number of surfaces is less than 100 (it become lag araund 100k surfaces) and sort of hash will make deleting order nondetermistic
	if (std::find(_surfacesOwned.begin(), _surfacesOwned.end(), surface) == _surfacesOwned.end())
	{
		_surfacesOwned.push_back(surface);
	}
}


/**
 * Adds a new child surface for the state to take care of,
 * giving it the game's display palette. Once associated,
 * the state handles all of the surface's behaviour
 * and management automatically.
 * @param surface Child surface.
 * @note Since visible elements can overlap one another,
 * they have to be added in ascending Z-Order to be blitted
 * correctly onto the screen.
 */
void State::add(Surface *surface)
{
	// Set palette
	surface->setPalette(_palette);

	// Set default text resources
	if (_game->getLanguage() && _game->getMod())
		surface->initText(_game->getMod()->getFont("FONT_BIG"), _game->getMod()->getFont("FONT_SMALL"), _game->getLanguage());

	applyPresentationScaleToSurface(surface);
	_surfaces.push_back(surface);
	preAdd(surface);
}

/**
 * As above, except this adds a surface based on an
 * interface element defined in the ruleset.
 * @note that this function REQUIRES the ruleset to have been loaded prior to use.
 * @param surface Child surface.
 * @param id the ID of the element defined in the ruleset, if any.
 * @param category the category of elements this interface is associated with.
 * @param parent the surface to base the coordinates of this element off.
 * @note if no parent is defined the element will not be moved.
 */
void State::add(Surface *surface, const std::string &id, const std::string &category, Surface *parent)
{
	// Set palette
	surface->setPalette(_palette);

	// this only works if we're dealing with a battlescape button
	BattlescapeButton *bsbtn = dynamic_cast<BattlescapeButton*>(surface);

	if (_game->getMod()->getInterface(category, false))
	{
		const Element *element = _game->getMod()->getInterface(category)->getElementOptional(id);
		if (element)
		{
			if (parent && element->w != INT_MAX && element->h != INT_MAX)
			{
				surface->setWidth(element->w);
				surface->setHeight(element->h);
			}

			if (parent && element->x != INT_MAX && element->y != INT_MAX)
			{
				surface->setX(parent->getX() + element->x);
				surface->setY(parent->getY() + element->y);
			}

			auto inter = dynamic_cast<InteractiveSurface*>(surface);
			if (inter)
			{
				inter->setTFTDMode(element->TFTDMode);
			}

			if (element->color != INT_MAX)
			{
				surface->setColor(element->color);
			}
			if (element->color2 != INT_MAX)
			{
				surface->setSecondaryColor(element->color2);
			}
			if (element->border != INT_MAX)
			{
				surface->setBorderColor(element->border);
			}
			if (!element->hdAsset.empty())
			{
				std::string hdPath = element->hdAsset;
				if (hdPath.rfind("Resources/", 0) != 0)
				{
					hdPath = "Resources/TFTD_HD/UI/" + hdPath;
				}
				_hdExplicitSurfacePath[surface] = hdPath;
			}
		}
	}

	if (bsbtn)
	{
		// this will initialize the graphics and settings of the battlescape button.
		bsbtn->copy(parent);
		bsbtn->initSurfaces();
	}

	// Set default text resources
	if (_game->getLanguage() && _game->getMod())
		surface->initText(_game->getMod()->getFont("FONT_BIG"), _game->getMod()->getFont("FONT_SMALL"), _game->getLanguage());

	registerHdSurface(surface, id, category);
	applyPresentationScaleToSurface(surface);
	_surfaces.push_back(surface);
	preAdd(surface);
}

/**
 * Returns whether this is a full-screen state.
 * This is used to optimize the state machine since full-screen
 * states automatically cover the whole screen, (whether they
 * actually use it all or not) so states behind them can be
 * safely ignored since they'd be covered up.
 * @return True if it's a screen, False otherwise.
 */
bool State::isScreen() const
{
	return _screen;
}

/**
 * Toggles the full-screen flag. Used by windows to
 * keep the previous screen in display while the window
 * is still "popping up".
 */
void State::toggleScreen()
{
	_screen = !_screen;
}

/**
 * Initializes the state and its child elements. This is
 * used for settings that have to be reset every time the
 * state is returned to focus (eg. palettes), so can't
 * just be put in the constructor (remember there's a stack
 * of states, so they can be created once while being
 * repeatedly switched back into focus).
 */
void State::init()
{
	_game->getScreen()->setPalette(_palette);
	_game->getCursor()->setPalette(_palette);
	_game->getCursor()->setColor(_cursorColor);
	_game->getCursor()->draw();
	_game->getFpsCounter()->setPalette(_palette);
	_game->getFpsCounter()->setColor(_cursorColor);
	_game->getFpsCounter()->draw();

	// Highest priority: custom sound set explicitly in the code
	// Medium priority: sound defined by the interface ruleset
	// Lowest priority: default window popup sound
	bool muteWindowPopupSound = false;
	if (!_soundPlayed)
	{
		_soundPlayed = true;
		if (!_customSound && _ruleInterface && _ruleInterface->getSound() != Mod::NO_SOUND)
		{
			_customSound = _game->getMod()->getSound("GEO.CAT", _ruleInterface->getSound());
		}
		if (_customSound)
		{
			muteWindowPopupSound = true;
			_customSound->play();
		}
	}

	for (auto* surface : _surfaces)
	{
		Window* window = dynamic_cast<Window*>(surface);
		if (window)
		{
			if (muteWindowPopupSound)
			{
				window->mute();
			}
			window->invalidate();
		}
	}
	if (_ruleInterface != 0 && !_ruleInterface->getMusic().empty())
	{
		_game->getMod()->playMusic(_ruleInterface->getMusic());
	}
}

/**
 * Runs any code the state needs to keep updating every
 * game cycle, like timers and other real-time elements.
 */
void State::think()
{
	for (auto* surface : _surfaces)
	{
		surface->think();
	}
}

/**
 * Takes care of any events from the core game engine,
 * and passes them on to its InteractiveSurface child elements.
 * @param action Pointer to an action.
 */
void State::handle(Action *action)
{
	if (!_modal)
	{
		for (std::vector<Surface*>::reverse_iterator i = _surfaces.rbegin(); i != _surfaces.rend(); ++i)
		{
			InteractiveSurface* j = dynamic_cast<InteractiveSurface*>(*i);
			if (j != 0)
				j->handle(action, this);
		}
	}
	else
	{
		_modal->handle(action, this);
	}
}

/**
 * Blits all the visible Surface child elements onto the
 * display screen, by order of addition.
 */
void State::registerHdSurface(Surface *surface, const std::string &id, const std::string &category)
{
	if (!surface || id.empty() || category.empty()) return;
	_hdSurfaceMeta[surface] = { id, category };
}

bool State::blitSurfaceWithHdOverride(Surface *surface, SDL_Surface *destination)
{
	if (!surface || !destination) return false;
	if (!Options::hdGraphics || destination->format->BitsPerPixel != 32 || !surface->isDisplayVisible())
	{
		surface->blit(destination);
		return false;
	}
	std::string path;
	auto explicitIt = _hdExplicitSurfacePath.find(surface);
	if (explicitIt != _hdExplicitSurfacePath.end() && _hdUiCache.exists(explicitIt->second))
	{
		path = explicitIt->second;
	}
	else
	{
		auto it = _hdSurfaceMeta.find(surface);
		if (it != _hdSurfaceMeta.end())
		{
			path = "Resources/TFTD_HD/UI/" + it->second.category + "/" + it->second.id + ".png";
		}
	}
	if (path.empty() || !_hdUiCache.exists(path))
	{
		surface->blit(destination);
		return false;
	}
	HdImage *image = _hdUiCache.get(path);
	if (!image)
	{
		surface->blit(destination);
		return false;
	}
	const int scale = std::max(1, surface->getDisplayScale());
	HdImageCache::blit(destination, *image, surface->getDisplayX(), surface->getDisplayY(),
		surface->getWidth() * scale, surface->getHeight() * scale, surface->getDisplayAlpha(), 0, nullptr, false);
	return true;
}

bool State::blitSurfaceWithHdOverridePhysical(Surface *surface, SDL_Surface *destination, Screen *screen)
{
	if (!surface || !destination || !screen || !Options::hdGraphics || destination->format->BitsPerPixel != 32 || !surface->isDisplayVisible()) return false;

	std::string path;
	auto explicitIt = _hdExplicitSurfacePath.find(surface);
	if (explicitIt != _hdExplicitSurfacePath.end() && _hdUiCache.exists(explicitIt->second))
	{
		path = explicitIt->second;
	}
	else
	{
		auto it = _hdSurfaceMeta.find(surface);
		if (it != _hdSurfaceMeta.end())
		{
			path = "Resources/TFTD_HD/UI/" + it->second.category + "/" + it->second.id + ".png";
		}
	}
	if (path.empty() || !_hdUiCache.exists(path)) return false;
	HdImage *image = _hdUiCache.get(path);
	if (!image) return false;

	const int logicalScale = std::max(1, surface->getDisplayScale());
	const int lx = surface->getDisplayX();
	const int ly = surface->getDisplayY();
	const int lx2 = lx + surface->getWidth() * logicalScale;
	const int ly2 = ly + surface->getHeight() * logicalScale;
	const int px = screen->logicalToPhysicalX(lx);
	const int py = screen->logicalToPhysicalY(ly);
	const int pw = std::max(1, screen->logicalToPhysicalX(lx2) - px);
	const int ph = std::max(1, screen->logicalToPhysicalY(ly2) - py);

	// TEST3/TEST6 UI cache: resample the PNG only when its physical target size
	// changes. SDL then performs a single cached blit on every following frame.
	HdPhysicalSurfaceCache &cached = _hdPhysicalUiCache[surface];
	if (!cached.surface || cached.path != path || cached.width != pw || cached.height != ph)
	{
		if (cached.surface) SDL_FreeSurface(cached.surface);
#if SDL_BYTEORDER == SDL_BIG_ENDIAN
		const Uint32 rmask = 0xff000000, gmask = 0x00ff0000, bmask = 0x0000ff00, amask = 0x000000ff;
#else
		const Uint32 rmask = 0x000000ff, gmask = 0x0000ff00, bmask = 0x00ff0000, amask = 0xff000000;
#endif
		cached.surface = SDL_CreateRGBSurface(SDL_SWSURFACE, pw, ph, 32, rmask, gmask, bmask, amask);
		cached.path = path;
		cached.width = pw;
		cached.height = ph;
		cached.hasPerPixelAlpha = false;
		if (!cached.surface) return false;

		if (!SDL_MUSTLOCK(cached.surface) || SDL_LockSurface(cached.surface) == 0)
		{
			for (int yy = 0; yy < ph; ++yy)
			{
				const unsigned sy = std::min(image->height - 1, (unsigned)((long long)yy * image->height / ph));
				for (int xx = 0; xx < pw; ++xx)
				{
					const unsigned sx = std::min(image->width - 1, (unsigned)((long long)xx * image->width / pw));
					const size_t si = ((size_t)sy * image->width + sx) * 4;
					const Uint8 a = image->rgba[si + 3];
					if (a != 255) cached.hasPerPixelAlpha = true;
					const Uint32 pixel = SDL_MapRGBA(cached.surface->format, image->rgba[si], image->rgba[si + 1], image->rgba[si + 2], a);
					memcpy((Uint8*)cached.surface->pixels + yy * cached.surface->pitch + xx * 4, &pixel, 4);
				}
			}
			if (SDL_MUSTLOCK(cached.surface)) SDL_UnlockSurface(cached.surface);
		}
		SDL_SetAlpha(cached.surface, cached.hasPerPixelAlpha ? SDL_SRCALPHA : 0, 255);
	}

	const Uint32 oldFlags = cached.surface->flags;
	const Uint8 oldAlpha = cached.surface->format->alpha;
	if (surface->getDisplayAlpha() < 255)
	{
		SDL_SetAlpha(cached.surface, SDL_SRCALPHA, surface->getDisplayAlpha());
	}
	SDL_Rect dst = { (Sint16)px, (Sint16)py, 0, 0 };
	HdGpuBackend &gpu = HdGpuBackend::instance();
	if (gpu.isCpuOverlay(destination))
	{
		if (!gpu.blitCpuOverlay(cached.surface, px, py)) return false;
	}
	else
	{
		SDL_BlitSurface(cached.surface, nullptr, destination, &dst);
	}
	if (surface->getDisplayAlpha() < 255)
	{
		SDL_SetAlpha(cached.surface, (oldFlags & SDL_SRCALPHA) ? SDL_SRCALPHA : 0, oldAlpha);
	}
	return true;
}

void State::blit()
{
	SDL_Surface *destination = _game->getScreen()->getSurface();
	const bool physicalHd = Options::hdGraphics;
	for (auto* surface : _surfaces)
	{
		// In the software true-HD path the logical buffer stays purely legacy.
		// The RGBA replacement is sampled once, directly into the final framebuffer.
		if (physicalHd)
			surface->blit(destination);
		else
			blitSurfaceWithHdOverride(surface, destination);
	}
}

void State::blitPhysical(SDL_Surface *destination, Screen *screen, bool redrawLegacy)
{
	if (!Options::hdGraphics || !destination || !screen) return;
	for (auto *surface : _surfaces)
	{
		if (!surface || !surface->isDisplayVisible()) continue;
		if (!blitSurfaceWithHdOverridePhysical(surface, destination, screen) && redrawLegacy)
		{
			screen->blitSurfacePhysical(surface, false, destination);
		}
	}
}

/**
 * Hides all the Surface child elements on display.
 */
void State::hideAll()
{
	for (auto* surface : _surfaces)
	{
		surface->setHidden(true);
	}
}

/**
 * Shows all the hidden Surface child elements.
 */
void State::showAll()
{
	for (auto* surface : _surfaces)
	{
		surface->setHidden(false);
	}
}

/**
 * Resets the status of all the Surface child elements,
 * like unpressing buttons.
 */
void State::resetAll()
{
	for (auto* surface : _surfaces)
	{
		InteractiveSurface *s = dynamic_cast<InteractiveSurface*>(surface);
		if (s != 0)
		{
			s->unpress(this);
			//s->setFocus(false);
		}
	}
}

/**
 * Get the localized text for dictionary key @a id.
 * This function forwards the call to Language::getString(const std::string &).
 * @param id The dictionary key to search for.
 * @return The localized text.
 */
LocalizedText State::tr(const std::string &id) const
{
	return _game->getLanguage()->getString(id);
}

/**
* Get the localized text from dictionary.
* This function forwards the call to Language::getString(const std::string &).
* @param id The (prefix of) dictionary key to search for.
* @param alt Used to construct the (suffix of) dictionary key to search for.
* @return The localized text.
*/
LocalizedText State::trAlt(const std::string &id, int alt) const
{
	std::ostringstream ss;
	ss << id;
	// alt = 0 is the original, alt > 0 are the alternatives
	if (alt > 0)
	{
		ss << "_" << alt;
	}
	return _game->getLanguage()->getString(ss.str());
}

/**
 * Get a modifiable copy of the localized text for dictionary key @a id.
 * This function forwards the call to Language::getString(const std::string &, unsigned).
 * @param id The dictionary key to search for.
 * @param n The number to use for the proper version.
 * @return The localized text.
 */
LocalizedText State::tr(const std::string &id, unsigned n) const
{
	return _game->getLanguage()->getString(id, n);
}

/**
 * Get the localized text for dictionary key @a id.
 * This function forwards the call to Language::getString(const std::string &, SoldierGender).
 * @param id The dictionary key to search for.
 * @param gender Current soldier gender.
 * @return The localized text.
 */
LocalizedText State::tr(const std::string &id, SoldierGender gender) const
{
	return _game->getLanguage()->getString(id, gender);
}

/**
 * centers all the surfaces on the screen.
 */
void State::centerAllSurfaces()
{
	for (auto* surface : _surfaces)
	{
		surface->setX(surface->getX() + _game->getScreen()->getDX());
		surface->setY(surface->getY() + _game->getScreen()->getDY());
	}
	refreshPresentationScaleAnchors();
}

/**
 * drop all the surfaces by half the screen height
 */
void State::lowerAllSurfaces()
{
	for (auto* surface : _surfaces)
	{
		surface->setY(surface->getY() + _game->getScreen()->getDY() / 2);
	}
}

/**
 * switch all the colours to something a little more battlescape appropriate.
 */
void State::applyBattlescapeTheme(const std::string& category)
{
	const Element * element = _game->getMod()->getInterface("mainMenu")->getElement("battlescapeTheme");
	std::string altBg = _game->getMod()->getInterface(category)->getAltBackgroundImage();
	if (altBg.empty())
	{
		altBg = "TAC00.SCR";
	}
	for (auto* surface : _surfaces)
	{
		surface->setColor(element->color);
		surface->setHighContrast(true);
		Window* window = dynamic_cast<Window*>(surface);
		if (window)
		{
			window->setBackground(_game->getMod()->getSurface(altBg));
		}
		TextList* list = dynamic_cast<TextList*>(surface);
		if (list)
		{
			list->setArrowColor(element->border);
		}
		ComboBox *combo = dynamic_cast<ComboBox*>(surface);
		if (combo)
		{
			combo->setArrowColor(element->border);
		}
	}
}

/**
 * redraw all the text-type surfaces.
 */
void State::redrawText()
{
	for (auto* surface : _surfaces)
	{
		Text* text = dynamic_cast<Text*>(surface);
		TextButton* button = dynamic_cast<TextButton*>(surface);
		TextEdit* edit = dynamic_cast<TextEdit*>(surface);
		TextList* list = dynamic_cast<TextList*>(surface);
		if (text || button || edit || list)
		{
			surface->draw();
		}
	}
}

/**
 * does the state only have one text list (to scroll)?
 */
bool State::hasOnlyOneScrollableTextList() const
{
	int count = 0;
	for (auto* surface : _surfaces)
	{
		TextList* list = dynamic_cast<TextList*>(surface);
		if (list && (list->getRowsDoNotUse() > list->getVisibleRows()))
		{
			count++;
		}
	}
	return (count == 1);
}

/**
 * Changes the current modal surface. If a surface is modal,
 * then only that surface can receive events. This is used
 * when an element needs to take priority over everything else,
 * eg. focus.
 * @param surface Pointer to modal surface, NULL for no modal.
 */
void State::setModal(InteractiveSurface *surface)
{
	_modal = surface;
}

/**
 * Replaces a certain amount of colors in the state's palette.
 * @param colors Pointer to the set of colors.
 * @param firstcolor Offset of the first color to replace.
 * @param ncolors Amount of colors to replace.
 */
void State::setStatePalette(const SDL_Color *colors, int firstcolor, int ncolors)
{
	if (colors)
	{
		memcpy(_palette + firstcolor, colors, ncolors * sizeof(SDL_Color));
	}
}

/**
 * Set palette for helper surfaces like cursor or fps counter.
 */
void State::setModPalette()
{
	{
		_game->getCursor()->setPalette(_palette);
		_game->getCursor()->draw();
		_game->getFpsCounter()->setPalette(_palette);
		_game->getFpsCounter()->draw();
	}
}

/**
 * Loads palettes from the game resources into the state.
 * @param palette String ID of the palette to load.
 * @param backpals BACKPALS.DAT offset to use.
 */
void State::setStandardPalette(const std::string &palette, int backpals)
{
	setStatePalette(_game->getMod()->getPalette(palette)->getColors(), 0, 256);
	if (palette == "PAL_GEOSCAPE")
	{
		_cursorColor = Mod::GEOSCAPE_CURSOR;
	}
	else if (palette == "PAL_BASESCAPE")
	{
		_cursorColor = Mod::BASESCAPE_CURSOR;
	}
	else if (palette == "PAL_UFOPAEDIA")
	{
		_cursorColor = Mod::UFOPAEDIA_CURSOR;
	}
	else if (palette == "PAL_GRAPHS")
	{
		_cursorColor = Mod::GRAPHS_CURSOR;
	}
	else
	{
		_cursorColor = Mod::BATTLESCAPE_CURSOR;
	}
	if (backpals != -1)
		setStatePalette(_game->getMod()->getPalette("BACKPALS.DAT")->getColors(Palette::blockOffset(backpals)), Palette::backPos, 16);
	setModPalette(); // delay actual update to the end
}

/**
* Loads palettes from the given resources into the state.
* @param colors Pointer to the set of colors.
* @param cursorColor Cursor color to use.
*/
void State::setCustomPalette(SDL_Color *colors, int cursorColor)
{
	setStatePalette(colors, 0, 256);
	_cursorColor = cursorColor;
	setModPalette(); // delay actual update to the end
}

/**
 * Returns the state's 8bpp palette.
 * @return Pointer to the palette's colors.
 */
SDL_Color *State::getPalette()
{
	return _palette;
}

/**
 * Each state will probably need its own resize handling,
 * so this space intentionally left blank
 * @param dX delta of X;
 * @param dY delta of Y;
 */
void State::resize(int &dX, int &dY)
{
	recenter(dX, dY);
}

/**
 * Re-orients all the surfaces in the state.
 * @param dX delta of X;
 * @param dY delta of Y;
 */
void State::recenter(int dX, int dY)
{
	for (auto* surface : _surfaces)
	{
		surface->setX(surface->getX() + dX / 2);
		surface->setY(surface->getY() + dY / 2);
	}
	refreshPresentationScaleAnchors();
}

int State::getCursorX() const
{
	return _game->getCursor()->getX();
}

int State::getCursorY() const
{
	return _game->getCursor()->getY();
}

void State::setGamePtr(Game* game)
{
	_game = game;
}

}
