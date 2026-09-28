#include "HdRenderTrace.h"
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
#include "HdUiPicture.h"
#include "PresentationSpaces.h"
#include <SDL_rotozoom.h>
#include <algorithm>
#include <climits>
#include <cstring>
#include <set>
#include <typeinfo>
#include <limits>
#include "InteractiveSurface.h"
#include "Game.h"
#include "Screen.h"
#include "Surface.h"
#include "Language.h"
#include "LocalizedText.h"
#include "Palette.h"
#include "Options.h"
#include "Logger.h"
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
State::State() : _screen(true), _soundPlayed(false), _modal(0), _ruleInterface(0), _ruleInterfaceParent(0), _customSound(nullptr), _presentationScale(1), _uiFamily(UiFamily::Legacy), _uiFamilyTraceLogged(false), _uiFamilyCompositeLegacy(false), _uiFamilyCompositeCanvas(nullptr)
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
	if (_uiFamilyCompositeCanvas)
	{
		SDL_FreeSurface(_uiFamilyCompositeCanvas);
		_uiFamilyCompositeCanvas = nullptr;
	}
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

static const char *uiFamilyName(UiFamily family)
{
	switch (family)
	{
	case UiFamily::Global: return "Global";
	case UiFamily::Battlescape: return "Battlescape";
	case UiFamily::Geoscape: return "Geoscape";
	case UiFamily::Aquanaut: return "Aquanaut";
	default: return "Legacy";
	}
}

void State::setUiFamily(UiFamily family)
{
	_uiFamily = family;
	_uiFamilyTraceLogged = false;
	if (_uiFamily != UiFamily::Legacy)
	{
		// A family-owned presentation space is authoritative. Do not stack the
		// historical State::presentationScale on top of it.
		_presentationScale = 1;
	}
	refreshPresentationScaleAnchors();
	refreshUiFamilyInputTransforms();
	Log(LOG_INFO) << "[UI-FAMILY V1-A][ASSIGN] state=" << typeid(*this).name()
		<< " family=" << uiFamilyName(_uiFamily);
}

bool State::hasFixedUiFamilyPresentation() const
{
	// Explicit family presentation is currently active for Battlescape and
	// Aquanaut. Geoscape/Global remain contracts for later migrations.
	return Options::hdGraphics && (_uiFamily == UiFamily::Battlescape || _uiFamily == UiFamily::Aquanaut);
}

void State::getUiFamilyLogicalSize(int &width, int &height) const
{
	// Battlescape UI golden reference validated throughout the presentation
	// spaces work: 640x360 logical -> 2560x1440 physical at x4.
	if (_uiFamily == UiFamily::Battlescape)
	{
		width = 640;
		height = 360;
		return;
	}
	if (_uiFamily == UiFamily::Aquanaut)
	{
		// AQUANAUT_UI_FAMILY_V1_R2: Inventory content is authored at 320x200,
		// but its validated x4 physical reference is that 320x200 layout centred
		// inside the same 640x360 UI presentation viewport as Battlescape.
		// The authored content is centred by centerAllSurfaces(); aquanautUiScale
		// remains a family-local content scale around this viewport centre.
		width = 640;
		height = 360;
		return;
	}
	width = Screen::ORIGINAL_WIDTH;
	height = Screen::ORIGINAL_HEIGHT;
}

void State::refreshUiFamilyInputTransforms()
{
	if (!_game || !_game->getScreen()) return;
	const bool enabled = hasFixedUiFamilyPresentation();
	int uiW = 0, uiH = 0;
	getUiFamilyLogicalSize(uiW, uiH);
	const PresentationTransform ui = PresentationSpacesContract::uniformUiFit(
		uiW, uiH, Options::displayWidth, Options::displayHeight);

	// BATTLE_UI_FAMILY_V1-C1: a family can retain an independent content
	// scale (e.g. minimapScale) without borrowing the World transform.
	// Surface::blit scales around the family centre. The inverse input affine
	// below maps physical pixels back into the ORIGINAL widget coordinates, so
	// legacy Action math remains valid at contentScale 1..4.
	const int contentScale = std::max(1, _presentationScale);
	const double inputScaleX = ui.scaleX * contentScale;
	const double inputScaleY = ui.scaleY * contentScale;
	const double anchorX = uiW / 2.0;
	const double anchorY = uiH / 2.0;
	const int inputPhysicalX = (int)std::lround(ui.physicalContent.x + ui.scaleX * anchorX * (1.0 - contentScale));
	const int inputPhysicalY = (int)std::lround(ui.physicalContent.y + ui.scaleY * anchorY * (1.0 - contentScale));

	for (Surface *surface : _surfaces)
	{
		InteractiveSurface *interactive = dynamic_cast<InteractiveSurface*>(surface);
		if (!interactive) continue;
		if (enabled)
		{
			interactive->setPresentationInputTransform(
				surface->getX(), surface->getY(), surface->getWidth(), surface->getHeight(),
				inputPhysicalX, inputPhysicalY, inputScaleX, inputScaleY);
		}
		else
		{
			interactive->clearPresentationInputTransform();
		}
	}
}

void State::applyPresentationScaleToSurface(Surface *surface)
{
	if (!surface) return;
	const int scale = std::max(1, _presentationScale);
	if (_uiFamily != UiFamily::Legacy)
	{
		int uiW = 0, uiH = 0;
		getUiFamilyLogicalSize(uiW, uiH);
		surface->setDisplayScale(scale, uiW / 2, uiH / 2);
		return;
	}

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
	// Legacy: historical State display scale. Explicit UiFamily: the same API
	// becomes a FAMILY-LOCAL content scale inside the canonical UI canvas.
	// It never changes World Space or Physical Display.
	_presentationScale = std::max(1, scale);
	refreshPresentationScaleAnchors();
	refreshUiFamilyInputTransforms();
}

/**
 * Set interface data from the ruleset, also sets the palette for the state.
 * @param category Name of the interface set.
 * @param alterPal Should we swap out the backpal colors?
 * @param battleGame Should we use battlescape palette? (this only applies to options screens)
 */
void State::setInterface(const std::string& category, bool alterPal, SavedBattleGame *battleGame)
{
	_traceInterfaceCategory = category;
	// HD/UI architecture: logical coordinates remain 320x200-era values while
	// presentation scale is selected independently by screen family.
	// A popup opened FROM Battlescape belongs to the Battlescape UI family,
	// including the in-battle Options screens. This must take precedence over
	// the generic "options" category or those popups remain microscopic while
	// the tactical HUD is scaled.
	if (_uiFamily == UiFamily::Battlescape)
	{
		// Explicit family beats all historical category/battleGame heuristics.
		// Physical sizing is owned by PresentationSpacesContract.
		_presentationScale = 1;
	}
	else if (battleGame)
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

	// PRESENTATION_SPACES_TRACE_V1: observe the CURRENT heuristic without changing it.
	Log(LOG_INFO) << "[PRESENTATION-SPACES TRACE V1][INTERFACE] state=" << typeid(*this).name()
		<< " category=" << category
		<< " battleGame=" << (battleGame ? 1 : 0)
		<< " presentationScale=" << _presentationScale
		<< " base=" << Options::baseXResolution << "x" << Options::baseYResolution
		<< " ui(battle/geo/aqua)=" << Options::getBattleUiScale() << "/"
		<< Options::getGeoUiScale() << "/" << Options::getAquanautUiScale()
		<< " world(battle/geo)=" << Options::battlescapeScale << "/" << Options::geoscapeScale;

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
	window->setBackground(bgImage, bgImageName);
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
		if (Options::hdGraphics && parent && parent->hasHdPicture())
			bsbtn->setHdPicture(parent->getHdPicture().cropped({
				double(bsbtn->getX() - parent->getX()), double(bsbtn->getY() - parent->getY()),
				double(bsbtn->getWidth()), double(bsbtn->getHeight())}));
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

	// PRESENTATION_SPACES_TRACE_V1: snapshot the state composition in both the
	// original logical coordinate system and the current per-surface display space.
	// This is diagnostics only: no transform or surface property is changed here.
	int logicalMinX = std::numeric_limits<int>::max();
	int logicalMinY = std::numeric_limits<int>::max();
	int logicalMaxX = std::numeric_limits<int>::min();
	int logicalMaxY = std::numeric_limits<int>::min();
	int displayMinX = std::numeric_limits<int>::max();
	int displayMinY = std::numeric_limits<int>::max();
	int displayMaxX = std::numeric_limits<int>::min();
	int displayMaxY = std::numeric_limits<int>::min();
	size_t visibleCount = 0;
	for (Surface *surface : _surfaces)
	{
		if (!surface || !surface->isDisplayVisible()) continue;
		++visibleCount;
		logicalMinX = std::min(logicalMinX, surface->getX());
		logicalMinY = std::min(logicalMinY, surface->getY());
		logicalMaxX = std::max(logicalMaxX, surface->getX() + surface->getWidth());
		logicalMaxY = std::max(logicalMaxY, surface->getY() + surface->getHeight());
		const int ds = std::max(1, surface->getDisplayScale());
		displayMinX = std::min(displayMinX, surface->getDisplayX());
		displayMinY = std::min(displayMinY, surface->getDisplayY());
		displayMaxX = std::max(displayMaxX, surface->getDisplayX() + surface->getWidth() * ds);
		displayMaxY = std::max(displayMaxY, surface->getDisplayY() + surface->getHeight() * ds);
	}
	const Screen *traceScreen = _game->getScreen();
	const PresentationContext &pc = traceScreen->getPresentationContext();
	Log(LOG_INFO) << "[PRESENTATION-SPACES TRACE V1][STATE] state=" << typeid(*this).name()
		<< " category=" << (_traceInterfaceCategory.empty() ? "<unset>" : _traceInterfaceCategory)
		<< " pScale=" << _presentationScale
		<< " surfaces=" << _surfaces.size() << " visible=" << visibleCount
		<< " optionsBase=" << Options::baseXResolution << "x" << Options::baseYResolution
		<< " pcLogical=" << pc.logicalWidth() << "x" << pc.logicalHeight()
		<< " pcPhysical=" << pc.physicalWidth() << "x" << pc.physicalHeight()
		<< " pcScale=" << pc.scaleX() << "x" << pc.scaleY()
		<< (visibleCount ? (std::string(" logicalBounds=") + std::to_string(logicalMinX) + "," + std::to_string(logicalMinY) + "," + std::to_string(logicalMaxX) + "," + std::to_string(logicalMaxY)) : std::string(" logicalBounds=<none>"))
		<< (visibleCount ? (std::string(" displayBounds=") + std::to_string(displayMinX) + "," + std::to_string(displayMinY) + "," + std::to_string(displayMaxX) + "," + std::to_string(displayMaxY)) : std::string(" displayBounds=<none>"));

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
	if (hasFixedUiFamilyPresentation()) refreshUiFamilyInputTransforms();
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

std::string State::resolveHdUiOverride(Surface *surface)
{
    std::vector<std::string> names;
    const auto explicitIt = _hdExplicitSurfacePath.find(surface);
    if (explicitIt != _hdExplicitSurfacePath.end()) names.push_back(explicitIt->second);
    const auto meta = _hdSurfaceMeta.find(surface);
    if (meta != _hdSurfaceMeta.end())
        names.push_back("Resources/TFTD_HD/UI/" + meta->second.category + "/" + meta->second.id + ".png");
    for (const auto &prefix : {std::string("Resources/TFTD_HD/RealHD/"),
        std::string("Resources/TFTD_HD/"), std::string("Resources/TFTD_HD/LegacyIndexed/")})
    {
        for (const auto &name : names)
        {
            const std::string base = "Resources/TFTD_HD/";
            // Explicit custom paths stay authoritative for their own provider.
            const std::string path = name.compare(0, base.size(), base) == 0 &&
                name.compare(base.size(), 3, "UI/") == 0 ? prefix + name.substr(base.size()) :
                (prefix == base ? name : std::string());
            if (!path.empty() && _hdUiCache.usable(path))
            {
                hdTraceRoute("ui-override", name, hdProviderForPath(path), "selected=" + path);
                return path;
            }
        }
    }
    return {};
}

void State::prewarmHdUiResources()
{
	if (!Options::hdGraphics || !hdUiMigrationEnabled()) return;

	std::set<std::string> manifest;
	for (Surface *surface : _surfaces)
	{
		if (!surface) continue;
		auto explicitIt = _hdExplicitSurfacePath.find(surface);
		if (explicitIt != _hdExplicitSurfacePath.end() && _hdUiCache.usable(explicitIt->second))
		{
			manifest.insert(explicitIt->second);
			continue;
		}
		auto metaIt = _hdSurfaceMeta.find(surface);
		if (metaIt == _hdSurfaceMeta.end()) continue;
		const std::string path = "Resources/TFTD_HD/UI/" + metaIt->second.category + "/" + metaIt->second.id + ".png";
		if (_hdUiCache.usable(path)) manifest.insert(path);
	}

	const Uint32 start = SDL_GetTicks();
	size_t loaded = 0;
	for (const std::string &path : manifest)
	{
		_hdUiCache.get(path);
		++loaded;
	}
	Log(LOG_INFO) << "[HD-PREWARM V2] UI manifest=" << manifest.size()
		<< " loaded=" << loaded << " ms=" << (SDL_GetTicks() - start);
	_hdUiCache.markPrewarmComplete();
}

bool State::blitSurfaceWithHdOverride(Surface *surface, SDL_Surface *destination)
{
	if (!surface || !destination) return false;
	if (!Options::hdGraphics || destination->format->BitsPerPixel != 32 || !surface->isDisplayVisible())
	{
		surface->blit(destination);
		return false;
	}
	const std::string path = resolveHdUiOverride(surface);
	if (path.empty() || !_hdUiCache.usable(path))
	{
		surface->blit(destination);
		return false;
	}
	HdImage *image = &_hdUiCache.require(path);
	const int scale = std::max(1, surface->getDisplayScale());
	HdImageCache::blit(destination, *image, surface->getDisplayX(), surface->getDisplayY(),
		surface->getWidth() * scale, surface->getHeight() * scale, surface->getDisplayAlpha(), 0, nullptr, false);
	return true;
}

bool State::blitSurfaceWithHdOverridePhysical(Surface *surface, SDL_Surface *destination, Screen *screen)
{
	if (!surface || !destination || !screen || !Options::hdGraphics || destination->format->BitsPerPixel != 32 || !surface->isDisplayVisible()) return false;
	if (!hdUiMigrationEnabled()) return false;

	const std::string path = resolveHdUiOverride(surface);
	if (path.empty() || !_hdUiCache.usable(path)) return false;
	HdImage *image = &_hdUiCache.require(path);

	const int logicalScale = std::max(1, surface->getDisplayScale());
	const int lx = surface->getDisplayX();
	const int ly = surface->getDisplayY();
	const int lx2 = lx + surface->getWidth() * logicalScale;
	const int ly2 = ly + surface->getHeight() * logicalScale;
	const int px = screen->logicalToPhysicalX(lx);
	const int py = screen->logicalToPhysicalY(ly);
	const int pw = std::max(1, screen->logicalToPhysicalX(lx2) - px);
	const int ph = std::max(1, screen->logicalToPhysicalY(ly2) - py);

	// Presentation Pipeline V1: authored HD UI no longer has to be resampled
	// into a temporary SDL surface before it reaches D3D11. Submit the native
	// PNG directly to the same physical presentation layer used by Legacy
	// surfaces. CPU resampling below remains the exact fallback path.
	HdGpuBackend &gpu = HdGpuBackend::instance();
	if (gpu.frameActive())
	{
		HdGpuPresentationSprite sprite;
		sprite.assetKey = path.c_str();
		sprite.rgba = image->rgba.data();
		sprite.imageWidth = image->width;
		sprite.imageHeight = image->height;
		sprite.destX = px;
		sprite.destY = py;
		sprite.destW = pw;
		sprite.destH = ph;
		sprite.opacity = surface->getDisplayAlpha();
		if (gpu.drawPresentationSprite(sprite)) return true;
	}

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

bool State::blitSurfaceInUiFamilyPhysical(Surface *surface, SDL_Surface *destination, Screen *screen)
{
	if (!surface || !destination || !screen || !surface->isDisplayVisible()) return false;
	int uiW = 0, uiH = 0;
	getUiFamilyLogicalSize(uiW, uiH);
	const PresentationTransform ui = PresentationSpacesContract::uniformUiFit(
		uiW, uiH, Options::displayWidth, Options::displayHeight);
	const int contentScale = std::max(1, surface->getDisplayScale());
	const PresentationRect logical = { surface->getDisplayX(), surface->getDisplayY(),
		surface->getWidth() * contentScale, surface->getHeight() * contentScale };
	const PresentationRect phys = ui.logicalToPhysical(logical);
	if (phys.w <= 0 || phys.h <= 0) return false;
	if (!hdUiMigrationEnabled())
		return screen->blitNativeSurfaceAt(surface, destination, phys.x, phys.y, phys.w, phys.h);

	const std::string path = resolveHdUiOverride(surface);

	HdGpuBackend &gpu = HdGpuBackend::instance();
	if (!path.empty() && _hdUiCache.usable(path))
	{
		HdImage *image = &_hdUiCache.require(path);
		if (image)
		{
			if (gpu.frameActive())
			{
				HdGpuPresentationSprite sprite;
				sprite.assetKey = path.c_str();
				sprite.rgba = image->rgba.data();
				sprite.imageWidth = image->width;
				sprite.imageHeight = image->height;
				sprite.destX = phys.x;
				sprite.destY = phys.y;
				sprite.destW = phys.w;
				sprite.destH = phys.h;
				sprite.opacity = surface->getDisplayAlpha();
				if (gpu.drawPresentationSprite(sprite)) return true;
			}
			HdImageCache::blit(destination, *image, phys.x, phys.y, phys.w, phys.h,
				surface->getDisplayAlpha(), 0, nullptr, false);
			return true;
		}
	}

	if (screen->tryBlitHdSurfaceAt(surface, destination, phys.x, phys.y, phys.w, phys.h)) return true;
	if (Options::hdGraphics) return false; // Unmigrated OXCE widget stays absent.
	if (screen->blitNativeSurfaceAt(surface, destination, phys.x, phys.y, phys.w, phys.h)) return true;
	SDL_Surface *src = surface->getPresentationSurface();
	if (!src || src->w <= 0 || src->h <= 0) return false;
	if (gpu.isCpuOverlay(destination) && gpu.drawLegacySurface(src, surface,
		phys.x, phys.y, phys.w, phys.h, surface->getDisplayAlpha()))
	{
		return true;
	}

	SDL_Surface *scaled = zoomSurface(src, (double)phys.w / (double)src->w,
		(double)phys.h / (double)src->h, 0);
	if (!scaled) return false;
	if (src->flags & SDL_SRCCOLORKEY)
	{
		SDL_SetColorKey(scaled, SDL_SRCCOLORKEY, src->format->colorkey);
	}
	if (surface->getDisplayAlpha() < 255)
	{
		SDL_SetAlpha(scaled, SDL_SRCALPHA, surface->getDisplayAlpha());
	}
	if (gpu.isCpuOverlay(destination))
	{
		gpu.blitCpuOverlay(scaled, phys.x, phys.y);
	}
	else
	{
		SDL_Rect dst = { (Sint16)phys.x, (Sint16)phys.y, 0, 0 };
		SDL_BlitSurface(scaled, nullptr, destination, &dst);
	}
	SDL_FreeSurface(scaled);
	return true;
}

bool State::blitUiFamilyCompositePhysical(SDL_Surface *destination, Screen *screen)
{
	if (Options::hdGraphics) return false; // Native family raster is forbidden.
	if (!destination || !screen) return false;
	const bool fixed = hasFixedUiFamilyPresentation();
	int uiW = screen->getSurface()->w, uiH = screen->getSurface()->h;
	if (fixed) getUiFamilyLogicalSize(uiW, uiH);
	if (uiW <= 0 || uiH <= 0) return false;

	if (!_uiFamilyCompositeCanvas || _uiFamilyCompositeCanvas->w != uiW || _uiFamilyCompositeCanvas->h != uiH)
	{
		if (_uiFamilyCompositeCanvas) SDL_FreeSurface(_uiFamilyCompositeCanvas);
		_uiFamilyCompositeCanvas = SDL_CreateRGBSurface(SDL_SWSURFACE, uiW, uiH, 8, 0, 0, 0, 0);
		if (!_uiFamilyCompositeCanvas) return false;
		SDL_SetColorKey(_uiFamilyCompositeCanvas, SDL_SRCCOLORKEY, 0);
	}
	if (_uiFamilyCompositeCanvas->format && _uiFamilyCompositeCanvas->format->palette)
	{
		SDL_SetPalette(_uiFamilyCompositeCanvas, SDL_LOGPAL | SDL_PHYSPAL, _palette, 0, 256);
	}
	SDL_FillRect(_uiFamilyCompositeCanvas, nullptr, 0);

	// Rich Legacy controls (ComboBox, Slider, TextList...) often own visual
	// children that are not registered in State::_surfaces. Calling their
	// historical blit() onto a canonical UI canvas preserves those semantics
	// exactly, while the canvas itself is no longer tied to World Space.
	for (Surface *surface : _surfaces)
	{
		if (!surface || !surface->isDisplayVisible()) continue;
		surface->blit(_uiFamilyCompositeCanvas);
	}

	const PresentationTransform ui = PresentationSpacesContract::uniformUiFit(
		uiW, uiH, Options::displayWidth, Options::displayHeight);
	const PresentationRect phys = fixed ? ui.physicalContent : PresentationRect{
		screen->logicalToPhysicalX(0), screen->logicalToPhysicalY(0),
		screen->logicalToPhysicalX(uiW) - screen->logicalToPhysicalX(0),
		screen->logicalToPhysicalY(uiH) - screen->logicalToPhysicalY(0)};
	if (phys.w <= 0 || phys.h <= 0) return false;

	HdGpuBackend &gpu = HdGpuBackend::instance();
	if (gpu.isCpuOverlay(destination) && gpu.drawLegacySurface(_uiFamilyCompositeCanvas, this,
		phys.x, phys.y, phys.w, phys.h, 255))
	{
		return true;
	}

	SDL_Surface *scaled = zoomSurface(_uiFamilyCompositeCanvas,
		(double)phys.w / (double)uiW, (double)phys.h / (double)uiH, 0);
	if (!scaled) return false;
	SDL_SetColorKey(scaled, SDL_SRCCOLORKEY, 0);
	if (gpu.isCpuOverlay(destination))
	{
		gpu.blitCpuOverlay(scaled, phys.x, phys.y);
	}
	else
	{
		SDL_Rect dst = { (Sint16)phys.x, (Sint16)phys.y, 0, 0 };
		SDL_BlitSurface(scaled, nullptr, destination, &dst);
	}
	SDL_FreeSurface(scaled);
	return true;
}

void State::blit()
{
	// BATTLE_UI_FAMILY_V1-B: a family-owned overlay must not be baked into
	// the variable World logical canvas. It is presented once in blitPhysical().
	if (Options::hdGraphics) return;
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
	screen->setHdTraceContext(std::string(typeid(*this).name()) + ":" + _traceInterfaceCategory, hdUiMigrationEnabled());
	if (!hdUiMigrationEnabled())
	{
		hdTraceRoute("ui-policy", std::string(typeid(*this).name()) + ":" + _traceInterfaceCategory,
			"REAL_HD_MISSING", "Native family raster forbidden; migrate HD producer", true);
	}
	if (hasFixedUiFamilyPresentation())
	{
		if (!_uiFamilyTraceLogged)
		{
			int uiW = 0, uiH = 0;
			getUiFamilyLogicalSize(uiW, uiH);
			const PresentationTransform ui = PresentationSpacesContract::uniformUiFit(
				uiW, uiH, Options::displayWidth, Options::displayHeight);
			Log(LOG_INFO) << "[BATTLE-UI FAMILY V1-B][CONFIG] state=" << typeid(*this).name()
				<< " family=" << uiFamilyName(_uiFamily)
				<< " uiLogical=" << uiW << "x" << uiH
				<< " uiContent=" << ui.physicalContent.x << "," << ui.physicalContent.y << ","
				<< ui.physicalContent.w << "x" << ui.physicalContent.h
				<< " uiScale=" << ui.scaleX << "x" << ui.scaleY
				<< " contentScale=" << std::max(1, _presentationScale)
				<< " surfaces=" << _surfaces.size();
			_uiFamilyTraceLogged = true;
		}
		// P2: each widget tries its HD producer, then a separately traced native fallback.
		for (Surface *surface : _surfaces)
		{
			if (!surface || !surface->isDisplayVisible()) continue;
			blitSurfaceInUiFamilyPhysical(surface, destination, screen);
		}
		return;
	}
	for (auto *surface : _surfaces)
	{
		if (!surface || !surface->isDisplayVisible()) continue;
		if (!blitSurfaceWithHdOverridePhysical(surface, destination, screen))
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
	int dX = _game->getScreen()->getDX();
	int dY = _game->getScreen()->getDY();
	if (_uiFamily != UiFamily::Legacy)
	{
		// Explicit UI families centre against THEIR canonical canvas, never
		// against the current World/Screen canvas.  For Aquanaut 320x200 this
		// naturally yields 0,0; Battlescape keeps the validated +160,+80 shift.
		int uiW = 0, uiH = 0;
		getUiFamilyLogicalSize(uiW, uiH);
		dX = (uiW - Screen::ORIGINAL_WIDTH) / 2;
		dY = (uiH - Screen::ORIGINAL_HEIGHT) / 2;
	}
	for (auto* surface : _surfaces)
	{
		surface->setX(surface->getX() + dX);
		surface->setY(surface->getY() + dY);
	}
	refreshPresentationScaleAnchors();
	refreshUiFamilyInputTransforms();
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
			window->setBackground(_game->getMod()->getSurface(altBg), altBg);
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
	if (_uiFamily == UiFamily::Battlescape)
	{
		// World canvas changes must not move a family-owned UI composition.
		refreshPresentationScaleAnchors();
		refreshUiFamilyInputTransforms();
		return;
	}
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
