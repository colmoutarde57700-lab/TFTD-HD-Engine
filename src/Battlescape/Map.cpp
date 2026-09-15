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
#include "Map.h"
#include <algorithm>
#include <iomanip>
#include <sstream>
#include <map>
#include <unordered_map>
#include <SDL_rotozoom.h>
#include "Camera.h"
#include "UnitSprite.h"
#include "ItemSprite.h"
#include "Pathfinding.h"
#include "TileEngine.h"
#include "Projectile.h"
#include "Explosion.h"
#include "BattlescapeState.h"
#include "Particle.h"
#include "../Mod/Mod.h"
#include "../Engine/Action.h"
#include "../Engine/SurfaceSet.h"
#include "../Engine/Timer.h"
#include "../Engine/Language.h"
#include "../Engine/Palette.h"
#include "../Engine/Game.h"
#include "../Engine/Screen.h"
#include "../Engine/HdRenderSpace.h"
#include "../Engine/HdGpuBackend.h"
#include "../Engine/HdColorTransform.h"
#include "../Engine/Options.h"
#include "../Engine/FileMap.h"
#include "../Engine/Logger.h"
#include "../Engine/HdPerf.h"
#include "../lodepng.h"
#include "../Engine/ShaderDraw.h"
#include "../Engine/ShaderMove.h"
#include "../Savegame/SavedBattleGame.h"
#include "../Savegame/Tile.h"
#include "../Savegame/BattleUnit.h"
#include "../Savegame/BattleItem.h"
#include "../Ufopaedia/Ufopaedia.h"
#include "../Mod/RuleItem.h"
#include "../Mod/RuleInterface.h"
#include "../Mod/MapDataSet.h"
#include "../Mod/MapData.h"
#include "../Mod/Armor.h"
#include "../Mod/RuleEnviroEffects.h"
#include "BattlescapeMessage.h"
#include "../Savegame/SavedGame.h"
#include "../Interface/NumberText.h"
#include "../Interface/Text.h"
#include "../fmath.h"


/*
  1) Map origin is top corner.
  2) X axis goes downright. (width of the map)
  3) Y axis goes downleft. (length of the map
  4) Z axis goes up (height of the map)

           0,0
            /\
           /  \
        y+ \  / x+
            \/

  Compass directions

         W  /\  N
           /  \
           \  /
         S  \/  E

  Unit directions

         6  /\  0
           /  \
           \  /
         4  \/  2

  Big units parts

            /\
           /0 \
          /\  /\
         /2 \/1 \
         \  /\  /
          \/3 \/
           \  /
            \/
 */


namespace
{

struct HdMaterialProfileRule
{
	std::string defaultProfile;
	std::unordered_map<int, std::string> frameProfiles;
};

/**
 * Optional mod-side visual material classification.  This is deliberately
 * presentation-only: it never changes MCD/MAP membership, TilePart, collision,
 * height or destruction semantics.  The sidecar is a normal VFS resource (not
 * an OXCE ruleset) so the current mod can ship it as .yml without changing any
 * existing ruleset loading behaviour.
 */
const std::map<std::string, HdMaterialProfileRule> &hdMaterialProfileRules()
{
	static bool loaded = false;
	static std::map<std::string, HdMaterialProfileRule> rules;
	if (loaded) return rules;
	loaded = true;

	const std::string filename = "Ruleset/TFTD_HD_material_profiles.yml";
	if (!OpenXcom::FileMap::fileExists(filename)) return rules;

	try
	{
		const OpenXcom::YAML::YamlRootNodeReader root = OpenXcom::FileMap::getYAML(filename);
		const auto profiles = root["hdMaterialProfiles"];
		for (const auto &datasetReader : profiles.children())
		{
			const std::string dataset = OpenXcom::hdLower(std::string(datasetReader.key()));
			if (dataset.empty()) continue;

			HdMaterialProfileRule rule;
			rule.defaultProfile = dataset;
			datasetReader.tryRead("default", rule.defaultProfile);
			rule.defaultProfile = OpenXcom::hdLower(rule.defaultProfile);

			const auto frames = datasetReader["frames"];
			for (const auto &frameReader : frames.children())
			{
				try
				{
					const int frame = std::stoi(std::string(frameReader.key()));
					const std::string profile = OpenXcom::hdLower(frameReader.readVal<std::string>());
					if (!profile.empty()) rule.frameProfiles[frame] = profile;
				}
				catch (...)
				{
					// Ignore malformed individual frame keys; the dataset default remains valid.
				}
			}
			rules[dataset] = std::move(rule);
		}
		OpenXcom::Logger().get(OpenXcom::LOG_INFO) << "[HD-MATERIAL] loaded " << rules.size()
			<< " dataset material profiles from " << filename;
	}
	catch (const std::exception &e)
	{
		OpenXcom::Logger().get(OpenXcom::LOG_WARNING) << "[HD-MATERIAL] failed to read " << filename << ": " << e.what();
	}
	return rules;
}

std::string hdMaterialProfileForTerrain(const std::string &datasetName, int sprite)
{
	const std::string dataset = OpenXcom::hdLower(datasetName);
	const auto &rules = hdMaterialProfileRules();
	const auto it = rules.find(dataset);
	// No sidecar rule: keep the existing asset-path profile exactly as before.
	if (it == rules.end()) return std::string();

	const auto frame = it->second.frameProfiles.find(sprite);
	if (frame != it->second.frameProfiles.end()) return frame->second;
	return it->second.defaultProfile.empty() ? dataset : it->second.defaultProfile;
}

OpenXcom::HdEnvironmentProfile hdEnvironmentProfileForMaterialProfile(const std::string &materialProfile)
{
	const std::string profile = OpenXcom::hdLower(materialProfile);
	if (profile == "coral_bubble" || profile == "coralbubble")
		return OpenXcom::hdEnvironmentProfileFromBlockMask(0x8000u); // Legacy F0 cyan block.

	return OpenXcom::hdEnvironmentProfileFromBlockMask(OpenXcom::hdTerrainDatasetBlockMask(profile));
}

}

namespace OpenXcom
{

static const SDL_Color *hdNeutralBattlescapePalette(Game *game, const SDL_Color *fallback)
{
	if (game && game->getMod())
	{
		// Always calibrate authored HD colour from the immutable surface palette.
		// OXCE keeps palette backups specifically so later mission/mod operations
		// cannot move the reference beneath the environment transform.
		Palette *palette = game->getMod()->getPalette("BACKUP_PAL_BATTLESCAPE", false);
		if (!palette || palette->getColorCount() < 256)
			palette = game->getMod()->getPalette("PAL_BATTLESCAPE", false);
		if (palette && palette->getColorCount() >= 256) return palette->getColors();
	}
	return fallback;
}

/**
 * P8 colour semantics are explicit and independent from PNG storage.
 *
 * Convention namespaces:
 *   Resources/TFTD_HD/Terrain/...               -> Environment (default authored HD)
 *   Resources/TFTD_HD/LegacyIndexed/Terrain/... -> IndexedLegacy (exact TFTD indices)
 *   Resources/TFTD_HD/Fixed/Terrain/...         -> Fixed (diagnostic/UI-like colour)
 *
 * Rule-driven assets may still override this with colorMode explicitly.
 */
static std::string hdColorModeForAssetPath(const std::string &assetPath, const std::string &requested)
{
	if (parseHdColorMode(requested) != HdColorMode::Auto) return requested;
	if (assetPath.find("Resources/TFTD_HD/LegacyIndexed/") == 0) return "indexedLegacy";
	if (assetPath.find("Resources/TFTD_HD/Fixed/") == 0) return "fixed";
	return "environment";
}

static std::array<HdEnvironmentTransform, HdEnvironmentProfileCount> &hdEnvironmentProfiles()
{
	// Presentation-only cache. Map rendering is single-threaded; signatures inside
	// each LUT rebuild automatically when OXCE switches D0/D1/D2/D3. Keeping the
	// cache here avoids changing Map's object layout for this deliberately small test.
	static std::array<HdEnvironmentTransform, HdEnvironmentProfileCount> profiles;
	return profiles;
}

static void hdBuildEnvironmentProfiles(const SDL_Color *reference, const SDL_Color *active)
{
	auto &profiles = hdEnvironmentProfiles();
	for (size_t i = 0; i < HdEnvironmentProfileCount; ++i)
	{
		const HdEnvironmentProfile profile = (HdEnvironmentProfile)i;
		profiles[i].build(reference, active, hdEnvironmentProfileBlockMask(profile));
	}
}

static HdEnvironmentTransform &hdEnvironmentTransformFor(HdEnvironmentProfile profile)
{
	return hdEnvironmentProfiles()[hdEnvironmentProfileIndex(profile)];
}

/**
 * Sets up a map with the specified size and position.
 * @param game Pointer to the core game.
 * @param width Width in pixels.
 * @param height Height in pixels.
 * @param x X position in pixels.
 * @param y Y position in pixels.
 * @param visibleMapHeight Current visible map height.
 */
Map::Map(Game *game, int width, int height, int x, int y, int visibleMapHeight) : InteractiveSurface(width, height, x, y),
	_game(game), _isTFTD(false), _arrow(0), _anyIndicator(false), _isAltPressed(false), _isCtrlPressed(false),
	_selectorX(0), _selectorY(0), _mouseX(0), _mouseY(0), _cursorType(CT_NORMAL), _cursorSize(1), _animFrame(0),
	_projectile(0), _followProjectile(true), _projectileInFOV(false), _explosionInFOV(false), _launch(false), _visibleMapHeight(visibleMapHeight),
	_unitDying(false), _smoothingEngaged(false), _flashScreen(false), _bgColor(15), _projectileSet(0), _drawSequence(0), _hdLastRealTick(SDL_GetTicks()),
	_hdPhysicalRevision(0), _hdPhysicalCachedRevision(~0ULL), _hdPhysicalCurrentCacheIndex(-1), _hdPhysicalUseCounter(0), _hdPhysicalCacheMaxEntries(0),
	_hdPhysicalCacheX(0), _hdPhysicalCacheY(0), _hdPhysicalCacheW(0), _hdPhysicalCacheH(0),
	_hdPhysicalCacheScaleX(0.0), _hdPhysicalCacheScaleY(0.0), _hdPhysicalLastMs(0), _hdPhysicalPixelsTested(0), _hdPhysicalPixelsWritten(0), _hdPhysicalLastCacheHit(false),
	_hdRedrawTraceTick(SDL_GetTicks()), _hdRedrawPendingMask(0), _hdRedrawLastMask(0),
	_showObstacles(false), _showInfoOnCursor(false)
{
	for (int i = 0; i < HDR_COUNT; ++i)
	{
		_hdRedrawCount[i] = 0;
		_hdRedrawSnapshot[i] = 0;
		_hdRedrawRate[i] = 0;
	}
	// TODO: extract to a better place later
	for (const auto& pair : Options::mods)
	{
		if (pair.second)
		{
			if (pair.first == "xcom2")
			{
				_isTFTD = true;
				break;
			}
		}
	}

	_iconHeight = _game->getMod()->getInterface("battlescape")->getElement("icons")->h;
	_iconWidth = _game->getMod()->getInterface("battlescape")->getElement("icons")->w;
	_messageColor = _game->getMod()->getInterface("battlescape")->getElement("messageWindows")->color;

	auto* itf = _game->getMod()->getInterface("battlescape")->getElement("thinkingProgressBar");
	_hostileBarColor = itf->color;
	_neutralBarColor = itf->color2;
	_borderBarColor = itf->border;

	PathPreview previewSetting = Options::battleNewPreviewPath;
	_smoothCamera = Options::battleSmoothCamera;
	if (Options::traceAI)
	{
		// turn everything on because we want to see the markers.
		previewSetting = PATH_ARROW_TU;
	}
	_previewSettingArrows = previewSetting & PATH_ARROWS;
	_previewSettingTu     = previewSetting & PATH_TU_COST;
	_previewSettingEnergy = previewSetting & PATH_ENERGY_COST;

	_save = _game->getSavedGame()->getSavedBattle();
	if ((int)(_game->getMod()->getLUTs()->size()) > _save->getDepth())
	{
		_transparencies = &_game->getMod()->getLUTs()->at(_save->getDepth());
	}
	else
	{
		const static std::vector<Uint8> dummy;
		_transparencies = &dummy;
	}

	// RC11 architecture: the tactical simulation and every Legacy renderer
	// coordinate stay permanently on the historical 32x40 grid.  Asset
	// resolution is presentation metadata only and must never resize Camera,
	// tiles, corpses, items or unit anchors.
	_spriteWidth = 32;
	_spriteHeight = 40;
	_message = new BattlescapeMessage(320, (visibleMapHeight < 200)? visibleMapHeight : 200, 0, 0);
	_message->setX(_game->getScreen()->getDX());
	_message->setY((visibleMapHeight - _message->getHeight()) / 2);
	_message->setTextColor(_messageColor);
	_camera = new Camera(_spriteWidth, _spriteHeight, _save->getMapSizeX(), _save->getMapSizeY(), _save->getMapSizeZ(), this, visibleMapHeight);
	_scrollMouseTimer = new Timer(SCROLL_INTERVAL);
	_scrollMouseTimer->onTimer((SurfaceHandler)&Map::scrollMouse);
	_scrollKeyTimer = new Timer(SCROLL_INTERVAL);
	_scrollKeyTimer->onTimer((SurfaceHandler)&Map::scrollKey);
	_camera->setScrollTimer(_scrollMouseTimer, _scrollKeyTimer);
	_obstacleTimer = new Timer(2500);
	_obstacleTimer->stop();
	_obstacleTimer->onTimer((SurfaceHandler)&Map::disableObstacles);

	_showInfoOnCursor = (Options::oxceShowAccuracyOnCrosshair == 1 && Options::battleUFOExtenderAccuracy) || Options::oxceShowAccuracyOnCrosshair == 2;
	_txtAccuracy = new Text(44, 18, 0, 0);
	_txtAccuracy->setSmall();
	_txtAccuracy->setPalette(_game->getScreen()->getPalette());
	_txtAccuracy->setHighContrast(true);
	_txtAccuracy->initText(_game->getMod()->getFont("FONT_BIG"), _game->getMod()->getFont("FONT_SMALL"), _game->getLanguage());
	_cacheActiveWeaponUfopediaArticleUnlocked = -1;
	_cacheIsCtrlPressed = false;
	_cacheCursorPosition = TileEngine::invalid;
	_cacheHasLOS = -1;

	_nightVisionOn = false;
	if (Options::oxceToggleNightVisionType == 2)
	{
		// persisted per campaign
		_nightVisionOn = _game->getSavedGame()->getToggleNightVision();
	}
	else if (Options::oxceToggleNightVisionType == 1)
	{
		// persisted per battle
		_nightVisionOn = _save->getToggleNightVision();
	}

	_debugVisionMode = 0;
	if (Options::oxceToggleBrightnessType == 2)
	{
		// persisted per campaign
		_debugVisionMode = _game->getSavedGame()->getToggleBrightness();
	}
	else if (Options::oxceToggleBrightnessType == 1)
	{
		// persisted per battle
		_debugVisionMode = _save->getToggleBrightness();
	}

	_save->setToggleNightVisionTemp(false);
	_save->setToggleNightVisionColorTemp(0);
	_save->setToggleBrightnessTemp(_debugVisionMode);

	_fadeShade = 16;
	_nvColor = 0;
	_fadeTimer = new Timer(FADE_INTERVAL);
	_fadeTimer->onTimer((SurfaceHandler)&Map::fadeShade);
	_fadeTimer->start();

	auto* enviro = _save->getEnviroEffects();
	if (enviro)
	{
		_bgColor = enviro->getMapBackgroundColor();
	}

	_stunIndicator = _game->getMod()->getSurface("FloorStunIndicator", false);
	_woundIndicator = _game->getMod()->getSurface("FloorWoundIndicator", false);
	_burnIndicator = _game->getMod()->getSurface("FloorBurnIndicator", false);
	_shockIndicator = _game->getMod()->getSurface("FloorShockIndicator", false);
	_anyIndicator = _stunIndicator || _woundIndicator || _burnIndicator || _shockIndicator;

	if (enviro)
	{
		if (!enviro->getMapShockIndicator().empty())
		{
			_shockIndicator = _game->getMod()->getSurface(enviro->getMapShockIndicator(), false);
		}
	}

	_vaporParticlesInit.resize(_camera->getMapSizeY() * _camera->getMapSizeX());
	_vaporParticles.resize(_camera->getMapSizeY() * _camera->getMapSizeX());
}

/**
 * Deletes the map.
 */
Map::~Map()
{
	delete _scrollMouseTimer;
	delete _scrollKeyTimer;
	delete _fadeTimer;
	delete _obstacleTimer;
	delete _arrow;
	delete _message;
	delete _camera;
	delete _txtAccuracy;
}

/**
 * Initializes the map.
 */
void Map::init()
{
	// load the tiny arrow into a surface
	int f = Palette::blockOffset(1); // yellow
	int b = 15; // black
	int pixels[81] = { 0, 0, b, b, b, b, b, 0, 0,
					   0, 0, b, f, f, f, b, 0, 0,
					   0, 0, b, f, f, f, b, 0, 0,
					   b, b, b, f, f, f, b, b, b,
					   b, f, f, f, f, f, f, f, b,
					   0, b, f, f, f, f, f, b, 0,
					   0, 0, b, f, f, f, b, 0, 0,
					   0, 0, 0, b, f, b, 0, 0, 0,
					   0, 0, 0, 0, b, 0, 0, 0, 0 };

	const int graphicsScale = std::max(1, _spriteWidth / 32);
	_arrow = new Surface(9 * graphicsScale, 9 * graphicsScale);
	_arrow->setPalette(this->getPalette());
	_arrow->lock();
	for (int y = 0; y < 9; ++y)
	{
		for (int x = 0; x < 9; ++x)
		{
			const Uint8 pixel = pixels[x + (y * 9)];
			for (int yy = 0; yy < graphicsScale; ++yy)
			{
				for (int xx = 0; xx < graphicsScale; ++xx)
				{
					_arrow->setPixel(x * graphicsScale + xx, y * graphicsScale + yy, pixel);
				}
			}
		}
	}
	_arrow->unlock();

	_projectile = 0;
	if (_save->getDepth() == 0)
	{
		_projectileSet = _game->getMod()->getSurfaceSet("Projectiles");
	}
	else
	{
		_projectileSet = _game->getMod()->getSurfaceSet("UnderwaterProjectiles");
	}
}

/**
 * Keeps the animation timers running.
 */
void Map::think()
{
	// Presentation-only HD clock. This deliberately does not use the historical
	// 100 ms Battlescape animation frame, so a ruleset may request e.g. 16/33/50 ms
	// frames without touching gameplay timing. Very large gaps (minimise/debugger)
	// are ignored instead of fast-forwarding visual state machines by hours.
	const Uint32 now = SDL_GetTicks();
	const Uint32 delta = now - _hdLastRealTick; // unsigned subtraction also handles SDL tick wrap
	_hdLastRealTick = now;
	if (Options::hdGraphics && delta <= 1000)
	{
		_save->advanceHdVisualTime(delta);
	}

	updateHdRedrawTraceRates();

	_scrollMouseTimer->think(0, this);
	_scrollKeyTimer->think(0, this);
	_fadeTimer->think(0, this);
	_obstacleTimer->think(0, this);
}

/**
 * Draws the whole map, part by part.
 */
void Map::draw()
{
	if (!_redraw)
	{
		return;
	}
	const uint64_t perfMapDrawStart = hdPerfNowUs();
	getHdPerfStats().current.mapRedraw = true;
	_hdRedrawLastMask = _hdRedrawPendingMask;
	_hdRedrawPendingMask = 0;

	// normally we'd call for a Surface::draw();
	// but we don't want to clear the background with colour 0, which is transparent (aka black)
	// we use colour 15 because that actually corresponds to the colour we DO want in all variations of the xcom and tftd palettes.
	// Note: un-hardcoded the color from 15 to ruleset value, default 15
	_redraw = false;
	ShaderDrawFunc(
		[](Uint8& dest, Uint8 color)
		{
			dest = color;
		},
		ShaderSurface(this),
		ShaderScalar<Uint8>(Palette::blockOffset(0) + _bgColor)
	);

	Tile *t;

	_projectileInFOV = _save->getDebugMode();
	if (_projectile)
	{
		t = _save->getTile(_projectile->getPosition(0).toTile());
		if (_save->getSide() == FACTION_PLAYER || (t && t->getVisible()))
		{
			_projectileInFOV = true;
		}
	}
	_explosionInFOV = _save->getDebugMode();
	if (!_explosions.empty())
	{
		for (auto* explosion : _explosions)
		{
			if (explosion->isBig())
			{
				_explosionInFOV = true;
				break;
			}
			t = _save->getTile(explosion->getPosition().toTile());
			if (t && t->getVisible())
			{
				_explosionInFOV = true;
				break;
			}
		}
	}

	if ((_save->getSelectedUnit() && _save->getSelectedUnit()->getVisible()) || _unitDying || _save->getSide() == FACTION_PLAYER || _save->getDebugMode() || _projectileInFOV || _explosionInFOV)
	{
		drawTerrain(this);
	}
	else
	{
		// Hidden-movement/end-turn screens replace the tactical map entirely.
		// HD assets are composited after the legacy map surface, so commands
		// cached from the previous visible tactical frame must not survive here.
		// Otherwise a full-body RGBA unit would appear as a ghost over the
		// hidden-movement background even though legacy units are correctly hidden.
		_hdDrawCommands.clear();
		_drawOrderBuffer.clear();
		_drawSequence = 0;
		_message->blit(this->getSurface());
	}

	// Map::draw() is OXCE's authoritative scene invalidation point.  Every
	// successful redraw gets one revision; mouse-only presentation frames do not.
	++_hdPhysicalRevision;
	getHdPerfStats().current.mapDrawUs = hdPerfNowUs() - perfMapDrawStart;
}

void Map::refreshAIProgress(int progress)
{
	if (_save->getSide() == FACTION_NEUTRAL)
	{
		_message->setProgressBarColor(_neutralBarColor, _borderBarColor);
	}
	else
	{
		_message->setProgressBarColor(_hostileBarColor, _borderBarColor);
	}
	_message->setProgressValue(progress);
}

/**
 * TEST8-F: records the origin of a Map invalidation.
 */
void Map::invalidateHd(HdRedrawReason reason)
{
	if (reason < HDR_EXTERNAL || reason >= HDR_COUNT) reason = HDR_EXTERNAL;
	++_hdRedrawCount[reason];
	_hdRedrawPendingMask |= (1u << (unsigned)reason);
	Surface::invalidate(true);
}

/**
 * Catch unclassified Map invalidations.
 */
void Map::invalidate(bool valid)
{
	if (valid) invalidateHd(HDR_EXTERNAL);
	else Surface::invalidate(false);
}

void Map::updateHdRedrawTraceRates()
{
	const Uint32 now = SDL_GetTicks();
	const Uint32 elapsed = now - _hdRedrawTraceTick;
	if (elapsed < 1000) return;
	for (int i = 0; i < HDR_COUNT; ++i)
	{
		const uint64_t delta = _hdRedrawCount[i] - _hdRedrawSnapshot[i];
		_hdRedrawRate[i] = elapsed ? ((delta * 1000ULL + elapsed / 2) / elapsed) : delta;
		_hdRedrawSnapshot[i] = _hdRedrawCount[i];
	}
	_hdRedrawTraceTick = now;
}

std::string Map::getHdRedrawTrace() const
{
	static const char *labels[HDR_COUNT] = { "EXT", "CAM", "SEL", "ANIM", "STATE", "ATK", "SIZE" };
	std::ostringstream out;
	out << "R/s ext=" << _hdRedrawRate[HDR_EXTERNAL]
		<< " cam=" << _hdRedrawRate[HDR_CAMERA]
		<< " sel=" << _hdRedrawRate[HDR_SELECTOR]
		<< " anim=" << _hdRedrawRate[HDR_ANIMATION]
		<< " state=" << _hdRedrawRate[HDR_STATE_QUEUE]
		<< " atk=" << _hdRedrawRate[HDR_ATTACK]
		<< " size=" << _hdRedrawRate[HDR_RESIZE]
		<< "  last=";
	if (_hdRedrawLastMask == 0) out << "NONE";
	else
	{
		bool first = true;
		for (int i = 0; i < HDR_COUNT; ++i)
		{
			if ((_hdRedrawLastMask & (1u << (unsigned)i)) == 0) continue;
			if (!first) out << "+";
			out << labels[i];
			first = false;
		}
	}
	return out.str();
}

/**
 * Replaces a certain amount of colors in the surface's palette.
 * @param colors Pointer to the set of colors.
 * @param firstcolor Offset of the first color to replace.
 * @param ncolors Amount of colors to replace.
 */
void Map::setPalette(const SDL_Color *colors, int firstcolor, int ncolors)
{
	Surface::setPalette(colors, firstcolor, ncolors);
	for (auto* mds : *_save->getMapDataSets())
	{
		mds->getSurfaceset()->setPalette(colors, firstcolor, ncolors);
	}
	_message->setPalette(colors, firstcolor, ncolors);
	refreshHiddenMovementBackground();
	_message->initText(_game->getMod()->getFont("FONT_BIG"), _game->getMod()->getFont("FONT_SMALL"), _game->getLanguage());
	_message->setText(_game->getLanguage()->getString("STR_HIDDEN_MOVEMENT"), _game->getLanguage()->getString("STR_THINKING"));
}

void Map::refreshHiddenMovementBackground()
{
	_message->setBackground(_game->getMod()->getSurface(_save->getHiddenMovementBackground()));
}

/**
 * Get shade of wall.
 * @param part For what wall do calculations.
 * @param tileFrot Tile of wall.
 * @return Current shade of wall.
 */
int Map::getWallShade(TilePart part, Tile* tileFrot)
{
	int shade;
	if (tileFrot->isDiscovered(O_FLOOR))
	{
		shade = reShade(tileFrot);
	}
	else
	{
		shade = 16;
	}
	if (part)
	{
		if ((tileFrot->isDoor(part) || tileFrot->isUfoDoor(part)) && tileFrot->isDiscovered(part))
		{
			Position offset =
				part == O_NORTHWALL ? Position(1,0,0) :
				part == O_WESTWALL ? Position(0,1,0) :
					throw Exception("Unsupported tile part for wall shade");

			Tile *tileBehind = _save->getTile(tileFrot->getPosition() - offset);

			shade = std::min(reShade(tileFrot), tileBehind ? tileBehind->getShade() + 5 : 16);
		}
	}
	return shade;
}

/**
 * Check two positions if have same XY cords
 */
static bool positionHaveSameXY(Position a, Position b)
{
	return a.x == b.x && a.y == b.y;
}

/**
 * Check two positions if have same XY cords
 */
static bool positionInRangeXY(Position a, Position b, int diff)
{
	return std::abs(a.x - b.x) <= diff && std::abs(a.y - b.y) <= diff;
}

namespace
{

static const int ArrowBobOffsets[8] = {0,1,2,1,0,1,2,1};

static const int ArrowColorsUFO[4]  = { 6,  3, 14, 4 }; // white,    red, blue, green
static const int ArrowColorsTFTD[4] = { 4, 11, 16, 6 }; // white, orange, blue, green

int getArrowBobForFrame(int frame)
{
	return ArrowBobOffsets[frame % 8];
}

int getShadePulseForFrame(int shade, int frame)
{
	if (shade > 7) shade = 7;
	if (shade < 2) shade = 2;
	shade += (ArrowBobOffsets[frame % 8] * 2 - 2);
	return shade;
}

static unsigned hdHashString(const std::string &s)
{
	unsigned h = 2166136261u;
	for (unsigned char c : s)
	{
		h ^= c;
		h *= 16777619u;
	}
	return h ? h : 1u;
}

static unsigned hdNextRandom(unsigned &seed)
{
	// Small deterministic xorshift; presentation only, never gameplay RNG.
	if (!seed) seed = 1u;
	seed ^= seed << 13;
	seed ^= seed >> 17;
	seed ^= seed << 5;
	return seed;
}

static int hdRandomRange(unsigned &seed, int minimum, int maximum)
{
	if (maximum <= minimum) return minimum;
	return minimum + (int)(hdNextRandom(seed) % (unsigned)(maximum - minimum + 1));
}


static std::string hdJoinPath(const std::string &root, const std::string &frame)
{
	if (frame.empty()) return frame;
	if (root.empty() || frame.rfind("Resources/", 0) == 0) return frame;
	if (root.back() == '/') return root + frame;
	return root + "/" + frame;
}

}

/**
 * Draw part of unit graphic that overlap current tile.
 * @param surface
 * @param unitTile
 * @param currTile
 * @param currTileScreenPosition
 * @param shade
 * @param obstacleShade
 * @param topLayer
 */
bool Map::usesFullBodySprite(const BattleUnit *unit) const
{
	return Options::hdGraphics && unit && unit->getArmor() && !unit->isOut()
		&& (!unit->getArmor()->getFullBodySprite().empty() || !unit->getArmor()->getFullBodySpriteRoot().empty());
}

std::string Map::getHdUnitState(const BattleUnit *unit) const
{
	if (!unit) return "idle";
	switch (unit->getStatus())
	{
	case STATUS_WALKING:
	case STATUS_FLYING: return "walk";
	case STATUS_AIMING: return unit->isKneeled() ? "kneel_aim" : "aim";
	case STATUS_COLLAPSING: return "fall";
	case STATUS_PANICKING: return "panic";
	case STATUS_BERSERK: return "berserk";
	default: break;
	}
	return unit->isKneeled() ? "kneel" : "idle";
}

int Map::getHdUnitFrame(const BattleUnit *unit, const std::string &state) const
{
	if (!unit) return 0;
	if (state == "walk") return std::max(0, unit->getWalkingPhase());
	if (state == "fall") return std::max(0, unit->getFallingPhase());
	// Idle/aim/kneel assets may optionally animate using the presentation clock.
	return _animFrame;
}

std::string Map::findHdUnitVariant(const std::string &base, const std::string &state, int direction, int frame) const
{
	if (base.empty()) return std::string();
	std::ostringstream padded;
	padded << std::setw(3) << std::setfill('0') << frame;
	const std::string dir = "dir" + std::to_string(direction);
	const std::vector<std::string> candidates =
	{
		base + "/" + state + "/" + dir + "/frame_" + padded.str() + ".png",
		base + "/" + state + "/" + dir + "/" + padded.str() + ".png",
		base + "/" + state + "/" + dir + "/frame_" + std::to_string(frame) + ".png",
		base + "/" + state + "/" + dir + ".png",
		base + "/" + state + "/frame_" + padded.str() + ".png",
		base + "/" + state + "/" + padded.str() + ".png",
		base + "/" + state + ".png",
		base + "/" + dir + ".png"
	};
	for (const std::string &path : candidates)
	{
		if (const_cast<Map*>(this)->_hdImageCache.exists(path)) return path;
	}
	return std::string();
}

std::string Map::findHdUnitBodyAsset(const BattleUnit *unit) const
{
	if (!unit || !unit->getArmor()) return std::string();
	const Armor *armor = unit->getArmor();
	const std::string &root = armor->getFullBodySpriteRoot();
	if (root.empty()) return std::string();
	const std::string state = getHdUnitState(unit);
	const int dir = unit->getDirection();
	const int frame = getHdUnitFrame(unit, state);
	std::string path = findHdUnitVariant(root + "/body", state, dir, frame);
	if (path.empty()) path = findHdUnitVariant(root, state, dir, frame);
	return path;
}

std::string Map::findHdUnitCombinedAsset(const BattleUnit *unit) const
{
	if (!unit || !unit->getArmor()) return std::string();
	const std::string &root = unit->getArmor()->getFullBodySpriteRoot();
	if (root.empty()) return std::string();
	const BattleItem *left = unit->getLeftHandWeapon();
	const BattleItem *right = unit->getRightHandWeapon();
	const BattleItem *active = unit->getActiveHand(left, right);
	if (!active || !active->getRules()) active = right ? right : left;
	if (!active || !active->getRules()) return std::string();
	const std::string state = getHdUnitState(unit);
	const int dir = unit->getDirection();
	const int frame = getHdUnitFrame(unit, state);
	return findHdUnitVariant(root + "/combined/" + active->getRules()->getType(), state, dir, frame);
}

std::string Map::findHdUnitOverlayAsset(const BattleUnit *unit, const std::string &layer, const std::string &hand, const BattleItem *item) const
{
	if (!unit || !unit->getArmor()) return std::string();
	const std::string &root = unit->getArmor()->getFullBodySpriteRoot();
	if (root.empty()) return std::string();
	const std::string state = getHdUnitState(unit);
	const int dir = unit->getDirection();
	const int frame = getHdUnitFrame(unit, state);
	std::vector<std::string> bases;
	if (item && item->getRules())
	{
		bases.push_back(root + "/overlays/" + layer + "/" + hand + "/" + item->getRules()->getType());
		bases.push_back(root + "/overlays/" + layer + "/" + item->getRules()->getType());
	}
	bases.push_back(root + "/overlays/" + layer + "/" + hand);
	bases.push_back(root + "/overlays/" + layer);
	for (const std::string &base : bases)
	{
		std::string path = findHdUnitVariant(base, state, dir, frame);
		if (!path.empty()) return path;
	}
	return std::string();
}

unsigned Map::nextDrawSequence()
{
	return ++_drawSequence;
}

std::vector<Uint8> Map::snapshotArea(const GraphSubset &area) const
{
	GraphSubset clipped = GraphSubset::intersection(area, GraphSubset(getWidth(), getHeight()));
	std::vector<Uint8> result;
	if (!clipped) return result;
	result.reserve((size_t)clipped.size_x() * clipped.size_y());
	const Uint8 *buffer = getBuffer();
	for (int y = clipped.beg_y; y < clipped.end_y; ++y)
	{
		const Uint8 *row = buffer + y * getPitch();
		result.insert(result.end(), row + clipped.beg_x, row + clipped.end_x);
	}
	return result;
}

void Map::markChangedDrawOrder(const std::vector<Uint8> &before, const GraphSubset &area, unsigned order)
{
	GraphSubset clipped = GraphSubset::intersection(area, GraphSubset(getWidth(), getHeight()));
	if (!clipped || before.empty()) return;
	const Uint8 *buffer = getBuffer();
	size_t k = 0;
	for (int y = clipped.beg_y; y < clipped.end_y; ++y)
	{
		const Uint8 *row = buffer + y * getPitch();
		for (int x = clipped.beg_x; x < clipped.end_x; ++x, ++k)
		{
			if (k < before.size() && row[x] != before[k])
			{
				_drawOrderBuffer[(size_t)y * getWidth() + x] = order;
			}
		}
	}
}

void Map::markSourceDrawOrder(SurfaceRaw<const Uint8> src, int x, int y, bool rightHalfOnly, unsigned order)
{
	if (!src) return;
	const int startX = rightHalfOnly ? src.getWidth() / 2 : 0;
	const Uint8 *buffer = src.getBuffer();
	for (int sy = 0; sy < src.getHeight(); ++sy)
	{
		const int dy = y + sy;
		if (dy < 0 || dy >= getHeight()) continue;
		const Uint8 *row = buffer + sy * src.getPitch();
		for (int sx = startX; sx < src.getWidth(); ++sx)
		{
			const int dx = x + sx;
			if (dx < 0 || dx >= getWidth()) continue;
			if (row[sx]) _drawOrderBuffer[(size_t)dy * getWidth() + dx] = order;
		}
	}
}

void Map::trackedBlitRaw(SurfaceRaw<Uint8> destination, SurfaceRaw<const Uint8> source, int x, int y, int shade, bool rightHalfOnly, int newBaseColor)
{
	const unsigned order = nextDrawSequence();
	Surface::blitRaw(destination, source, x, y, shade, rightHalfOnly, newBaseColor);
	markSourceDrawOrder(source, x, y, rightHalfOnly, order);
}

void Map::trackedBlitNShade(const Surface *source, SurfaceRaw<Uint8> destination, int x, int y, int shade, bool half, int newBaseColor)
{
	if (!source) return;
	GraphSubset area(source->getWidth(), source->getHeight());
	area = area.offset(x, y);
	const std::vector<Uint8> before = snapshotArea(area);
	const unsigned order = nextDrawSequence();
	source->blitNShade(destination, x, y, shade, half, newBaseColor);
	markChangedDrawOrder(before, area, order);
}

void Map::queueHdAsset(const std::string &assetPath, int nativeScale, int offsetX, int offsetY, int x, int y, int shade, bool rightHalfOnly, unsigned order, const GraphSubset *clipMask, const std::string &colorMode)
{
	HdDrawCommand cmd;
	cmd.assetPath = assetPath;
	cmd.nativeScale = nativeScale;
	cmd.colorMode = hdColorModeForAssetPath(assetPath, colorMode);
	cmd.environmentProfile = hdEnvironmentProfileForAssetPath(assetPath);
	cmd.offsetX = offsetX;
	cmd.offsetY = offsetY;
	cmd.x = x;
	cmd.y = y;
	cmd.shade = shade;
	cmd.rightHalfOnly = rightHalfOnly;
	cmd.drawOrder = order;
	if (clipMask)
	{
		cmd.clipMask = *clipMask;
		cmd.hasClipMask = true;
	}
	_hdDrawCommands.push_back(cmd);
}

std::string Map::findHdTerrainAsset(const Tile *tile, TilePart part) const
{
	if (!Options::hdGraphics || !tile) return std::string();
	MapData *data = tile->getMapData(part);
	if (!data || !data->getDataset()) return std::string();
	// Keep automatically discovered HD terrain animations on the exact Legacy/MCD
	// animation frame. This preserves the historical 100 ms cadence and frame order;
	// HD resolution must not alter gameplay-side terrain animation timing.
	const int visualFrame = tile->getCurrentFrame(part);
	const int sprite = data->getSprite(visualFrame);
	if (sprite < 0) return std::string();

	const char *partName = "OBJECT";
	switch (part)
	{
	case O_FLOOR: partName = "FLOOR"; break;
	case O_WESTWALL: partName = "WESTWALL"; break;
	case O_NORTHWALL: partName = "NORTHWALL"; break;
	case O_OBJECT: partName = "OBJECT"; break;
	default: break;
	}

	std::ostringstream numberStream;
	numberStream << std::setw(3) << std::setfill('0') << sprite;
	const std::string number = numberStream.str();
	const std::string dataset = data->getDataset()->getName();

	int mapDataId = -1, mapDataSetId = -1;
	tile->getMapData(&mapDataId, &mapDataSetId, part);
	const int currentSprite = tile->getCurrentSpriteIndex(part);

	auto tryRoot = [&](const std::string &root) -> std::string
	{
		// Most-specific convention: two MCD records may reuse one PCK frame.
		if (mapDataId >= 0)
		{
			std::ostringstream mcd;
			mcd << "MCD_" << std::setw(3) << std::setfill('0') << mapDataId << "_F" << visualFrame << ".png";
			const std::string semantic = root + partName + "/" + mcd.str();
			if (const_cast<Map*>(this)->_hdImageCache.exists(semantic)) return semantic;
		}

		const std::string specific = root + partName + "/" + number + ".png";
		if (const_cast<Map*>(this)->_hdImageCache.exists(specific)) return specific;
		const std::string flat = root + number + ".png";
		if (const_cast<Map*>(this)->_hdImageCache.exists(flat)) return flat;

		// A higher-priority authored-HD namespace may intentionally contain only
		// part of an animation. Keep its gameplay-synchronised frame before
		// falling through to the LegacyIndexed reference layer.
		if (currentSprite >= 0 && currentSprite != sprite)
		{
			std::ostringstream fallbackNumber;
			fallbackNumber << std::setw(3) << std::setfill('0') << currentSprite;
			const std::string fallbackSpecific = root + partName + "/" + fallbackNumber.str() + ".png";
			if (const_cast<Map*>(this)->_hdImageCache.exists(fallbackSpecific)) return fallbackSpecific;
			const std::string fallbackFlat = root + fallbackNumber.str() + ".png";
			if (const_cast<Map*>(this)->_hdImageCache.exists(fallbackFlat)) return fallbackFlat;
		}
		return std::string();
	};

	// Explicit fixed diagnostics win while enabled. Ordinary authored HD is the
	// normal path. LegacyIndexed is a semantic fallback/reference layer and no
	// longer depends on whether the PNG happens to use a palette internally.
	const std::string fixedRoot = "Resources/TFTD_HD/Fixed/Terrain/" + dataset + "/";
	if (const std::string asset = tryRoot(fixedRoot); !asset.empty()) return asset;

	const std::string authoredRoot = "Resources/TFTD_HD/Terrain/" + dataset + "/";
	if (const std::string asset = tryRoot(authoredRoot); !asset.empty()) return asset;

	const std::string legacyRoot = "Resources/TFTD_HD/LegacyIndexed/Terrain/" + dataset + "/";
	if (const std::string asset = tryRoot(legacyRoot); !asset.empty()) return asset;

	return std::string();
}


std::string Map::findHdSurfaceSetAsset(const std::string &setName, int frame) const
{
	if (!Options::hdGraphics || frame < 0) return std::string();
	std::ostringstream number;
	number << std::setw(3) << std::setfill('0') << frame;

	auto tryRoot = [&](const std::string &root) -> std::string
	{
		const std::string padded = root + number.str() + ".png";
		if (const_cast<Map*>(this)->_hdImageCache.exists(padded)) return padded;
		const std::string raw = root + std::to_string(frame) + ".png";
		if (const_cast<Map*>(this)->_hdImageCache.exists(raw)) return raw;
		return std::string();
	};

	const std::string fixedRoot = "Resources/TFTD_HD/Fixed/SurfaceSets/" + setName + "/";
	if (const std::string asset = tryRoot(fixedRoot); !asset.empty()) return asset;

	const std::string authoredRoot = "Resources/TFTD_HD/SurfaceSets/" + setName + "/";
	if (const std::string asset = tryRoot(authoredRoot); !asset.empty()) return asset;

	const std::string legacyRoot = "Resources/TFTD_HD/LegacyIndexed/SurfaceSets/" + setName + "/";
	if (const std::string asset = tryRoot(legacyRoot); !asset.empty()) return asset;

	return std::string();
}


const HdVisualRule *Map::findHdTerrainVisualRule(const Tile *tile, TilePart part) const
{
	if (!Options::hdGraphics || !tile) return nullptr;
	MapData *data = tile->getMapData(part);
	if (!data || !data->getDataset()) return nullptr;
	int mapDataId = -1, mapDataSetId = -1;
	tile->getMapData(&mapDataId, &mapDataSetId, part);
	const int sprite = tile->getCurrentSpriteIndex(part);
	const char *partName = "OBJECT";
	switch (part)
	{
	case O_FLOOR: partName = "FLOOR"; break;
	case O_WESTWALL: partName = "WESTWALL"; break;
	case O_NORTHWALL: partName = "NORTHWALL"; break;
	case O_OBJECT: partName = "OBJECT"; break;
	default: break;
	}

	const HdVisualRule *best = nullptr;
	int bestScore = -1;
	for (const auto &pair : _game->getMod()->getHdVisuals())
	{
		const HdVisualRule &rule = pair.second;
		if (rule.target != "terrain" || !rule.depthMatches(_save->getDepth())) continue;
		if (!rule.dataset.empty() && rule.dataset != data->getDataset()->getName()) continue;
		if (!rule.part.empty() && rule.part != partName) continue;
		if (rule.mcd >= 0 && rule.mcd != mapDataId) continue;
		if (rule.sprite >= 0 && rule.sprite != sprite) continue;
		int score = 0;
		if (!rule.dataset.empty()) score += 8;
		if (!rule.part.empty()) score += 4;
		if (rule.mcd >= 0) score += 16;
		if (rule.sprite >= 0) score += 2;
		if (rule.depthMin >= 0 || rule.depthMax >= 0) score += 1;
		if (score > bestScore)
		{
			best = &rule;
			bestScore = score;
		}
	}
	return best;
}

const HdVisualRule *Map::findHdSurfaceSetVisualRule(const std::string &setName, int frame) const
{
	if (!Options::hdGraphics) return nullptr;
	const HdVisualRule *best = nullptr;
	int bestScore = -1;
	for (const auto &pair : _game->getMod()->getHdVisuals())
	{
		const HdVisualRule &rule = pair.second;
		if (rule.target != "surfaceSet" || !rule.depthMatches(_save->getDepth())) continue;
		if (!rule.setName.empty() && rule.setName != setName) continue;
		if (rule.setFrame >= 0 && rule.setFrame != frame) continue;
		int score = 0;
		if (!rule.setName.empty()) score += 8;
		if (rule.setFrame >= 0) score += 16;
		if (rule.depthMin >= 0 || rule.depthMax >= 0) score += 1;
		if (score > bestScore)
		{
			best = &rule;
			bestScore = score;
		}
	}
	return best;
}

bool Map::queueHdVisualRule(const HdVisualRule &rule, const std::string &instanceKey, int x, int y, int shade, bool rightHalfOnly, unsigned order, const GraphSubset *clipMask, long long epochTimeMs, int epochTurn)
{
	bool queuedAny = false;
	for (const HdVisualLayer &layer : rule.layers)
	{
		if (layer.states.empty()) continue;
		const std::string memoryKey = rule.id + "|" + layer.id + "|" + instanceKey;
		auto &memory = _save->getHdVisualMemory(memoryKey);

		auto initializeState = [&](const std::string &requested, bool useEpoch, bool applyRandomPhase)
		{
			auto it = layer.states.find(requested);
			if (it == layer.states.end()) it = layer.states.begin();
			memory.state = it->first;
			memory.startAnimFrame = _animFrame; // retained for old-save diagnostics only
			memory.startTimeMs = (useEpoch && epochTimeMs >= 0) ? (unsigned long long)epochTimeMs : _save->getHdVisualTimeMs();
			memory.startTurn = (useEpoch && epochTurn >= 0) ? epochTurn : _save->getTurn();
			memory.seed = memory.seed ? memory.seed : hdHashString(memoryKey);
			const int variance = layer.speedVariancePct;
			memory.speedPct = variance ? hdRandomRange(memory.seed, 100 - variance, 100 + variance) : 100;
			const HdVisualState &state = it->second;
			memory.repeatsRemaining = hdRandomRange(memory.seed, state.minRepeats, state.maxRepeats);
			if (state.maxDurationTurns > 0)
				memory.durationValue = hdRandomRange(memory.seed, state.minDurationTurns, state.maxDurationTurns);
			else if (state.maxDurationMs > 0)
				memory.durationValue = hdRandomRange(memory.seed, state.minDurationMs, state.maxDurationMs);
			else
				memory.durationValue = 0;

			if (applyRandomPhase && !state.frames.empty())
			{
				if (state.frameTurns > 0)
				{
					const int phase = hdRandomRange(memory.seed, 0, std::max(0, (int)state.frames.size() * state.frameTurns - 1));
					memory.startTurn = std::max(0, memory.startTurn - phase);
				}
				else
				{
					const int cycleMs = std::max(1, (int)state.frames.size() * std::max(1, state.frameMs));
					const unsigned phaseMs = (unsigned)hdRandomRange(memory.seed, 0, cycleMs - 1);
					memory.startTimeMs = memory.startTimeMs > phaseMs ? memory.startTimeMs - phaseMs : 0;
				}
			}
		};

		if (memory.state.empty() || layer.states.find(memory.state) == layer.states.end())
		{
			initializeState(layer.initialState, true, layer.randomPhase);
		}

		// Advance the presentation-only state machine when its configured
		// duration/repeat count has elapsed. It never touches gameplay RNG.
		for (int guard = 0; guard < 16; ++guard)
		{
			auto stateIt = layer.states.find(memory.state);
			if (stateIt == layer.states.end())
			{
				initializeState(layer.initialState, false, false);
				stateIt = layer.states.find(memory.state);
				if (stateIt == layer.states.end()) break;
			}
			const HdVisualState &state = stateIt->second;
			const int elapsedTurns = std::max(0, _save->getTurn() - memory.startTurn);
			const unsigned long long nowMs = _save->getHdVisualTimeMs();
			const unsigned long long rawElapsedMs = nowMs >= memory.startTimeMs ? nowMs - memory.startTimeMs : 0;
			const unsigned long long elapsedMs = rawElapsedMs * (unsigned)std::max(1, memory.speedPct) / 100u;
			bool finished = false;
			if (memory.durationValue > 0)
			{
				if (state.maxDurationTurns > 0) finished = elapsedTurns >= memory.durationValue;
				else if (state.maxDurationMs > 0) finished = elapsedMs >= (unsigned long long)memory.durationValue;
			}
			else if (!state.frames.empty())
			{
				if (state.frameTurns > 0)
				{
					const long long cycleTurns = (long long)state.frames.size() * state.frameTurns;
					finished = cycleTurns > 0 && elapsedTurns >= cycleTurns * memory.repeatsRemaining;
				}
				else
				{
					const long long cycleMs = (long long)state.frames.size() * state.frameMs;
					finished = cycleMs > 0 && elapsedMs >= (unsigned long long)(cycleMs * memory.repeatsRemaining);
				}
			}

			if (!finished || state.next.empty()) break;
			int totalWeight = 0;
			for (const auto &next : state.next) totalWeight += std::max(0, next.weight);
			if (totalWeight <= 0) break;
			int pick = hdRandomRange(memory.seed, 1, totalWeight);
			std::string nextState = state.next.back().state;
			for (const auto &next : state.next)
			{
				pick -= std::max(0, next.weight);
				if (pick <= 0) { nextState = next.state; break; }
			}
			initializeState(nextState, false, false);
		}

		auto stateIt = layer.states.find(memory.state);
		if (stateIt == layer.states.end() || stateIt->second.frames.empty()) continue;
		const HdVisualState &state = stateIt->second;
		const int elapsedTurns = std::max(0, _save->getTurn() - memory.startTurn);
		const unsigned long long nowMs = _save->getHdVisualTimeMs();
		const unsigned long long rawElapsedMs = nowMs >= memory.startTimeMs ? nowMs - memory.startTimeMs : 0;
		const unsigned long long elapsedMs = rawElapsedMs * (unsigned)std::max(1, memory.speedPct) / 100u;
		long long frameOrdinal = 0;
		if (state.frameTurns > 0) frameOrdinal = elapsedTurns / std::max(1, state.frameTurns);
		else frameOrdinal = elapsedMs / std::max(1, state.frameMs);
		int frameIndex;
		if (state.holdLastFrame && state.next.empty())
			frameIndex = (int)std::min<long long>((long long)state.frames.size() - 1, frameOrdinal);
		else
			frameIndex = (int)(frameOrdinal % state.frames.size());

		const std::string path = hdJoinPath(layer.root, state.frames[frameIndex]);
		if (path.empty() || !_hdImageCache.exists(path)) continue;
		queueHdAsset(path, layer.nativeScale, layer.offsetX, layer.offsetY, x, y, shade, rightHalfOnly, order, clipMask, layer.colorMode);
		queuedAny = true;
	}
	return queuedAny && rule.replaceLegacy;
}

bool Map::queueHdTerrainOrConvention(const Tile *tile, TilePart part, int x, int y, int shade, bool rightHalfOnly, unsigned order, const GraphSubset *clipMask)
{
	if (!Options::hdGraphics || !tile) return false;
	if (const HdVisualRule *rule = findHdTerrainVisualRule(tile, part))
	{
		int mapDataId = -1, mapDataSetId = -1;
		tile->getMapData(&mapDataId, &mapDataSetId, part);
		const Position p = tile->getPosition();
		std::ostringstream key;
		key << "tile:" << p.x << ',' << p.y << ',' << p.z << ":part:" << (int)part << ":mcd:" << mapDataId;
		SavedBattleGame::HdVisualEpoch epoch;
		const bool hasEpoch = _save->getHdVisualTileEpoch(p, part, mapDataId, epoch);
		const bool replaced = queueHdVisualRule(*rule, key.str(), x, y, shade, rightHalfOnly, order, clipMask,
			hasEpoch ? (long long)epoch.timeMs : -1, hasEpoch ? epoch.turn : -1);
		if (replaced) return true;
		// replaceLegacy:false intentionally falls through so the legacy/base
		// graphic remains and the HD layers behave as overlays.
		if (!rule->replaceLegacy) return false;
	}
	const std::string hd = findHdTerrainAsset(tile, part);
	if (!hd.empty())
	{
		queueHdAsset(hd, 0, 0, 0, x, y, shade, rightHalfOnly, order, clipMask);

		// The gameplay dataset remains untouched.  Only the just-queued HD draw
		// command may borrow another material family's environment response.
		// Example: DEBRIS/057..066 can remain DEBRIS OBJECT records while using
		// the SAND palette-block profile declared by the mod sidecar.
		MapData *data = tile->getMapData(part);
		if (data && data->getDataset() && !_hdDrawCommands.empty())
		{
			const int visualFrame = tile->getCurrentFrame(part);
			const int sprite = data->getSprite(visualFrame);
			const std::string materialProfile = hdMaterialProfileForTerrain(data->getDataset()->getName(), sprite);
			if (!materialProfile.empty())
				_hdDrawCommands.back().environmentProfile = hdEnvironmentProfileForMaterialProfile(materialProfile);
		}
		return true;
	}
	return false;
}

bool Map::queueHdSurfaceSetOrConvention(const std::string &setName, int frame, const std::string &instanceKey, int x, int y, int shade, bool rightHalfOnly, unsigned order, const GraphSubset *clipMask)
{
	if (const HdVisualRule *rule = findHdSurfaceSetVisualRule(setName, frame))
	{
		const bool replaced = queueHdVisualRule(*rule, instanceKey, x, y, shade, rightHalfOnly, order, clipMask);
		if (replaced) return true;
		if (!rule->replaceLegacy) return false;
	}
	const std::string hd = findHdSurfaceSetAsset(setName, frame);
	if (!hd.empty())
	{
		queueHdAsset(hd, HdRenderSpace::AutoNativeScale, 0, 0, x, y, shade, rightHalfOnly, order, clipMask);
		return true;
	}
	return false;
}

void Map::drawHdSurfaceSetOrLegacy(SurfaceRaw<Uint8> destination, const std::string &setName, int frame, SurfaceRaw<const Uint8> legacy, int x, int y, int shade, bool rightHalfOnly, int newBaseColor)
{
	const unsigned order = nextDrawSequence();
	std::ostringstream key;
	key << "set:" << setName << ":xy:" << x << ',' << y;
	if (!queueHdSurfaceSetOrConvention(setName, frame, key.str(), x, y, shade, rightHalfOnly, order))
	{
		Surface::blitRaw(destination, legacy, x, y, shade, rightHalfOnly, newBaseColor);
		markSourceDrawOrder(legacy, x, y, rightHalfOnly, order);
	}
}

void Map::blitHdOverlays(SDL_Surface *destination)
{
	if (!Options::hdGraphics || _hdDrawCommands.empty() || !destination || destination->format->BitsPerPixel != 32) return;
	const int mapScale = std::max(1, _spriteWidth / 32);
	SDL_Rect mapClip = { (Sint16)getX(), (Sint16)getY(), (Uint16)getWidth(), (Uint16)std::min(getHeight(), _visibleMapHeight) };
	const SDL_Color *activePalette = getPalette();
	const SDL_Color *neutralPalette = hdNeutralBattlescapePalette(_game, activePalette);
	hdBuildEnvironmentProfiles(neutralPalette, activePalette);

	// Commands are already queued in original Battlescape draw order.
	for (const auto &cmd : _hdDrawCommands)
	{
		HdImage *image = _hdImageCache.get(cmd.assetPath);
		if (!image || !image->loaded || !image->width || !image->height) continue;
		const int nativeScale = HdRenderSpace::resolveNativeScale(cmd.nativeScale, image->width, image->height);
		const int drawW = std::max(1, (int)image->width * mapScale / nativeScale);
		const int drawH = std::max(1, (int)image->height * mapScale / nativeScale);
		const int scaledOffsetX = cmd.offsetX * mapScale / nativeScale;
		const int scaledOffsetY = cmd.offsetY * mapScale / nativeScale;
		const int baseMapX = cmd.x + scaledOffsetX;
		const int baseMapY = cmd.y + scaledOffsetY;
		const int startX = cmd.rightHalfOnly ? drawW / 2 : 0;
		const double legacyLight = (16.0 - std::max(0, std::min(16, cmd.shade))) / 16.0;
		// A Legacy 16-step palette ramp shifted toward black has an aggregate
		// luminance close to legacyLight^2.  Use 2.2 for a deliberately small
		// extra darkness bias in HD while keeping shade 0 exactly unchanged.
		const double lightFactor = std::pow(legacyLight, 2.2);

		if (SDL_MUSTLOCK(destination) && SDL_LockSurface(destination) != 0) continue;
		for (int yy = 0; yy < drawH; ++yy)
		{
			const int mapY = baseMapY + yy;
			const int dy = getY() + mapY;
			if (mapY < 0 || mapY >= getHeight() || dy < mapClip.y || dy >= mapClip.y + mapClip.h || dy < 0 || dy >= destination->h) continue;
			const unsigned sy = std::min(image->height - 1, (unsigned)((long long)yy * image->height / drawH));
			for (int xx = startX; xx < drawW; ++xx)
			{
				const int mapX = baseMapX + xx;
				const int dx = getX() + mapX;
				if (mapX < 0 || mapX >= getWidth() || dx < mapClip.x || dx >= mapClip.x + mapClip.w || dx < 0 || dx >= destination->w) continue;
				if (cmd.hasClipMask && (mapX < cmd.clipMask.beg_x || mapX >= cmd.clipMask.end_x || mapY < cmd.clipMask.beg_y || mapY >= cmd.clipMask.end_y)) continue;
				if ((size_t)mapY * getWidth() + mapX < _drawOrderBuffer.size() && _drawOrderBuffer[(size_t)mapY * getWidth() + mapX] > cmd.drawOrder) continue;

				const unsigned sx = std::min(image->width - 1, (unsigned)((long long)xx * image->width / drawW));
				const size_t pixelIndex = (size_t)sy * image->width + sx;
				const HdColorMode colorMode = resolveHdColorMode(cmd.colorMode, image->paletteIndexed8);
				Uint8 sa = 255, sr = 0, sg = 0, sb = 0;
				if (colorMode == HdColorMode::IndexedLegacy && pixelIndex < image->indices.size())
				{
					const Uint8 srcIndex = image->indices[pixelIndex];
					if (!srcIndex) continue;
					const Uint8 shaded = (Uint8)(srcIndex + std::max(0, std::min(16, cmd.shade)));
					const Uint8 finalIndex = ((shaded ^ srcIndex) & 0xF0) ? 0x0F : shaded;
					const SDL_Color &pc = activePalette[finalIndex];
					sr = pc.r; sg = pc.g; sb = pc.b;
				}
				else
				{
					const size_t si = pixelIndex * 4u;
					sa = image->rgba[si + 3];
					if (!sa) continue;
					sr = image->rgba[si + 0];
					sg = image->rgba[si + 1];
					sb = image->rgba[si + 2];
					// Match Legacy ordering: choose/grade the active environment colour
					// first, then apply local shade. This guarantees shade 16 stays black
					// instead of being lifted back toward blue by the environment LUT.
					if (colorMode == HdColorMode::Environment && Options::hdEnvironmentGrade) hdEnvironmentTransformFor(cmd.environmentProfile).apply(sr, sg, sb);
					sr = (Uint8)std::lround((double)sr * lightFactor);
					sg = (Uint8)std::lround((double)sg * lightFactor);
					sb = (Uint8)std::lround((double)sb * lightFactor);
				}
				Uint8 *pixelAddress = (Uint8*)destination->pixels + dy * destination->pitch + dx * 4;
				Uint32 dstPixel;
				memcpy(&dstPixel, pixelAddress, sizeof(dstPixel));
				Uint8 dr, dg, db, da;
				SDL_GetRGBA(dstPixel, destination->format, &dr, &dg, &db, &da);
				const unsigned inv = 255 - sa;
				const Uint8 rr = (Uint8)((sr * sa + dr * inv + 127) / 255);
				const Uint8 rg = (Uint8)((sg * sa + dg * inv + 127) / 255);
				const Uint8 rb = (Uint8)((sb * sa + db * inv + 127) / 255);
				const Uint8 ra = (Uint8)std::min(255u, (unsigned)sa + ((unsigned)da * inv + 127) / 255);
				const Uint32 out = SDL_MapRGBA(destination->format, rr, rg, rb, ra);
				memcpy(pixelAddress, &out, sizeof(out));
			}
		}
		if (SDL_MUSTLOCK(destination)) SDL_UnlockSurface(destination);
	}
}


bool Map::blitHdOverlaysGpu(const Screen *screen, int physicalWidth, int physicalHeight)
{
	HdGpuBackend &gpu = HdGpuBackend::instance();
	if (!Options::hdGraphics || !screen || !gpu.frameActive() || physicalWidth <= 0 || physicalHeight <= 0) return false;

	const uint64_t perfStart = hdPerfNowUs();
	auto &perf = getHdPerfStats().current;
	perf.mapSeedUs = 0;
	perf.mapCompositeUs = 0;
	perf.mapCacheBlitUs = 0;
	perf.mapCacheHit = false;
	_hdPhysicalLastCacheHit = false;
	_hdPhysicalPixelsTested = 0;
	_hdPhysicalPixelsWritten = 0;

	const double sx = screen->getRenderScaleX();
	const double sy = screen->getRenderScaleY();
	const int logicalMapX = getDisplayX();
	const int logicalMapY = getDisplayY();
	const int logicalMapW = getWidth() * std::max(1, getDisplayScale());
	const int logicalVisibleH = std::min(getHeight(), _visibleMapHeight) * std::max(1, getDisplayScale());
	int px0 = screen->logicalToPhysicalX(logicalMapX);
	int py0 = screen->logicalToPhysicalY(logicalMapY);
	int px1 = screen->logicalToPhysicalX(logicalMapX + logicalMapW);
	int py1 = screen->logicalToPhysicalY(logicalMapY + logicalVisibleH);
	px0 = std::max(0, std::min(physicalWidth, px0));
	py0 = std::max(0, std::min(physicalHeight, py0));
	px1 = std::max(px0, std::min(physicalWidth, px1));
	py1 = std::max(py0, std::min(physicalHeight, py1));
	if (px1 <= px0 || py1 <= py0)
	{
		perf.mapGpuSubmitUs = hdPerfNowUs() - perfStart;
		perf.mapPhysicalUs = perf.mapGpuSubmitUs;
		return true;
	}

	// The final physical base already contains P4's correctly scaled Legacy map.
	// GPU work therefore starts exactly where P4's expensive RGBA loop started:
	// OXCE's draw-order buffer is uploaded once, then each native x1/x4/x8/x16
	// image is alpha-composited directly into the D3D11 backbuffer.
	const SDL_Color *activePalette = getPalette();
	const SDL_Color *neutralPalette = hdNeutralBattlescapePalette(_game, activePalette);
	if (!_drawOrderBuffer.empty() && !gpu.beginMap(_drawOrderBuffer.data(), getWidth(), getHeight(), activePalette, neutralPalette, px0, py0, px1, py1))
		return false;

	const int mapScale = std::max(1, _spriteWidth / 32);
	const double renderToPhysicalX = std::max(0.0001, sx / (double)HdRenderSpace::Scale);
	const double renderToPhysicalY = std::max(0.0001, sy / (double)HdRenderSpace::Scale);

	for (const auto &cmd : _hdDrawCommands)
	{
		HdImage *image = _hdImageCache.get(cmd.assetPath);
		if (!image || !image->loaded || !image->width || !image->height || !image->hasVisiblePixels) continue;

		const int nativeScale = HdRenderSpace::resolveNativeScale(cmd.nativeScale, image->width, image->height);
		const double renderDrawW = HdRenderSpace::assetPixelsToRender((double)image->width * mapScale, nativeScale);
		const double renderDrawH = HdRenderSpace::assetPixelsToRender((double)image->height * mapScale, nativeScale);
		const double renderOffsetX = HdRenderSpace::assetPixelsToRender((double)cmd.offsetX * mapScale, nativeScale);
		const double renderOffsetY = HdRenderSpace::assetPixelsToRender((double)cmd.offsetY * mapScale, nativeScale);
		const double baseRenderX = HdRenderSpace::legacyToRender((double)cmd.x) + renderOffsetX;
		const double baseRenderY = HdRenderSpace::legacyToRender((double)cmd.y) + renderOffsetY;
		const int globalX0 = screen->logicalToPhysicalX(logicalMapX + HdRenderSpace::renderToLegacy(baseRenderX));
		const int globalY0 = screen->logicalToPhysicalY(logicalMapY + HdRenderSpace::renderToLegacy(baseRenderY));
		const int globalX1 = screen->logicalToPhysicalX(logicalMapX + HdRenderSpace::renderToLegacy(baseRenderX + renderDrawW));
		const int globalY1 = screen->logicalToPhysicalY(logicalMapY + HdRenderSpace::renderToLegacy(baseRenderY + renderDrawH));
		const int drawW = std::max(1, globalX1 - globalX0);
		const int drawH = std::max(1, globalY1 - globalY0);
		if (globalX1 <= px0 || globalY1 <= py0 || globalX0 >= px1 || globalY0 >= py1) continue;

		HdGpuMapSprite sprite;
		sprite.assetKey = cmd.assetPath.c_str();
		sprite.rgba = image->rgba.data();
		sprite.indices = image->indices.empty() ? nullptr : image->indices.data();
		sprite.colorMode = resolveHdColorMode(cmd.colorMode, image->paletteIndexed8);
		sprite.environmentProfile = cmd.environmentProfile;
		// P10A A/B test switch: authored Environment assets use the ordinary RGBA
		// shader when grading is disabled. IndexedLegacy and explicit Fixed semantics
		// remain untouched.
		if (sprite.colorMode == HdColorMode::Environment && !Options::hdEnvironmentGrade)
			sprite.colorMode = HdColorMode::Fixed;
		sprite.legacyIndexed = sprite.colorMode == HdColorMode::IndexedLegacy;
		sprite.imageWidth = image->width;
		sprite.imageHeight = image->height;
		sprite.destX = globalX0;
		sprite.destY = globalY0;
		sprite.destW = drawW;
		sprite.destH = drawH;
		sprite.baseRenderX = baseRenderX;
		sprite.baseRenderY = baseRenderY;
		sprite.renderToPhysicalX = renderToPhysicalX;
		sprite.renderToPhysicalY = renderToPhysicalY;
		sprite.drawOrder = cmd.drawOrder;
		sprite.shade = cmd.shade;
		sprite.rightHalfOnly = cmd.rightHalfOnly;
		sprite.hasClipMask = cmd.hasClipMask;
		if (cmd.hasClipMask)
		{
			sprite.clipBegX = cmd.clipMask.beg_x;
			sprite.clipBegY = cmd.clipMask.beg_y;
			sprite.clipEndX = cmd.clipMask.end_x;
			sprite.clipEndY = cmd.clipMask.end_y;
		}
		if (!gpu.drawMapSprite(sprite))
		{
			gpu.endMap();
			return false;
		}
	}
	gpu.endMap();

	perf.mapCommands = (unsigned)_hdDrawCommands.size();
	perf.mapGpuSubmitUs = hdPerfNowUs() - perfStart;
	perf.mapPhysicalUs = perf.mapGpuSubmitUs;
	_hdPhysicalLastMs = (Uint32)(perf.mapGpuSubmitUs / 1000ULL);
	return true;
}


void Map::blitHdOverlaysPhysical(SDL_Surface *destination, const Screen *screen)
{
	_hdPhysicalPixelsTested = 0;
	_hdPhysicalPixelsWritten = 0;
	_hdPhysicalLastCacheHit = false;
	const Uint32 started = SDL_GetTicks();
	const uint64_t perfPhysicalStart = hdPerfNowUs();
	auto &perf = getHdPerfStats().current;
	if (!Options::hdGraphics || !destination || !screen || destination->format->BitsPerPixel != 32)
	{
		_hdPhysicalLastMs = SDL_GetTicks() - started;
		perf.mapPhysicalUs = hdPerfNowUs() - perfPhysicalStart;
		return;
	}

	// RC12 P5: the D3D11 frame is already seeded from the exact P4 physical
	// framebuffer. Draw HD Map commands directly on the GPU; the software cache
	// below remains the complete safety/reference fallback.
	if (HdGpuBackend::instance().frameActive())
	{
		if (blitHdOverlaysGpu(screen, destination->w, destination->h))
		{
			perf.mapPhysicalUs = hdPerfNowUs() - perfPhysicalStart;
			return;
		}
		HdGpuBackend::instance().abortFrame();
	}

	const double sx = screen->getRenderScaleX();
	const double sy = screen->getRenderScaleY();
	const int logicalMapX = getDisplayX();
	const int logicalMapY = getDisplayY();
	const int logicalMapW = getWidth() * std::max(1, getDisplayScale());
	const int logicalVisibleH = std::min(getHeight(), _visibleMapHeight) * std::max(1, getDisplayScale());
	int px0 = screen->logicalToPhysicalX(logicalMapX);
	int py0 = screen->logicalToPhysicalY(logicalMapY);
	int px1 = screen->logicalToPhysicalX(logicalMapX + logicalMapW);
	int py1 = screen->logicalToPhysicalY(logicalMapY + logicalVisibleH);
	px0 = std::max(0, std::min(destination->w, px0));
	py0 = std::max(0, std::min(destination->h, py0));
	px1 = std::max(px0, std::min(destination->w, px1));
	py1 = std::max(py0, std::min(destination->h, py1));
	const int cacheW = px1 - px0;
	const int cacheH = py1 - py0;
	if (cacheW <= 0 || cacheH <= 0)
	{
		_hdPhysicalLastMs = SDL_GetTicks() - started;
		perf.mapPhysicalUs = hdPerfNowUs() - perfPhysicalStart;
		return;
	}

	const bool geometryChanged = _hdPhysicalFrameCache.empty() || _hdPhysicalCacheW != cacheW || _hdPhysicalCacheH != cacheH ||
		_hdPhysicalCacheX != px0 || _hdPhysicalCacheY != py0 ||
		std::fabs(_hdPhysicalCacheScaleX - sx) > 0.0001 || std::fabs(_hdPhysicalCacheScaleY - sy) > 0.0001;
	if (geometryChanged)
	{
		_hdPhysicalFrameCache.clear();
		_hdPhysicalCurrentCacheIndex = -1;
		_hdPhysicalUseCounter = 0;
		_hdPhysicalCacheX = px0;
		_hdPhysicalCacheY = py0;
		_hdPhysicalCacheW = cacheW;
		_hdPhysicalCacheH = cacheH;
		_hdPhysicalCacheScaleX = sx;
		_hdPhysicalCacheScaleY = sy;
		_hdPhysicalCachedRevision = ~0ULL;

		// TEST8-C: deliberately trade RAM for CPU.  Keep a history of complete
		// physical Map frames so the repeating 8-frame legacy animation cycle can
		// reuse exact previous compositions instead of rebuilding ~1000 RGBA
		// commands every 100 ms.  The budget is capped at 256 MiB and entries are
		// allocated lazily.
		const unsigned long long bytesPerEntry = (unsigned long long)cacheW * (unsigned long long)cacheH * 4ULL;
		const unsigned long long budget = 256ULL * 1024ULL * 1024ULL;
		_hdPhysicalCacheMaxEntries = bytesPerEntry ? (int)(budget / bytesPerEntry) : 8;
		_hdPhysicalCacheMaxEntries = std::max(8, std::min(64, _hdPhysicalCacheMaxEntries));
	}

	// Same Map revision: the current cached physical frame remains exact.
	if (_hdPhysicalCachedRevision == _hdPhysicalRevision &&
		_hdPhysicalCurrentCacheIndex >= 0 && _hdPhysicalCurrentCacheIndex < (int)_hdPhysicalFrameCache.size() &&
		_hdPhysicalFrameCache[(size_t)_hdPhysicalCurrentCacheIndex].valid)
	{
		auto &entry = _hdPhysicalFrameCache[(size_t)_hdPhysicalCurrentCacheIndex];
		entry.lastUse = ++_hdPhysicalUseCounter;
		const uint64_t perfCacheBlitStart = hdPerfNowUs();
		SDL_Rect dst = { (Sint16)px0, (Sint16)py0, 0, 0 };
		SDL_BlitSurface(entry.surface.get(), nullptr, destination, &dst);
		perf.mapCacheBlitUs = hdPerfNowUs() - perfCacheBlitStart;
		_hdPhysicalLastCacheHit = true;
		perf.mapCacheHit = true;
		perf.mapCommands = (unsigned)_hdDrawCommands.size();
		perf.mapPixelsTested = 0;
		perf.mapPixelsWritten = 0;
		_hdPhysicalLastMs = SDL_GetTicks() - started;
		perf.mapPhysicalUs = hdPerfNowUs() - perfPhysicalStart;
		return;
	}

	// TEST8-C scene signature.  It describes the exact logical Map pixels,
	// recorded draw-order mask and queued HD commands.  If an animation later
	// returns to an already-seen visual state, the old complete physical frame
	// is safe to reuse byte-for-byte.  Gameplay state is not inferred or skipped.
	uint64_t sceneHash = 1469598103934665603ULL;
	auto hashBytes = [&sceneHash](const void *ptr, size_t length)
	{
		const Uint8 *bytes = static_cast<const Uint8*>(ptr);
		for (size_t i = 0; i < length; ++i)
		{
			sceneHash ^= (uint64_t)bytes[i];
			sceneHash *= 1099511628211ULL;
		}
	};
	auto hashInt = [&hashBytes](const int &value) { hashBytes(&value, sizeof(value)); };
	auto hashUnsigned = [&hashBytes](const unsigned &value) { hashBytes(&value, sizeof(value)); };

	SDL_Surface *logicalSurface = getSurface();
	if (logicalSurface && logicalSurface->pixels)
	{
		const int rowBytes = logicalSurface->w * logicalSurface->format->BytesPerPixel;
		for (int y = 0; y < logicalSurface->h; ++y)
		{
			const Uint8 *row = static_cast<const Uint8*>(logicalSurface->pixels) + y * logicalSurface->pitch;
			hashBytes(row, (size_t)rowBytes);
		}
	}
	if (!_drawOrderBuffer.empty()) hashBytes(_drawOrderBuffer.data(), _drawOrderBuffer.size() * sizeof(_drawOrderBuffer[0]));
	for (const auto &cmd : _hdDrawCommands)
	{
		hashBytes(cmd.assetPath.data(), cmd.assetPath.size());
		hashInt(cmd.nativeScale); hashBytes(cmd.colorMode.data(), cmd.colorMode.size()); hashInt(cmd.offsetX); hashInt(cmd.offsetY);
		hashInt(cmd.x); hashInt(cmd.y); hashInt(cmd.shade);
		const Uint8 half = cmd.rightHalfOnly ? 1 : 0;
		const Uint8 hasMask = cmd.hasClipMask ? 1 : 0;
		hashBytes(&half, sizeof(half));
		hashUnsigned(cmd.drawOrder);
		hashBytes(&hasMask, sizeof(hasMask));
		if (cmd.hasClipMask)
		{
			hashInt(cmd.clipMask.beg_x); hashInt(cmd.clipMask.beg_y);
			hashInt(cmd.clipMask.end_x); hashInt(cmd.clipMask.end_y);
		}
	}

	// Search RAM history for an identical previously-composed visual state.
	for (size_t i = 0; i < _hdPhysicalFrameCache.size(); ++i)
	{
		auto &entry = _hdPhysicalFrameCache[i];
		if (!entry.valid || entry.sceneHash != sceneHash) continue;
		entry.lastUse = ++_hdPhysicalUseCounter;
		_hdPhysicalCurrentCacheIndex = (int)i;
		_hdPhysicalCachedRevision = _hdPhysicalRevision;
		const uint64_t perfCacheBlitStart = hdPerfNowUs();
		SDL_Rect dst = { (Sint16)px0, (Sint16)py0, 0, 0 };
		SDL_BlitSurface(entry.surface.get(), nullptr, destination, &dst);
		perf.mapCacheBlitUs = hdPerfNowUs() - perfCacheBlitStart;
		_hdPhysicalLastCacheHit = true;
		perf.mapCacheHit = true;
		perf.mapCommands = (unsigned)_hdDrawCommands.size();
		perf.mapPixelsTested = 0;
		perf.mapPixelsWritten = 0;
		_hdPhysicalLastMs = SDL_GetTicks() - started;
		perf.mapPhysicalUs = hdPerfNowUs() - perfPhysicalStart;
		return;
	}

	// No history hit: create or recycle the least-recently-used slot.
	size_t cacheIndex = 0;
	if ((int)_hdPhysicalFrameCache.size() < _hdPhysicalCacheMaxEntries)
	{
		cacheIndex = _hdPhysicalFrameCache.size();
		_hdPhysicalFrameCache.emplace_back();
		auto &entry = _hdPhysicalFrameCache.back();
		std::tie(entry.buffer, entry.surface) = Surface::NewPair32Bit(cacheW, cacheH);
	}
	else
	{
		cacheIndex = 0;
		for (size_t i = 1; i < _hdPhysicalFrameCache.size(); ++i)
		{
			if (_hdPhysicalFrameCache[i].lastUse < _hdPhysicalFrameCache[cacheIndex].lastUse) cacheIndex = i;
		}
	}
	auto &cacheEntry = _hdPhysicalFrameCache[cacheIndex];
	SDL_Surface *cache = cacheEntry.surface.get();
	// Seed the cache from Map's OWN logical surface, never from the final display.
	// The final display can already contain a firing popup, inventory window or
	// another state. Caching those pixels would make a closed window reappear on
	// a later cache HIT.  Re-scaling the legacy map is paid only on a cache MISS.
	const uint64_t perfSeedStart = hdPerfNowUs();
	SDL_FillRect(cache, nullptr, SDL_MapRGBA(cache->format, 0, 0, 0, 255));
	const double legacyZoomX = sx * std::max(1, getDisplayScale());
	const double legacyZoomY = sy * std::max(1, getDisplayScale());
	SDL_Surface *legacyScaled = zoomSurface(getSurface(), legacyZoomX, legacyZoomY, 0);
	if (legacyScaled)
	{
		SDL_Rect srcLegacy = { 0, 0, (Uint16)std::min(cacheW, legacyScaled->w), (Uint16)std::min(cacheH, legacyScaled->h) };
		SDL_Rect zero = { 0, 0, 0, 0 };
		SDL_BlitSurface(legacyScaled, &srcLegacy, cache, &zero);
		SDL_FreeSurface(legacyScaled);
	}
	perf.mapSeedUs = hdPerfNowUs() - perfSeedStart;

	const uint64_t perfCompositeStart = hdPerfNowUs();
	if (!_hdDrawCommands.empty())
	{
		const int mapScale = std::max(1, _spriteWidth / 32);
		if (!SDL_MUSTLOCK(cache) || SDL_LockSurface(cache) == 0)
		{
			SDL_PixelFormat *fmt = cache->format;
			const bool fast32 = fmt && fmt->BytesPerPixel == 4 &&
				fmt->Rmask && fmt->Gmask && fmt->Bmask &&
				fmt->Rloss == 0 && fmt->Gloss == 0 && fmt->Bloss == 0 &&
				(!fmt->Amask || fmt->Aloss == 0);

			auto packFast = [fmt](Uint8 r, Uint8 g, Uint8 b, Uint8 a) -> Uint32
			{
				Uint32 out =
					(((Uint32)r << fmt->Rshift) & fmt->Rmask) |
					(((Uint32)g << fmt->Gshift) & fmt->Gmask) |
					(((Uint32)b << fmt->Bshift) & fmt->Bmask);
				if (fmt->Amask) out |= (((Uint32)a << fmt->Ashift) & fmt->Amask);
				return out;
			};

			// TEST8-B: reuse these tables for every command.  Horizontal source
			// and map-coordinate divisions are now paid once per x position,
			// rather than once per pixel on every row.
			std::vector<unsigned> xSource;
			std::vector<int> xMap;

			// RC12 P4: all compositor geometry below is expressed first in the
			// dedicated x16 render space.  One historical 32x40 cell is therefore
			// 512x640 here.  Mapping back to physical output happens only at the
			// edge of the renderer; gameplay/Camera coordinates remain x1.
			const double renderToPhysicalX = std::max(0.0001, sx / (double)HdRenderSpace::Scale);
			const double renderToPhysicalY = std::max(0.0001, sy / (double)HdRenderSpace::Scale);
			const SDL_Color *activePalette = getPalette();
			const SDL_Color *neutralPalette = hdNeutralBattlescapePalette(_game, activePalette);
			hdBuildEnvironmentProfiles(neutralPalette, activePalette);

			for (const auto &cmd : _hdDrawCommands)
			{
				HdImage *image = _hdImageCache.get(cmd.assetPath);
				if (!image || !image->loaded || !image->width || !image->height || !image->hasVisiblePixels) continue;

				const int nativeScale = HdRenderSpace::resolveNativeScale(cmd.nativeScale, image->width, image->height);
				const double renderDrawW = HdRenderSpace::assetPixelsToRender((double)image->width * mapScale, nativeScale);
				const double renderDrawH = HdRenderSpace::assetPixelsToRender((double)image->height * mapScale, nativeScale);
				const double renderOffsetX = HdRenderSpace::assetPixelsToRender((double)cmd.offsetX * mapScale, nativeScale);
				const double renderOffsetY = HdRenderSpace::assetPixelsToRender((double)cmd.offsetY * mapScale, nativeScale);
				const double baseRenderX = HdRenderSpace::legacyToRender((double)cmd.x) + renderOffsetX;
				const double baseRenderY = HdRenderSpace::legacyToRender((double)cmd.y) + renderOffsetY;
				const int globalX0 = screen->logicalToPhysicalX(logicalMapX + HdRenderSpace::renderToLegacy(baseRenderX));
				const int globalY0 = screen->logicalToPhysicalY(logicalMapY + HdRenderSpace::renderToLegacy(baseRenderY));
				const int globalX1 = screen->logicalToPhysicalX(logicalMapX + HdRenderSpace::renderToLegacy(baseRenderX + renderDrawW));
				const int globalY1 = screen->logicalToPhysicalY(logicalMapY + HdRenderSpace::renderToLegacy(baseRenderY + renderDrawH));
				const int drawW = std::max(1, globalX1 - globalX0);
				const int drawH = std::max(1, globalY1 - globalY0);
				const int startX = cmd.rightHalfOnly ? drawW / 2 : 0;
				const double legacyLight = (16.0 - std::max(0, std::min(16, cmd.shade))) / 16.0;
				const double lightFactor = std::pow(legacyLight, 2.2);

				xSource.resize((size_t)drawW);
				xMap.resize((size_t)drawW);
				for (int xx = 0; xx < drawW; ++xx)
				{
					xSource[(size_t)xx] = std::min(image->width - 1,
						(unsigned)((unsigned long long)xx * image->width / (unsigned)drawW));
					const double renderX = baseRenderX + (double)xx / renderToPhysicalX;
					xMap[(size_t)xx] = (int)std::floor(HdRenderSpace::renderToLegacy(renderX));
				}

				for (int yy = 0; yy < drawH; ++yy)
				{
					const int gy = globalY0 + yy;
					const int cy = gy - py0;
					if (cy < 0 || cy >= cacheH) continue;

					const unsigned iy = std::min(image->height - 1,
						(unsigned)((unsigned long long)yy * image->height / (unsigned)drawH));
					if (iy >= image->alphaRowMinX.size() || iy >= image->alphaRowMaxX.size()) continue;
					const unsigned sourceMin = image->alphaRowMinX[iy];
					const unsigned sourceMax = image->alphaRowMaxX[iy];
					if (sourceMin >= sourceMax) continue;

					const double renderY = baseRenderY + (double)yy / renderToPhysicalY;
					const int mapY = (int)std::floor(HdRenderSpace::renderToLegacy(renderY));
					if (mapY < 0 || mapY >= getHeight()) continue;
					const size_t orderRow = (size_t)mapY * getWidth();

					// Convert the non-transparent source-row span to destination
					// coordinates.  This is conservative and therefore cannot cut
					// any visible pixel; internal alpha holes are still checked below.
					const int spanStart = (int)(((unsigned long long)sourceMin * drawW + image->width - 1) / image->width);
					const int spanEnd = (int)(((unsigned long long)sourceMax * drawW + image->width - 1) / image->width);
					int xx0 = std::max(startX, spanStart);
					int xx1 = std::min(drawW, spanEnd);
					xx0 = std::max(xx0, px0 - globalX0);
					xx1 = std::min(xx1, px1 - globalX0);
					if (xx0 >= xx1) continue;

					for (int xx = xx0; xx < xx1; ++xx)
					{
						++_hdPhysicalPixelsTested;
						const int mapX = xMap[(size_t)xx];
						if (mapX < 0 || mapX >= getWidth()) continue;
						if (cmd.hasClipMask && (mapX < cmd.clipMask.beg_x || mapX >= cmd.clipMask.end_x || mapY < cmd.clipMask.beg_y || mapY >= cmd.clipMask.end_y)) continue;
						const size_t orderIndex = orderRow + (size_t)mapX;
						if (orderIndex < _drawOrderBuffer.size() && _drawOrderBuffer[orderIndex] > cmd.drawOrder) continue;

						const unsigned ix = xSource[(size_t)xx];
						const size_t pixelIndex = (size_t)iy * image->width + ix;
						const HdColorMode colorMode = resolveHdColorMode(cmd.colorMode, image->paletteIndexed8);
						Uint8 sa = 255, sr = 0, sg = 0, sb = 0;
						if (colorMode == HdColorMode::IndexedLegacy && pixelIndex < image->indices.size())
						{
							const Uint8 srcIndex = image->indices[pixelIndex];
							if (!srcIndex) continue;
							const Uint8 shaded = (Uint8)(srcIndex + std::max(0, std::min(16, cmd.shade)));
							const Uint8 finalIndex = ((shaded ^ srcIndex) & 0xF0) ? 0x0F : shaded;
							const SDL_Color &pc = activePalette[finalIndex];
							sr = pc.r; sg = pc.g; sb = pc.b;
						}
						else
						{
							const size_t si = pixelIndex * 4u;
							sa = image->rgba[si + 3];
							if (!sa) continue;
							sr = image->rgba[si + 0];
							sg = image->rgba[si + 1];
							sb = image->rgba[si + 2];
							if (colorMode == HdColorMode::Environment && Options::hdEnvironmentGrade) hdEnvironmentTransformFor(cmd.environmentProfile).apply(sr, sg, sb);
							sr = (Uint8)std::lround((double)sr * lightFactor);
							sg = (Uint8)std::lround((double)sg * lightFactor);
							sb = (Uint8)std::lround((double)sb * lightFactor);
						}

						const int cx = globalX0 + xx - px0;
						if (cx < 0 || cx >= cacheW) continue;
						Uint8 *pixelAddress = (Uint8*)cache->pixels + cy * cache->pitch + cx * 4;
						Uint32 out;

						if (sa == 255)
						{
							out = fast32 ? packFast(sr, sg, sb, 255) : SDL_MapRGBA(fmt, sr, sg, sb, 255);
						}
						else
						{
							Uint32 dstPixel;
							memcpy(&dstPixel, pixelAddress, sizeof(dstPixel));
							Uint8 dr, dg, db, da;
							if (fast32)
							{
								dr = (Uint8)((dstPixel & fmt->Rmask) >> fmt->Rshift);
								dg = (Uint8)((dstPixel & fmt->Gmask) >> fmt->Gshift);
								db = (Uint8)((dstPixel & fmt->Bmask) >> fmt->Bshift);
								da = fmt->Amask ? (Uint8)((dstPixel & fmt->Amask) >> fmt->Ashift) : 255;
							}
							else
							{
								SDL_GetRGBA(dstPixel, fmt, &dr, &dg, &db, &da);
							}
							const unsigned inv = 255 - sa;
							const Uint8 rr = (Uint8)((sr * sa + dr * inv + 127) / 255);
							const Uint8 rg = (Uint8)((sg * sa + dg * inv + 127) / 255);
							const Uint8 rb = (Uint8)((sb * sa + db * inv + 127) / 255);
							const Uint8 ra = (Uint8)std::min(255u, (unsigned)sa + ((unsigned)da * inv + 127) / 255);
							out = fast32 ? packFast(rr, rg, rb, ra) : SDL_MapRGBA(fmt, rr, rg, rb, ra);
						}
						memcpy(pixelAddress, &out, sizeof(out));
						++_hdPhysicalPixelsWritten;
					}
				}
			}
			if (SDL_MUSTLOCK(cache)) SDL_UnlockSurface(cache);
		}
	}
	perf.mapCompositeUs = hdPerfNowUs() - perfCompositeStart;
	cacheEntry.sceneHash = sceneHash;
	cacheEntry.lastUse = ++_hdPhysicalUseCounter;
	cacheEntry.valid = true;
	_hdPhysicalCurrentCacheIndex = (int)cacheIndex;
	_hdPhysicalCachedRevision = _hdPhysicalRevision;
	const uint64_t perfCacheBlitStart = hdPerfNowUs();
	SDL_Rect dst = { (Sint16)px0, (Sint16)py0, 0, 0 };
	SDL_BlitSurface(cache, nullptr, destination, &dst);
	perf.mapCacheBlitUs = hdPerfNowUs() - perfCacheBlitStart;
	perf.mapCacheHit = false;
	perf.mapCommands = (unsigned)_hdDrawCommands.size();
	perf.mapPixelsTested = _hdPhysicalPixelsTested;
	perf.mapPixelsWritten = _hdPhysicalPixelsWritten;
	_hdPhysicalLastMs = SDL_GetTicks() - started;
	perf.mapPhysicalUs = hdPerfNowUs() - perfPhysicalStart;
}

void Map::drawUnit(UnitSprite &unitSprite, Tile *unitTile, Tile *currTile, Position currTileScreenPosition, bool topLayer, BattleUnit* movingUnit)
{
	if (!unitTile)
	{
		return;
	}
	BattleUnit* bu = unitTile->getOverlappingUnit(_save, TUO_ALWAYS);
	Position unitOffset;
	bool unitFromBelow = false;
	bool unitFromAbove = false;
	if (bu)
	{
		if (bu != unitTile->getUnit())
		{
			unitFromBelow = true;
		}
	}
	else if (movingUnit && unitTile == currTile)
	{
		auto* upperTile = _save->getAboveTile(unitTile);
		if (upperTile && upperTile->hasNoFloor(_save))
		{
			bu = upperTile->getUnit();
		}
		if (bu != movingUnit)
		{
			return;
		}
		unitFromAbove = true;
	}
	else
	{
		return;
	}

	// RC12 flare reconnaissance: a hostile unit standing in terrain currently
	// reached by ground-flare light may be drawn as a visual silhouette without
	// becoming an OXCE "visible" unit.  This is render-only: no HUD contact, no
	// reaction/FOV side effect, and the unit disappears again when LL_ITEMS light
	// no longer reaches its tile.
	const bool flareVisualContact = unitTile->isDiscovered(O_FLOOR) && unitTile->getLight(LL_ITEMS) > 0;
	if (!(bu->getVisible() || flareVisualContact || _save->getDebugMode()))
	{
		return;
	}

	// Clipping/occlusion geometry belongs to the map grid, not to the current
	// armor SurfaceSet. A unit asset can therefore never resize a tile mask.
	const int graphicsScale = std::max(1, _spriteWidth / 32);
	const int tileFoorWidth = 32 * graphicsScale;
	const int tileFoorHeight = 16 * graphicsScale;
	const int tileHeight = 40 * graphicsScale;

	unitOffset.x = unitTile->getPosition().x - bu->getPosition().x;
	unitOffset.y = unitTile->getPosition().y - bu->getPosition().y;
	int part = unitOffset.x + unitOffset.y*2;

	bool moving = bu->getStatus() == STATUS_WALKING || bu->getStatus() == STATUS_FLYING;
	int bonusWidth = moving ? 0 : tileFoorWidth;
	int topMargin = 0;
	int bottomMargin = 0;

	//if unit is from below then we draw only part that in in tile
	if (unitFromBelow)
	{
		bottomMargin = -tileFoorHeight / 2;
		topMargin = tileFoorHeight;
	}
	else if (topLayer)
	{
		topMargin = 2 * tileFoorHeight;
	}
	else
	{
		const Tile *top = _save->getAboveTile(unitTile);
		if (top && top->getOverlappingUnit(_save, TUO_ALWAYS) == bu)
		{
			topMargin = -tileFoorHeight / 2;
		}
		else
		{
			topMargin = tileFoorHeight;
		}
	}

	GraphSubset mask = GraphSubset(tileFoorWidth + bonusWidth, tileHeight + topMargin + bottomMargin).offset(currTileScreenPosition.x - bonusWidth / 2, currTileScreenPosition.y - topMargin);

	if (moving)
	{
		GraphSubset leftMask = mask.offset(-tileFoorWidth/2, 0);
		GraphSubset rightMask = mask.offset(+tileFoorWidth/2, 0);
		int direction = bu->getDirection();
		Position partCurr = currTile->getPosition();
		Position partDest = bu->getDestination() + unitOffset;
		Position partLast = bu->getLastPosition() + unitOffset;
		bool isTileDestPos = positionHaveSameXY(partDest, partCurr);
		bool isTileLastPos = positionHaveSameXY(partLast, partCurr);

		if (unitFromAbove && partLast != unitTile->getPosition())
		{
			//this tile is below moving unit and it do not change levels, nothing to draw
			return;
		}

		//adjusting mask
		if (positionHaveSameXY(partLast, partDest))
		{
			if (currTile == unitTile)
			{
				//no change
			}
			else
			{
				//nothing to draw
				return;
			}
		}
		else if (isTileDestPos)
		{
			//unit is moving to this tile
			switch (direction)
			{
			case 0:
			case 1:
				mask = GraphSubset::intersection(mask, rightMask);
				break;
			case 2:
				//no change
				break;
			case 3:
				//no change
				break;
			case 4:
				//no change
				break;
			case 5:
			case 6:
				mask = GraphSubset::intersection(mask, leftMask);
				break;
			case 7:
				//nothing to draw
				return;
			}
		}
		else if (isTileLastPos)
		{
			//unit is exiting this tile
			switch (direction)
			{
			case 0:
				//no change
				break;
			case 1:
			case 2:
				mask = GraphSubset::intersection(mask, leftMask);
				break;
			case 3:
				//nothing to draw
				return;
			case 4:
			case 5:
				mask = GraphSubset::intersection(mask, rightMask);
				break;
			case 6:
				//no change
				break;
			case 7:
				//no change
				break;
			}
		}
		else
		{
			Position leftPos = partCurr + Position(-1, 0, 0);
			Position rightPos = partCurr + Position(0, -1, 0);
			if (!topLayer && (partDest.z > partCurr.z || partLast.z > partCurr.z))
			{
				//unit change layers, it will be drawn by upper layer not lower.
				return;
			}
			else if (
				(direction == 1 && (partDest == rightPos || partLast == leftPos)) ||
				(direction == 5 && (partDest == leftPos || partLast == rightPos)))
			{
				mask = GraphSubset(tileFoorWidth, tileHeight + 2 * tileFoorHeight).offset(currTileScreenPosition.x, currTileScreenPosition.y - 2 * tileFoorHeight);
			}
			else
			{
				//unit is not moving close to tile
				return;
			}
		}
	}
	else if (unitTile != currTile || unitFromAbove)
	{
		return;
	}

	Position tileScreenPosition;
	_camera->convertMapToScreen(unitTile->getPosition() + Position(0,0, (-unitFromBelow) + (+unitFromAbove)), &tileScreenPosition);
	tileScreenPosition += _camera->getMapOffset();

	//get shade helpers
	auto getTileShade = [&](Tile* tile)
	{
		return tile ? (tile->isDiscovered(O_FLOOR) ? reShade(tile) : 16) : 16;
	};
	auto getMixedTileShade = [&](Tile* tile, int heightOffset, bool below)
	{
		int shadeLower = 0;
		int shadeUpper = 0;
		if (below)
		{
			shadeLower = getTileShade(_save->getBelowTile(tile));
			shadeUpper = getTileShade(tile);
		}
		else
		{
			shadeLower = getTileShade(tile);
			shadeUpper = getTileShade(_save->getAboveTile(tile));
		}

		return Interpolate(shadeLower, shadeUpper, -heightOffset, Position::TileZ);
	};

	// draw unit
	int shade = 0;
	UnitWalkingOffset offsets = calculateWalkingOffset(bu);
	if (moving)
	{
		const Position start = bu->getPosition();
		const Position end = bu->getDestination();
		const auto minLevel = std::min(start.z, end.z); // Sint16
		const int startShade = getMixedTileShade(_save->getTile(start), start.z == minLevel ? offsets.TerrainLevelOffset : 0, false);
		const int endShade = getMixedTileShade(_save->getTile(end), end.z == minLevel ? offsets.TerrainLevelOffset : 0, false);
		shade = Interpolate(startShade, endShade, offsets.NormalizedMovePhase, 16);
	}
	else
	{
		shade = getMixedTileShade(currTile, offsets.TerrainLevelOffset, unitFromBelow);
		if (_showObstacles && unitTile->getObstacle(4))
		{
			shade = getShadePulseForFrame(shade, _animFrame);
		}
	}
	if (_debugVisionMode == 1)
	{
		shade = std::min(+NIGHT_VISION_SHADE, shade);
	}
    const Armor *partsArmor = bu->getArmor();
    const bool partsMode = Options::hdGraphics && partsArmor &&
        !partsArmor->getHdUnitPartsRoot().empty() && partsArmor->getHdUnitPartsScale() > 0;
    if (partsMode)
    {
        unitSprite.setLayerRenderer([&](const UnitSprite::RenderLayer &layer,
            const std::function<void()> &legacyDraw) {
            const unsigned layerOrder = nextDrawSequence();
            std::string asset;
            // Weapons and effects retain their existing renderer in this first adapter.
            // Burned/collapsing bodies with a burn transform also retain the legacy shader.
            if (layer.body && layer.frame >= 0 && layer.burn == 0)
            {
                std::string dataset = layer.dataset;
                const std::size_t dot = dataset.find_last_of('.');
                if (dot != std::string::npos) dataset.erase(dot);
                std::ostringstream number;
                number << std::setw(4) << std::setfill('0') << layer.frame;
                asset = partsArmor->getHdUnitPartsRoot() + "/" + dataset + "/" + number.str() + ".png";
            }
            if (!asset.empty() && _hdImageCache.exists(asset))
            {
                queueHdAsset(asset, partsArmor->getHdUnitPartsScale(), 0, 0,
                    layer.x, layer.y, layer.shade, false, layerOrder, &layer.mask);
                return;
            }
            const std::vector<Uint8> beforeLayer = snapshotArea(layer.mask);
            legacyDraw();
            markChangedDrawOrder(beforeLayer, layer.mask, layerOrder);
        });
        unitSprite.draw(bu, part, tileScreenPosition.x + offsets.ScreenOffset.x,
            tileScreenPosition.y + offsets.ScreenOffset.y, shade, mask, _isAltPressed && !_isCtrlPressed);
        unitSprite.setLayerRenderer(UnitSprite::LayerRenderer());
        return;
    }
	if (usesFullBodySprite(bu))
	{
		const Armor *armor = bu->getArmor();
		std::string combined = findHdUnitCombinedAsset(bu);
		std::string body = combined.empty() ? findHdUnitBodyAsset(bu) : combined;
		if (body.empty() && !armor->getFullBodySprite().empty() && _hdImageCache.exists(armor->getFullBodySprite()))
		{
			body = armor->getFullBodySprite();
		}
		if (!body.empty())
		{
			Position fullBodyScreenPosition;
			_camera->convertMapToScreen(bu->getPosition(), &fullBodyScreenPosition);
			fullBodyScreenPosition += _camera->getMapOffset();
			const int drawX = fullBodyScreenPosition.x + offsets.ScreenOffset.x;
			const int drawY = fullBodyScreenPosition.y + offsets.ScreenOffset.y;
			const unsigned order = nextDrawSequence();
			queueHdAsset(body, armor->getFullBodySpriteScale(), armor->getFullBodySpriteOffsetX(), armor->getFullBodySpriteOffsetY(), drawX, drawY, shade, false, order, &mask);

			// A pre-composed weapon-specific image wins. Otherwise a body core may
			// receive independent left/right arm and weapon layers. Missing layers
			// are simply ignored, so production can remain incremental.
			if (combined.empty() && !armor->getFullBodySpriteRoot().empty())
			{
				const BattleItem *left = bu->getLeftHandWeapon();
				const BattleItem *right = bu->getRightHandWeapon();
				const std::string armL = findHdUnitOverlayAsset(bu, "arms", "left", left);
				const std::string armR = findHdUnitOverlayAsset(bu, "arms", "right", right);
				const std::string weaponL = findHdUnitOverlayAsset(bu, "weapons", "left", left);
				const std::string weaponR = findHdUnitOverlayAsset(bu, "weapons", "right", right);
				if (!armL.empty()) queueHdAsset(armL, armor->getFullBodySpriteScale(), armor->getFullBodySpriteOffsetX(), armor->getFullBodySpriteOffsetY(), drawX, drawY, shade, false, order, &mask);
				if (!armR.empty()) queueHdAsset(armR, armor->getFullBodySpriteScale(), armor->getFullBodySpriteOffsetX(), armor->getFullBodySpriteOffsetY(), drawX, drawY, shade, false, order, &mask);
				if (!weaponL.empty()) queueHdAsset(weaponL, armor->getFullBodySpriteScale(), armor->getFullBodySpriteOffsetX(), armor->getFullBodySpriteOffsetY(), drawX, drawY, shade, false, order, &mask);
				if (!weaponR.empty()) queueHdAsset(weaponR, armor->getFullBodySpriteScale(), armor->getFullBodySpriteOffsetX(), armor->getFullBodySpriteOffsetY(), drawX, drawY, shade, false, order, &mask);
			}
			return;
		}
	}

	const unsigned order = nextDrawSequence();
	const std::vector<Uint8> before = snapshotArea(mask);
	unitSprite.draw(bu, part, tileScreenPosition.x + offsets.ScreenOffset.x, tileScreenPosition.y + offsets.ScreenOffset.y, shade, mask, _isAltPressed && !_isCtrlPressed);
	markChangedDrawOrder(before, mask, order);
}

/**
 * Draw the terrain.
 * Keep this function as optimised as possible. It's big to minimise overhead of function calls.
 * @param surface The surface to draw on.
 */
void Map::drawTerrain(Surface *surface)
{
	_hdDrawCommands.clear();
	_drawSequence = 0;
	_drawOrderBuffer.assign((size_t)getWidth() * getHeight(), 0);
	_isAltPressed = _game->isAltPressed(true);
	_isCtrlPressed = _game->isCtrlPressed(true);
	int frameNumber = 0;
	SurfaceRaw<const Uint8> tmpSurface;
	Tile *tile;
	int beginX = 0, endX = _save->getMapSizeX() - 1;
	int beginY = 0, endY = _save->getMapSizeY() - 1;
	int beginZ = 0, endZ = _save->getMapSizeZ() - 1;
	Position mapPosition, screenPosition, bulletPositionScreen, movingUnitPosition;
	int bulletLowX=16000, bulletLowY=16000, bulletLowZ=16000, bulletHighX=0, bulletHighY=0, bulletHighZ=0;
	int dummy;
	BattleUnit *movingUnit = _save->getTileEngine()->getMovingUnit();
	int tileShade, tileColor, obstacleShade;
	UnitSprite unitSprite(surface, _game->getMod(), _save, _animFrame, _save->getDepth() != 0,
		_isTFTD ? ArrowColorsTFTD[1] : ArrowColorsUFO[1], _isTFTD ? ArrowColorsTFTD[2] : ArrowColorsUFO[2]);
	ItemSprite itemSprite(surface, _game->getMod(), _save, _animFrame);

	const int halfAnimFrame = (_animFrame / 2) % 4;
	const int halfAnimFrameRest = (_animFrame % 2);

	NumberText *_numWaypid = 0;

	// if we got bullet, get the highest x and y tiles to draw it on
	if (_projectile && _explosions.empty())
	{
		int part = _projectile->getItem() ? 0 : BULLET_SPRITES-1;
		for (int i = 0; i <= part; ++i)
		{
			if (_projectile->getPosition(1-i).x < bulletLowX)
				bulletLowX = _projectile->getPosition(1-i).x;
			if (_projectile->getPosition(1-i).y < bulletLowY)
				bulletLowY = _projectile->getPosition(1-i).y;
			if (_projectile->getPosition(1-i).z < bulletLowZ)
				bulletLowZ = _projectile->getPosition(1-i).z;
			if (_projectile->getPosition(1-i).x > bulletHighX)
				bulletHighX = _projectile->getPosition(1-i).x;
			if (_projectile->getPosition(1-i).y > bulletHighY)
				bulletHighY = _projectile->getPosition(1-i).y;
			if (_projectile->getPosition(1-i).z > bulletHighZ)
				bulletHighZ = _projectile->getPosition(1-i).z;
		}
		// divide by 16 to go from voxel to tile position
		bulletLowX = bulletLowX / 16;
		bulletLowY = bulletLowY / 16;
		bulletLowZ = bulletLowZ / 24;
		bulletHighX = bulletHighX / 16;
		bulletHighY = bulletHighY / 16;
		bulletHighZ = bulletHighZ / 24;

		// if the projectile is outside the viewport - center it back on it
		_camera->convertVoxelToScreen(_projectile->getPosition(), &bulletPositionScreen);

		if (_projectileInFOV && _followProjectile)
		{
			Position newCam = _camera->getMapOffset();
			if (newCam.z != bulletHighZ) //switch level
			{
				newCam.z = bulletHighZ;
				if (_projectileInFOV)
				{
					_camera->setMapOffset(newCam);
					_camera->convertVoxelToScreen(_projectile->getPosition(), &bulletPositionScreen);
				}
			}
			if (_smoothCamera)
			{
				if (_launch)
				{
					_launch = false;
					if ((bulletPositionScreen.x < 1 || bulletPositionScreen.x > surface->getWidth() - 1 ||
						bulletPositionScreen.y < 1 || bulletPositionScreen.y > _visibleMapHeight - 1))
					{
						_camera->centerOnPosition(Position(bulletLowX, bulletLowY, bulletHighZ), false);
						_camera->convertVoxelToScreen(_projectile->getPosition(), &bulletPositionScreen);
					}
				}
				if (!_smoothingEngaged)
				{
					if (bulletPositionScreen.x < 1 || bulletPositionScreen.x > surface->getWidth() - 1 ||
						bulletPositionScreen.y < 1 || bulletPositionScreen.y > _visibleMapHeight - 1)
					{
						_smoothingEngaged = true;
					}
				}
				else
				{
					_camera->jumpXY(surface->getWidth() / 2 - bulletPositionScreen.x, _visibleMapHeight / 2 - bulletPositionScreen.y);
				}
			}
			else
			{
				bool enough;
				do
				{
					enough = true;
					if (bulletPositionScreen.x < 0)
					{
						_camera->jumpXY(+surface->getWidth(), 0);
						enough = false;
					}
					else if (bulletPositionScreen.x > surface->getWidth())
					{
						_camera->jumpXY(-surface->getWidth(), 0);
						enough = false;
					}
					else if (bulletPositionScreen.y < 0)
					{
						_camera->jumpXY(0, +_visibleMapHeight);
						enough = false;
					}
					else if (bulletPositionScreen.y > _visibleMapHeight)
					{
						_camera->jumpXY(0, -_visibleMapHeight);
						enough = false;
					}
					_camera->convertVoxelToScreen(_projectile->getPosition(), &bulletPositionScreen);
				}
				while (!enough);
			}
		}
	}

	// get corner map coordinates to give rough boundaries in which tiles to redraw are
	_camera->convertScreenToMap(0, 0, &beginX, &dummy);
	_camera->convertScreenToMap(surface->getWidth(), 0, &dummy, &beginY);
	_camera->convertScreenToMap(surface->getWidth() + _spriteWidth, surface->getHeight() + _spriteHeight, &endX, &dummy);
	_camera->convertScreenToMap(0, surface->getHeight() + _spriteHeight, &dummy, &endY);
	beginY -= (_camera->getViewLevel() * 2);
	beginX -= (_camera->getViewLevel() * 2);
	if (beginX < 0)
		beginX = 0;
	if (beginY < 0)
		beginY = 0;

	if (!_camera->getShowAllLayers())
	{
		endZ = std::min(endZ, _camera->getViewLevel());
	}
	if (_camera->getShowSingleLayer())
	{
		beginZ = _camera->getViewLevel();
		endZ = _camera->getViewLevel();
	}


	const int mapGraphicsScale = std::max(1, _spriteWidth / 32);
	bool pathfinderTurnedOn = _save->getPathfinding()->isPathPreviewed();

	if (!_waypoints.empty() || (pathfinderTurnedOn && (_previewSettingTu || _previewSettingEnergy)))
	{
		_numWaypid = new NumberText(15, 15, 20, 30);
		_numWaypid->setPalette(getPalette());
		_numWaypid->setColor(pathfinderTurnedOn ? _messageColor + 1 : Palette::blockOffset(1));
	}

	if (movingUnit)
	{
		movingUnitPosition = movingUnit->getPosition();
	}

	surface->lock();
	const Position cameraPos = _camera->getMapOffset();
	for (int itZ = beginZ; itZ <= endZ; itZ++)
	{
		bool topLayer = itZ == endZ;
		for (int itY = beginY; itY < endY; itY++)
		{
			mapPosition = Position(beginX, itY, itZ);
			tile = _save->getTile(mapPosition);
			for (int itX = beginX; itX < endX; itX++, mapPosition.x++, tile++)
			{
				_camera->convertMapToScreen(mapPosition, &screenPosition);
				screenPosition += cameraPos;

				// only render cells that are inside the surface
				if (screenPosition.x > -_spriteWidth && screenPosition.x < surface->getWidth() + _spriteWidth &&
					screenPosition.y > -_spriteHeight && screenPosition.y < surface->getHeight() + _spriteHeight )
				{
					bool isUnitMovingNearby = movingUnit && positionInRangeXY(movingUnitPosition, mapPosition, 2);

					if (tile->isDiscovered(O_FLOOR))
					{
						tileShade = reShade(tile);
						obstacleShade = tileShade;
						if (_showObstacles)
						{
							if (tile->isObstacle())
							{
								obstacleShade = getShadePulseForFrame(tileShade, _animFrame);
							}
						}
					}
					else
					{
						tileShade = 16;
						obstacleShade = 16;
					}

					tileColor = tile->getMarkerColor();

					// Draw floor. A convention-based RGBA replacement can bypass only
					// this frame while all surrounding terrain remains legacy.
					tmpSurface = tile->getSprite(O_FLOOR);
					if (tmpSurface || findHdTerrainVisualRule(tile, O_FLOOR))
					{
						const int floorShade = tile->getObstacle(O_FLOOR) ? obstacleShade : tileShade;
						const int hx = screenPosition.x;
						const int hy = screenPosition.y - tile->getYOffset(O_FLOOR) * mapGraphicsScale;
						const unsigned hdOrder = nextDrawSequence();
						if (!queueHdTerrainOrConvention(tile, O_FLOOR, hx, hy, floorShade, false, hdOrder))
						{
							if (tmpSurface)
							{
								Surface::blitRaw(surface, tmpSurface, hx, hy, floorShade, false, _nvColor);
								markSourceDrawOrder(tmpSurface, hx, hy, false, hdOrder);
							}
						}
					}

					auto* unit = tile->getUnit();

					// Draw cursor back
					if (_cursorType != CT_NONE && _selectorX > itX - _cursorSize && _selectorY > itY - _cursorSize && _selectorX < itX+1 && _selectorY < itY+1 && !_save->getBattleState()->getMouseOverIcons())
					{
						if (_camera->getViewLevel() == itZ)
						{
							if (_cursorType != CT_AIM)
							{
								if (unit && (unit->getVisible() || _save->getDebugMode()))
									frameNumber = halfAnimFrameRest; // yellow box
								else
									frameNumber = 0; // red box
							}
							else
							{
								if (unit && (unit->getVisible() || _save->getDebugMode()))
									frameNumber = 7 + halfAnimFrame; // yellow animated crosshairs
								else
									frameNumber = 6; // red static crosshairs
							}
							tmpSurface = _game->getMod()->getSurfaceSet("CURSOR.PCK")->getFrame(frameNumber);
							drawHdSurfaceSetOrLegacy(surface, "CURSOR.PCK", frameNumber, tmpSurface, screenPosition.x, screenPosition.y, 0);
						}
						else if (_camera->getViewLevel() > itZ)
						{
							frameNumber = 2; // blue box
							tmpSurface = _game->getMod()->getSurfaceSet("CURSOR.PCK")->getFrame(frameNumber);
							drawHdSurfaceSetOrLegacy(surface, "CURSOR.PCK", frameNumber, tmpSurface, screenPosition.x, screenPosition.y, 0);
						}
					}

					if (isUnitMovingNearby)
					{
						// special handling for a moving unit in background of tile.
						constexpr static Position backPos[] =
						{
							Position(0, -1, 0),
							Position(-1, -1, 0),
							Position(-1, 0, 0),
						};

						for (size_t b = 0; b < std::size(backPos); ++b)
						{
							drawUnit(unitSprite, _save->getTile(mapPosition + backPos[b]), tile, screenPosition, topLayer);
						}
					}

					// Draw walls
					{
						// Draw west wall
						tmpSurface = tile->getSprite(O_WESTWALL);
						if (tmpSurface || findHdTerrainVisualRule(tile, O_WESTWALL))
						{
							const int wallShade = tile->getObstacle(O_WESTWALL) ? obstacleShade : getWallShade(O_WESTWALL, tile);
							const int hx = screenPosition.x;
							const int hy = screenPosition.y - tile->getYOffset(O_WESTWALL) * mapGraphicsScale;
							const unsigned hdOrder = nextDrawSequence();
							if (!queueHdTerrainOrConvention(tile, O_WESTWALL, hx, hy, wallShade, false, hdOrder))
							{
								if (tmpSurface)
								{
									Surface::blitRaw(surface, tmpSurface, hx, hy, wallShade, false, _nvColor);
									markSourceDrawOrder(tmpSurface, hx, hy, false, hdOrder);
								}
							}
						}
						// Draw north wall
						tmpSurface = tile->getSprite(O_NORTHWALL);
						if (tmpSurface || findHdTerrainVisualRule(tile, O_NORTHWALL))
						{
							const bool halfWall = bool(tile->getSprite(O_WESTWALL));
							const int wallShade = tile->getObstacle(O_NORTHWALL) ? obstacleShade : getWallShade(O_NORTHWALL, tile);
							const int hx = screenPosition.x;
							const int hy = screenPosition.y - tile->getYOffset(O_NORTHWALL) * mapGraphicsScale;
							const unsigned hdOrder = nextDrawSequence();
							if (!queueHdTerrainOrConvention(tile, O_NORTHWALL, hx, hy, wallShade, halfWall, hdOrder))
							{
								if (tmpSurface)
								{
									Surface::blitRaw(surface, tmpSurface, hx, hy, wallShade, halfWall, _nvColor);
									markSourceDrawOrder(tmpSurface, hx, hy, halfWall, hdOrder);
								}
							}
						}
						// Draw object behind the unit when the MCD requests it.
						tmpSurface = tile->getSprite(O_OBJECT);
						if ((tmpSurface || findHdTerrainVisualRule(tile, O_OBJECT)) && tile->isBackTileObject(O_OBJECT))
						{
							const int objectShade = tile->getObstacle(O_OBJECT) ? obstacleShade : tileShade;
							const int hx = screenPosition.x;
							const int hy = screenPosition.y - tile->getYOffset(O_OBJECT) * mapGraphicsScale;
							const unsigned hdOrder = nextDrawSequence();
							if (!queueHdTerrainOrConvention(tile, O_OBJECT, hx, hy, objectShade, false, hdOrder))
							{
								if (tmpSurface)
								{
									Surface::blitRaw(surface, tmpSurface, hx, hy, objectShade, false, _nvColor);
									markSourceDrawOrder(tmpSurface, hx, hy, false, hdOrder);
								}
							}
						}
						// draw an item/corpse on top of the floor (if any). FLOOROB.PCK
						// frames can also be replaced by native-resolution RGBA files (x4/x8/x16).
						BattleItem* item = tile->getTopItem();
						if (item)
						{
							GraphSubset itemTrackArea(_spriteWidth * 2, _spriteHeight * 2);
							itemTrackArea = itemTrackArea.offset(screenPosition.x - _spriteWidth / 2, screenPosition.y - _spriteHeight / 2);
							const int itemFrame = item->getFloorSpriteIndex(_save, _animFrame, tileShade);
							const int itemX = screenPosition.x;
							const int itemY = screenPosition.y + tile->getTerrainLevel() * mapGraphicsScale;
							const unsigned itemOrder = nextDrawSequence();
							std::ostringstream itemKey;
							itemKey << "groundItem:" << item->getId();
							if (!queueHdSurfaceSetOrConvention("FLOOROB.PCK", itemFrame, itemKey.str(), itemX, itemY, tileShade, false, itemOrder))
							{
								auto before = snapshotArea(itemTrackArea);
								itemSprite.draw(item, itemX, itemY, tileShade);
								markChangedDrawOrder(before, itemTrackArea, itemOrder);
							}

							if (_anyIndicator)
							{
								auto before = snapshotArea(itemTrackArea); const unsigned order = nextDrawSequence();
								BattleUnit *itemUnit = item->getUnit();
								if (itemUnit && itemUnit->getStatus() == STATUS_UNCONSCIOUS && itemUnit->indicatorsAreEnabled())
								{
									Surface *indicator = nullptr;
									if (_burnIndicator && itemUnit->getFire() > 0) indicator = _burnIndicator;
									else if (_woundIndicator && itemUnit->getFatalWounds() > 0) indicator = _woundIndicator;
									else if (_shockIndicator && itemUnit->hasNegativeHealthRegen()) indicator = _shockIndicator;
									else indicator = _stunIndicator;
									if (indicator) indicator->blitNShade(surface, screenPosition.x, screenPosition.y + tile->getTerrainLevel() * mapGraphicsScale, tileShade);
								}
								markChangedDrawOrder(before, itemTrackArea, order);
							}
						}
					}

					// check if we got bullet && it is in Field Of View
					if (_projectile && _projectileInFOV)
					{
						tmpSurface = nullptr;
						BattleItem* item = _projectile->getItem();
						if (item)
						{
							Position voxelPos = _projectile->getPosition();
							// draw shadow on the floor
							voxelPos.z = _save->getTileEngine()->castedShade(voxelPos);
							if (voxelPos.x / 16 >= itX &&
								voxelPos.y / 16 >= itY &&
								voxelPos.x / 16 <= itX+1 &&
								voxelPos.y / 16 <= itY+1 &&
								voxelPos.z / 24 == itZ &&
								_save->getTileEngine()->isVoxelVisible(voxelPos))
							{
								_camera->convertVoxelToScreen(voxelPos, &bulletPositionScreen);

								{
									const int ix = bulletPositionScreen.x - 16 * mapGraphicsScale;
									const int iy = bulletPositionScreen.y - 26 * mapGraphicsScale;
									GraphSubset area(_spriteWidth * 2, _spriteHeight * 2); area = area.offset(ix - _spriteWidth / 2, iy - _spriteHeight / 2);
									auto before = snapshotArea(area); const unsigned order = nextDrawSequence();
									itemSprite.drawShadow(item, ix, iy);
									markChangedDrawOrder(before, area, order);
								}
							}

							voxelPos = _projectile->getPosition();
							// draw thrown object
							if (voxelPos.x / 16 >= itX &&
								voxelPos.y / 16 >= itY &&
								voxelPos.x / 16 <= itX+1 &&
								voxelPos.y / 16 <= itY+1 &&
								voxelPos.z / 24 == itZ &&
								_save->getTileEngine()->isVoxelVisible(voxelPos))
							{
								_camera->convertVoxelToScreen(voxelPos, &bulletPositionScreen);

								{
									const int ix = bulletPositionScreen.x - 16 * mapGraphicsScale;
									const int iy = bulletPositionScreen.y - 26 * mapGraphicsScale;
									const int itemFrame = item->getFloorSpriteIndex(_save, _animFrame, tileShade);
									const unsigned itemOrder = nextDrawSequence();
									std::ostringstream itemKey;
									itemKey << "thrownItem:" << item->getId();
									if (!queueHdSurfaceSetOrConvention("FLOOROB.PCK", itemFrame, itemKey.str(), ix, iy, tileShade, false, itemOrder))
									{
										GraphSubset area(_spriteWidth * 2, _spriteHeight * 2); area = area.offset(ix - _spriteWidth / 2, iy - _spriteHeight / 2);
										auto before = snapshotArea(area);
										itemSprite.draw(item, ix, iy, tileShade);
										markChangedDrawOrder(before, area, itemOrder);
									}
								}
							}
						}
						else
						{
							// draw bullet on the correct tile
							if (itX >= bulletLowX && itX <= bulletHighX && itY >= bulletLowY && itY <= bulletHighY)
							{
								int begin = 0;
								int end = BULLET_SPRITES;
								int direction = 1;
								if (_projectile->isReversed())
								{
									begin = BULLET_SPRITES - 1;
									end = -1;
									direction = -1;
								}

								for (int i = begin; i != end; i += direction)
								{
									const int projectileFrame = _projectile->getParticle(i);
									tmpSurface = _projectileSet->getFrame(projectileFrame);
									if (tmpSurface)
									{
										Position voxelPos = _projectile->getPosition(1-i);
										// draw shadow on the floor
										voxelPos.z = _save->getTileEngine()->castedShade(voxelPos);
										if (voxelPos.x / 16 == itX &&
											voxelPos.y / 16 == itY &&
											voxelPos.z / 24 == itZ &&
											_save->getTileEngine()->isVoxelVisible(voxelPos))
										{
											_camera->convertVoxelToScreen(voxelPos, &bulletPositionScreen);
											bulletPositionScreen.x -= tmpSurface.getWidth() / 2;
											bulletPositionScreen.y -= tmpSurface.getHeight() / 2;
											drawHdSurfaceSetOrLegacy(surface, (_isTFTD ? "UnderwaterProjectiles" : "Projectiles"), projectileFrame, tmpSurface, bulletPositionScreen.x, bulletPositionScreen.y, 16, false, _nvColor);
										}

										// draw bullet itself
										voxelPos = _projectile->getPosition(1-i);
										if (voxelPos.x / 16 == itX &&
											voxelPos.y / 16 == itY &&
											voxelPos.z / 24 == itZ &&
											_save->getTileEngine()->isVoxelVisible(voxelPos))
										{
											_camera->convertVoxelToScreen(voxelPos, &bulletPositionScreen);
											bulletPositionScreen.x -= tmpSurface.getWidth() / 2;
											bulletPositionScreen.y -= tmpSurface.getHeight() / 2;
											drawHdSurfaceSetOrLegacy(surface, (_isTFTD ? "UnderwaterProjectiles" : "Projectiles"), projectileFrame, tmpSurface, bulletPositionScreen.x, bulletPositionScreen.y, 0, false, _nvColor);
										}
									}
								}
							}
						}
					}

					//draw particle clouds
					int pixelMaskArray[] = { 0, 2, 1, 3 };
					SurfaceRaw<int> pixelMask(pixelMaskArray, 2, 2);
					const int vaporScreenOriginX = screenPosition.x + _spriteWidth / 2;
					const int vaporScreenOriginY = screenPosition.y + _spriteHeight - _spriteWidth / 2 + tile->getPosition().toVoxel().z * mapGraphicsScale;
					const Uint8* const transparetPtr = _transparencies->data();

					//draw particle clouds behind solder
					for (const Particle& p : getVaporParticle(tile, 0))
					{
						int vaporX = vaporScreenOriginX + p.getOffsetX() * mapGraphicsScale;
						int vaporY = vaporScreenOriginY + p.getOffsetY() * mapGraphicsScale;
						auto transparetOffsets = transparetPtr
							+ (p.getColor() * Mod::TransparenciesOpacityLevels * Mod::TransparenciesPaletteColors)
							+ (p.getOpacity() * Mod::TransparenciesPaletteColors);

						GraphSubset vaporArea(2, 2); vaporArea = vaporArea.offset(vaporX, vaporY);
						auto vaporBefore = snapshotArea(vaporArea); const unsigned vaporOrder = nextDrawSequence();
						ShaderDrawFunc(
							[&](Uint8& dest, int size)
							{
								if (p.getSize() <= size)
								{
									dest = transparetOffsets[dest];
								}
							},
							ShaderSurface(this),
							ShaderMove(pixelMask, vaporX, vaporY)
						);
						markChangedDrawOrder(vaporBefore, vaporArea, vaporOrder);
					}

					unit = tile->getUnit();
					// Draw soldier from this tile, below or above
					drawUnit(unitSprite, tile, tile, screenPosition, topLayer, isUnitMovingNearby ? movingUnit : nullptr);

					if (isUnitMovingNearby)
					{
						// special handling for a moving unit in foreground of tile.
						constexpr static Position frontPos[] =
						{
							Position(-1, +1, 0),
							Position(0, +1, 0),
							Position(+1, +1, 0),
							Position(+1, 0, 0),
							Position(+1, -1, 0),
						};

						for (size_t f = 0; f < std::size(frontPos); ++f)
						{
							drawUnit(unitSprite, _save->getTile(mapPosition + frontPos[f]), tile, screenPosition, topLayer);
						}
					}

					// Draw smoke/fire
					if (tile->getSmoke() && tile->isDiscovered(O_FLOOR))
					{
						frameNumber = 0;
						int shade = 0;
						if (!tile->getFire())
						{
							if (_save->getDepth() > 0)
							{
								frameNumber += Mod::UNDERWATER_SMOKE_OFFSET;
							}
							else
							{
								frameNumber += Mod::SMOKE_OFFSET;
							}
							if (Mod::EXTENDED_SMOKE_OFFSET == 0)
							{
								frameNumber += int(floor((tile->getSmoke() / 6.0) - 0.1)); // see http://www.ufopaedia.org/images/c/cb/Smoke.gif
							}
							else if (Mod::EXTENDED_SMOKE_OFFSET == 1)
							{
								frameNumber += int(floor((tile->getSmoke() / 6.0) - 0.1)) * 4;
							}
							else // if (Mod::EXTENDED_SMOKE_OFFSET == 2)
							{
								frameNumber += (tile->getSmoke() - 1) / 5 * 4;
							}
							shade = tileShade;
						}

						if (halfAnimFrame + tile->getAnimationOffset() > 3)
						{
							frameNumber += halfAnimFrame + tile->getAnimationOffset() - 4;
						}
						else
						{
							frameNumber += halfAnimFrame + tile->getAnimationOffset();
						}
						tmpSurface = _game->getMod()->getSurfaceSet("SMOKE.PCK")->getFrame(frameNumber);
						drawHdSurfaceSetOrLegacy(surface, "SMOKE.PCK", frameNumber, tmpSurface, screenPosition.x, screenPosition.y, shade, false, _nvColor);
					}

					//draw particle clouds on front of solder
					for (const Particle& p : getVaporParticle(tile, topLayer ? 3 : 1))
					{
						int vaporX = vaporScreenOriginX + p.getOffsetX() * mapGraphicsScale;
						int vaporY = vaporScreenOriginY + p.getOffsetY() * mapGraphicsScale;
						auto transparetOffsets = transparetPtr
							+ (p.getColor() * Mod::TransparenciesOpacityLevels * Mod::TransparenciesPaletteColors)
							+ (p.getOpacity() * Mod::TransparenciesPaletteColors);

						GraphSubset vaporArea(2, 2); vaporArea = vaporArea.offset(vaporX, vaporY);
						auto vaporBefore = snapshotArea(vaporArea); const unsigned vaporOrder = nextDrawSequence();
						ShaderDrawFunc(
							[&](Uint8& dest, int size)
							{
								if (p.getSize() <= size)
								{
									dest = transparetOffsets[dest];
								}
							},
							ShaderSurface(this),
							ShaderMove(pixelMask, vaporX, vaporY)
						);
						markChangedDrawOrder(vaporBefore, vaporArea, vaporOrder);
					}

					// Draw Path Preview
					if (_previewSettingArrows && tile->getPreview() != -1 && tile->isDiscovered(O_FLOOR))
					{
						if (itZ > 0 && tile->hasNoFloor(_save))
						{
							tmpSurface = _game->getMod()->getSurfaceSet("Pathfinding")->getFrame(11);
							if (tmpSurface)
							{
								drawHdSurfaceSetOrLegacy(surface, "Pathfinding", 11, tmpSurface, screenPosition.x, screenPosition.y + 2 * mapGraphicsScale, 0, false, tile->getMarkerColor());
							}
						}
						tmpSurface = _game->getMod()->getSurfaceSet("Pathfinding")->getFrame(tile->getPreview());
						if (tmpSurface)
						{
							drawHdSurfaceSetOrLegacy(surface, "Pathfinding", tile->getPreview(), tmpSurface, screenPosition.x, screenPosition.y + tile->getTerrainLevel() * mapGraphicsScale, 0, false, tileColor);
						}
					}

					{
						// Draw object in front of units when requested by the MCD.
						tmpSurface = tile->getSprite(O_OBJECT);
						if ((tmpSurface || findHdTerrainVisualRule(tile, O_OBJECT)) && !tile->isBackTileObject(O_OBJECT))
						{
							const int objectShade = tile->getObstacle(O_OBJECT) ? obstacleShade : tileShade;
							const int hx = screenPosition.x;
							const int hy = screenPosition.y - tile->getYOffset(O_OBJECT) * mapGraphicsScale;
							const unsigned hdOrder = nextDrawSequence();
							if (!queueHdTerrainOrConvention(tile, O_OBJECT, hx, hy, objectShade, false, hdOrder))
							{
								if (tmpSurface)
								{
									Surface::blitRaw(surface, tmpSurface, hx, hy, objectShade, false, _nvColor);
									markSourceDrawOrder(tmpSurface, hx, hy, false, hdOrder);
								}
							}
						}
					}
					// Draw cursor front
					if (_cursorType != CT_NONE && _selectorX > itX - _cursorSize && _selectorY > itY - _cursorSize && _selectorX < itX+1 && _selectorY < itY+1 && !_save->getBattleState()->getMouseOverIcons())
					{
						if (_camera->getViewLevel() == itZ)
						{
							if (_cursorType != CT_AIM)
							{
								if (unit && (unit->getVisible() || _save->getDebugMode()))
									frameNumber = 3 + halfAnimFrameRest; // yellow box
								else
									frameNumber = 3; // red box
							}
							else
							{
								if (unit && (unit->getVisible() || _save->getDebugMode()))
									frameNumber = 7 + halfAnimFrame; // yellow animated crosshairs
								else
									frameNumber = 6; // red static crosshairs
							}
							tmpSurface = _game->getMod()->getSurfaceSet("CURSOR.PCK")->getFrame(frameNumber);
							drawHdSurfaceSetOrLegacy(surface, "CURSOR.PCK", frameNumber, tmpSurface, screenPosition.x, screenPosition.y, 0);

							// UFO extender accuracy: display adjusted accuracy value on crosshair in real-time.
							if (_cursorType >= CT_AIM && _showInfoOnCursor && (_cursorType != CT_THROW || !Options::oxceDisableInfoOnThrowCursor))
							{
								BattleAction *action = _save->getBattleGame()->getCurrentAction();
								const RuleItem *weapon = action->weapon->getRules();
								std::ostringstream ss;
								BattleActionAttack attack = BattleActionAttack::GetBeforeShoot(*action);
								int distanceSq = action->actor->distance3dToPositionSq(Position(itX, itY,itZ));
								int distance = (int)std::ceil(sqrt(float(distanceSq)));

								if (_cursorType == CT_AIM || _cursorType == CT_THROW)
								{
									int accuracy = BattleUnit::getFiringAccuracy(attack, _game->getMod());

									{
										int upperLimit, lowerLimit;
										int dropoff = weapon->calculateLimits(upperLimit, lowerLimit, _save->getDepth(), action->type);

										// at this point, let's assume the shot is adjusted and set the text amber.
										_txtAccuracy->setColor(Palette::blockOffset(Pathfinding::yellow - 1) - 1);

										if (distance > upperLimit)
										{
											accuracy -= (distance - upperLimit) * dropoff;
										}
										else if (distance < lowerLimit)
										{
											accuracy -= (lowerLimit - distance) * dropoff;
										}
										else
										{
											// no adjustment made? set it to green.
											_txtAccuracy->setColor(Palette::blockOffset(Pathfinding::green - 1) - 1);
										}
									}

									// Include LOS penalty for tiles in the unit's current view range
									// Don't recalculate LOS for outside of the current FOV
									int noLOSAccuracyPenalty = action->weapon->getRules()->getNoLOSAccuracyPenalty(_game->getMod());
									if (noLOSAccuracyPenalty != -1)
									{
										bool hasLOS = false;
										if (Position(itX, itY, itZ) == _cacheCursorPosition && _isCtrlPressed == _cacheIsCtrlPressed && _cacheHasLOS != -1)
										{
											// use cached result
											hasLOS = (_cacheHasLOS == 1);
										}
										else
										{
											// recalculate
											if (unit && (unit->getVisible() || _save->getDebugMode()))
											{
												hasLOS = _save->getTileEngine()->visible(action->actor, tile);
											}
											else
											{
												hasLOS = _save->getTileEngine()->isTileInLOS(action, tile, true);
											}
											// remember
											_cacheIsCtrlPressed = _isCtrlPressed;
											_cacheCursorPosition = Position(itX, itY, itZ);
											_cacheHasLOS = hasLOS ? 1 : 0;
										}

										if (!hasLOS)
										{
											accuracy = accuracy * noLOSAccuracyPenalty / 100;
											_txtAccuracy->setColor(Palette::blockOffset(Pathfinding::yellow - 1) - 1);
										}
									}

									bool outOfRange = action->type == BA_THROW
										? weapon->isOutOfThrowRange(distanceSq, _save->getDepth())
										: weapon->isOutOfRange(distanceSq);

									// zero accuracy or out of range: set it red.
									if (accuracy <= 0 || outOfRange)
									{
										accuracy = 0;
										_txtAccuracy->setColor(Palette::blockOffset(Pathfinding::red - 1) - 1);
									}
									ss << accuracy;
									ss << "%";
								}

								//TODO: merge this code with `InventoryState::calculateCurrentDamageTooltip` as 90% is same or should be same
								// display additional damage and psi-effectiveness info
								if (_isAltPressed)
								{
									// step 1: determine rule
									const RuleItem *rule;
									if (weapon->getBattleType() == BT_PSIAMP)
									{
										rule = weapon;
									}
									else if (action->weapon->needsAmmoForAction(action->type))
									{
										auto* ammo = attack.damage_item;
										if (ammo != nullptr)
										{
											rule = ammo->getRules();
										}
										else
										{
											rule = 0; // empty weapon = no rule
										}
									}
									else
									{
										rule = weapon;
									}

									// step 2: check if unlocked
									if (_cacheActiveWeaponUfopediaArticleUnlocked == -1)
									{
										_cacheActiveWeaponUfopediaArticleUnlocked = 0;
										if (_game->getSavedGame()->getMonthsPassed() == -1)
										{
											_cacheActiveWeaponUfopediaArticleUnlocked = 1; // new battle mode
										}
										else if (rule)
										{
											_cacheActiveWeaponUfopediaArticleUnlocked = 1; // assume unlocked
											ArticleDefinition *article = _game->getMod()->getUfopaediaArticle(rule->getType(), false);
											if (article && !Ufopaedia::isArticleAvailable(_game->getSavedGame(), article))
											{
												_cacheActiveWeaponUfopediaArticleUnlocked = 0; // ammo/weapon locked
											}
											if (rule->getType() != weapon->getType())
											{
												article = _game->getMod()->getUfopaediaArticle(weapon->getType(), false);
												if (article && !Ufopaedia::isArticleAvailable(_game->getSavedGame(), article))
												{
													_cacheActiveWeaponUfopediaArticleUnlocked = 0; // weapon locked
												}
											}
										}
									}

									// step 3: calculate and draw
									if (rule && _cacheActiveWeaponUfopediaArticleUnlocked == 1)
									{
										if (rule->getBattleType() == BT_PSIAMP)
										{
											float attackStrength = BattleUnit::getPsiAccuracy(attack);
											float defenseStrength = 30.0f; // indicator ignores: +victim->getArmor()->getPsiDefence(victim);

											float dis = Position::distance(action->actor->getPosition().toVoxel(), Position(itX, itY, itZ).toVoxel());
											int min = attackStrength - defenseStrength - rule->getPsiAccuracyRangeReduction(dis);
											int max = min + 55;
											if (max <= 0)
											{
												ss << "0%";
											}
											else
											{
												ss << min << "-" << max << "%";
											}
										}
										if (rule->getBattleType() != BT_PSIAMP || action->type == BA_USE)
										{
											int totalDamage = 0;
											if (weapon->getIgnoreAmmoPower())
											{
												totalDamage += weapon->getPowerBonus(attack);
												totalDamage -= weapon->getPowerRangeReduction(distance * 16);
											}
											else
											{
												totalDamage += rule->getPowerBonus(attack);
												totalDamage -= rule->getPowerRangeReduction(distance * 16);
											}
											if (totalDamage < 0) totalDamage = 0;
											if (_cursorType != CT_WAYPOINT)
												ss << "\n";
											ss << rule->getDamageType()->getRandomDamage(totalDamage, 1);
											ss << "-";
											ss << rule->getDamageType()->getRandomDamage(totalDamage, 2);
											if (rule->getDamageType()->RandomType == DRT_UFO_WITH_TWO_DICE)
												ss << "*";
										}
									}
									else
									{
										ss << "\n?-?";
									}
								}

								_txtAccuracy->setText(ss.str());
								_txtAccuracy->draw();
								trackedBlitNShade(_txtAccuracy, surface, screenPosition.x, screenPosition.y, 0);
							}
						}
						else if (_camera->getViewLevel() > itZ)
						{
							frameNumber = 5; // blue box
							tmpSurface = _game->getMod()->getSurfaceSet("CURSOR.PCK")->getFrame(frameNumber);
							drawHdSurfaceSetOrLegacy(surface, "CURSOR.PCK", frameNumber, tmpSurface, screenPosition.x, screenPosition.y, 0);
						}
						if (!_isAltPressed && _cursorType > CT_AIM && _camera->getViewLevel() == itZ)
						{
							bool ignore = false;
							if (_cursorType == CT_PSI || _cursorType == CT_WAYPOINT)
							{
								BattleAction* action = _save->getBattleGame()->getCurrentAction();
								int distanceSq = action->actor->distance3dToPositionSq(Position(itX, itY, itZ));
								if (action->weapon->getRules()->isOutOfRange(distanceSq))
								{
									// weapon doesn't work at this distance, just draw a normal cursor with a red 0% hint text
									ignore = true;
									_txtAccuracy->setColor(Palette::blockOffset(Pathfinding::red - 1) - 1);
									_txtAccuracy->setText("0%");
									_txtAccuracy->draw();
									trackedBlitNShade(_txtAccuracy, surface, screenPosition.x, screenPosition.y, 0);
								}
							}
							if (!ignore)
							{
								int frame[6] = { 0, 0, 0, 11, 13, 15 };
								const int cursorFrame = frame[_cursorType] + (_animFrame / 4) % 2;
								tmpSurface = _game->getMod()->getSurfaceSet("CURSOR.PCK")->getFrame(cursorFrame);
								drawHdSurfaceSetOrLegacy(surface, "CURSOR.PCK", cursorFrame, tmpSurface, screenPosition.x, screenPosition.y, 0);
							}
						}
					}

					// Draw waypoints if any on this tile
					int waypid = 1;
					int waypXOff = 2 * mapGraphicsScale;
					int waypYOff = 2 * mapGraphicsScale;

					for (const auto& waypoint : _waypoints)
					{
						if (waypoint == mapPosition)
						{
							if (waypXOff == 2 * mapGraphicsScale && waypYOff == 2 * mapGraphicsScale)
							{
								tmpSurface = _game->getMod()->getSurfaceSet("CURSOR.PCK")->getFrame(7);
								drawHdSurfaceSetOrLegacy(surface, "CURSOR.PCK", 7, tmpSurface, screenPosition.x, screenPosition.y, 0);
							}
							if (_save->getBattleGame()->getCurrentAction()->type == BA_LAUNCH || _save->getBattleGame()->getCurrentAction()->sprayTargeting)
							{
								_numWaypid->setValue(waypid);
								_numWaypid->setBordered(true); // OXCE, not configurable
								_numWaypid->draw();
								trackedBlitNShade(_numWaypid, surface, screenPosition.x + waypXOff, screenPosition.y + waypYOff, 0);

								waypXOff += (waypid > 9 ? 10 : 6) * mapGraphicsScale; // OXCE
								if (waypXOff >= 26 * mapGraphicsScale)
								{
									waypXOff = 2 * mapGraphicsScale;
									waypYOff += 8 * mapGraphicsScale;
								}
							}
						}
						waypid++;
					}
				}
			}
		}
	}
	if (pathfinderTurnedOn)
	{
		if (_numWaypid)
		{
			_numWaypid->setBordered(true); // give it a border for the pathfinding display, makes it more visible on snow, etc.
		}
		for (int itZ = beginZ; itZ <= endZ; itZ++)
		{
			for (int itX = beginX; itX <= endX; itX++)
			{
				for (int itY = beginY; itY <= endY; itY++)
				{
					mapPosition = Position(itX, itY, itZ);
					_camera->convertMapToScreen(mapPosition, &screenPosition);
					screenPosition += _camera->getMapOffset();

					// only render cells that are inside the surface
					if (screenPosition.x > -_spriteWidth && screenPosition.x < surface->getWidth() + _spriteWidth &&
						screenPosition.y > -_spriteHeight && screenPosition.y < surface->getHeight() + _spriteHeight )
					{
						tile = _save->getTile(mapPosition);
						if (!tile || !tile->isDiscovered(O_FLOOR) || tile->getPreview() == -1)
							continue;
						int adjustment = -tile->getTerrainLevel() * mapGraphicsScale;
						if (_previewSettingArrows)
						{
							if (itZ > 0 && tile->hasNoFloor(_save))
							{
								tmpSurface = _game->getMod()->getSurfaceSet("Pathfinding")->getFrame(23);
								if (tmpSurface)
								{
									drawHdSurfaceSetOrLegacy(surface, "Pathfinding", 23, tmpSurface, screenPosition.x, screenPosition.y + 2 * mapGraphicsScale, 0, false, tile->getMarkerColor());
								}
							}
							int overlay = tile->getPreview() + 12;
							tmpSurface = _game->getMod()->getSurfaceSet("Pathfinding")->getFrame(overlay);
							if (tmpSurface)
							{
								drawHdSurfaceSetOrLegacy(surface, "Pathfinding", overlay, tmpSurface, screenPosition.x, screenPosition.y - adjustment, 0, false, tile->getMarkerColor());
							}
						}

						if ((_previewSettingTu || _previewSettingEnergy) && (tile->getTUMarker() > -1 || tile->getEnergyMarker() > -1))
						{
							int off = tile->getTUMarker() > 9 ? 5 : 3;
							int offE = tile->getEnergyMarker() > 9 ? 5 : 3;
							int mcolor = _previewSettingArrows ? 0 : tile->getMarkerColor();
							if (_previewSettingArrows)
							{
								adjustment += 7;
							}
							if (_save->getSelectedUnit() && _save->getSelectedUnit()->isBigUnit())
							{
								adjustment += 1;
								if (!_previewSettingArrows)
								{
									adjustment += 7;
								}
							}
							if (_previewSettingTu)
							{
								_numWaypid->setValue(tile->getTUMarker());
								_numWaypid->draw();
								if (_previewSettingEnergy)
								{
									// TU
									trackedBlitNShade(_numWaypid, surface, screenPosition.x + 16 * mapGraphicsScale - off, screenPosition.y + 22 * mapGraphicsScale - adjustment, 0, false, mcolor);
									// and Energy
									_numWaypid->setValue(tile->getEnergyMarker());
									_numWaypid->draw();
									trackedBlitNShade(_numWaypid, surface, screenPosition.x + 16 * mapGraphicsScale - offE, screenPosition.y + 29 * mapGraphicsScale - adjustment, 0, false, mcolor);
								}
								else
								{
									// only TU
									trackedBlitNShade(_numWaypid, surface, screenPosition.x + 16 * mapGraphicsScale - off, screenPosition.y + 29 * mapGraphicsScale - adjustment, 0, false, mcolor);
								}
							}
							else if (_previewSettingEnergy)
							{
								// only Energy
								_numWaypid->setValue(tile->getEnergyMarker());
								_numWaypid->draw();
								trackedBlitNShade(_numWaypid, surface, screenPosition.x + 16 * mapGraphicsScale - offE, screenPosition.y + 29 * mapGraphicsScale - adjustment, 0, false, mcolor);
							}
						}
					}
				}
			}
		}
		if (_numWaypid)
		{
			_numWaypid->setBordered(false); // make sure we remove the border in case it's being used for missile waypoints.
		}
	}

	auto* selectedUnit = _save->getSelectedUnit();
	if (selectedUnit && (_save->getSide() == FACTION_PLAYER || _save->getDebugMode()) && selectedUnit->getPosition().z <= _camera->getViewLevel())
	{
		_camera->convertMapToScreen(selectedUnit->getPosition(), &screenPosition);
		screenPosition += _camera->getMapOffset();
		Position offset = calculateWalkingOffset(selectedUnit).ScreenOffset;
		if (selectedUnit->isBigUnit())
		{
			offset.y += 4;
		}
		offset.y += Position::TileZ - (selectedUnit->getHeight() + selectedUnit->getFloatHeight());
		if (selectedUnit->isKneeled())
		{
			offset.y -= 2;
		}
		if (this->getCursorType() != CT_NONE)
		{
			trackedBlitNShade(_arrow, surface, screenPosition.x + offset.x + (_spriteWidth / 2) - (_arrow->getWidth() / 2), screenPosition.y + offset.y - _arrow->getHeight() + getArrowBobForFrame(_animFrame) * mapGraphicsScale, 0);
		}
	}

	// Draw motion scanner arrows
	if (_isAltPressed && _save->getSide() == FACTION_PLAYER && this->getCursorType() != CT_NONE)
	{
		for (auto* myUnit : *_save->getUnits())
		{
			bool motionScan = myUnit->getScannedTurn() == _save->getTurn() && myUnit->getFaction() != FACTION_PLAYER && !myUnit->isOut();
			bool customMarker = myUnit->getCustomMarker() > 0 && myUnit->getFaction() == FACTION_PLAYER && !myUnit->isOut();
			if (motionScan || customMarker)
			{
				Position temp = myUnit->getPosition();
				temp.z = _camera->getViewLevel();
				_camera->convertMapToScreen(temp, &screenPosition);
				screenPosition += _camera->getMapOffset();
				Position offset;
				//calculateWalkingOffset(myUnit, &offset);
				if (myUnit->isBigUnit())
				{
					offset.y += 4;
				}
				if (motionScan)
				{
					offset.y += Position::TileZ - /*myUnit->getHeight()*/ 21; // no spoilers
				}
				else if (customMarker)
				{
					offset.y += Position::TileZ - (myUnit->getHeight() + myUnit->getFloatHeight());
				}
				if (myUnit->isKneeled())
				{
					offset.y -= 2;
				}
				if (motionScan)
				{
					_arrow->blitNShade(
						surface,
						screenPosition.x + offset.x + (_spriteWidth / 2) - (_arrow->getWidth() / 2),
						screenPosition.y + offset.y - _arrow->getHeight() + getArrowBobForFrame(_animFrame) * mapGraphicsScale,
						0);
				}
				else if (customMarker)
				{
					Surface::blitRaw(
						surface,
						_arrow,
						screenPosition.x + offset.x + (_spriteWidth / 2) - (_arrow->getWidth() / 2),
						screenPosition.y + offset.y - _arrow->getHeight() + getArrowBobForFrame(_animFrame) * mapGraphicsScale,
						0,
						false,
						_isTFTD ? ArrowColorsTFTD[myUnit->getCustomMarker() % 4] : ArrowColorsUFO[myUnit->getCustomMarker() % 4]);
				}
			}
		}
	}
	delete _numWaypid;

	// Draw craft deployment preview arrows
	if (_isAltPressed && _save->isPreview() && this->getCursorType() != CT_NONE)
	{
		for (auto& pos : _save->getCraftTiles())
		{
			if (pos.z == _camera->getViewLevel())
			{
				_camera->convertMapToScreen(pos, &screenPosition);
				screenPosition += _camera->getMapOffset();
				screenPosition.y += 2 * mapGraphicsScale; // based on vanilla soldier standHeight
				_arrow->blitNShade(
					surface,
					screenPosition.x + (_spriteWidth / 2) - (_arrow->getWidth() / 2),
					screenPosition.y - _arrow->getHeight() + getArrowBobForFrame(_animFrame) * mapGraphicsScale,
					0);
			}
		}
	}

	// check if we got big explosions
	if (_explosionInFOV)
	{
		// big explosions cause the screen to flash as bright as possible before any explosions are actually drawn.
		// this causes everything to look like EGA for a single frame.
		if (_flashScreen)
		{
			for (int x = 0, y = 0; x < surface->getWidth() && y < surface->getHeight();)
			{
				Uint8 pixel = surface->getPixel(x, y);
				if (pixel)
				{
					pixel = (pixel & 0xF0) + 1; //avoid 0 pixel
					surface->setPixelIterative(&x, &y, pixel);
				}
			}
			_flashScreen = false;
		}
		else
		{
			for (const auto* explosion : _explosions)
			{
				_camera->convertVoxelToScreen(explosion->getPosition(), &bulletPositionScreen);
				if (explosion->isBig())
				{
					if (explosion->getCurrentFrame() >= 0)
					{
						tmpSurface = _game->getMod()->getSurfaceSet("X1.PCK")->getFrame(explosion->getCurrentFrame());
						drawHdSurfaceSetOrLegacy(surface, "X1.PCK", explosion->getCurrentFrame(), tmpSurface, bulletPositionScreen.x - (tmpSurface.getWidth() / 2), bulletPositionScreen.y - (tmpSurface.getHeight() / 2), 0, false, _nvColor);
					}
				}
				else if (explosion->isHit())
				{
					tmpSurface = _game->getMod()->getSurfaceSet("HIT.PCK")->getFrame(explosion->getCurrentFrame());
					drawHdSurfaceSetOrLegacy(surface, "HIT.PCK", explosion->getCurrentFrame(), tmpSurface, bulletPositionScreen.x - 15 * mapGraphicsScale, bulletPositionScreen.y - 25 * mapGraphicsScale, 0, false, _nvColor);
				}
				else
				{
					tmpSurface = _game->getMod()->getSurfaceSet("SMOKE.PCK")->getFrame(explosion->getCurrentFrame());
					drawHdSurfaceSetOrLegacy(surface, "SMOKE.PCK", explosion->getCurrentFrame(), tmpSurface, bulletPositionScreen.x - 15 * mapGraphicsScale, bulletPositionScreen.y - 15 * mapGraphicsScale, 0, false, _nvColor);
				}
			}
		}
	}

	surface->unlock();
}

/**
 * Handles mouse presses on the map.
 * @param action Pointer to an action.
 * @param state State that the action handlers belong to.
 */
void Map::mousePress(Action *action, State *state)
{
	InteractiveSurface::mousePress(action, state);
	_camera->mousePress(action, state);
}

/**
 * Handles mouse releases on the map.
 * @param action Pointer to an action.
 * @param state State that the action handlers belong to.
 */
void Map::mouseRelease(Action *action, State *state)
{
	InteractiveSurface::mouseRelease(action, state);
	_camera->mouseRelease(action, state);
}

/**
 * Handles keyboard presses on the map.
 * @param action Pointer to an action.
 * @param state State that the action handlers belong to.
 */
void Map::keyboardPress(Action *action, State *state)
{
	InteractiveSurface::keyboardPress(action, state);
	_camera->keyboardPress(action, state);
}

/**
 * Handles map vision toggle mode.
 */

void Map::enableNightVision()
{
	_nightVisionOn = true;
	_debugVisionMode = 0;
	persistToggles();
}

void Map::toggleNightVision()
{
	_nightVisionOn = !_nightVisionOn;
	_debugVisionMode = 0;
	persistToggles();
}

void Map::toggleDebugVisionMode()
{
	_debugVisionMode = (_debugVisionMode + 1) % 3;
	_nightVisionOn = false;
	persistToggles();
}

void Map::persistToggles()
{
	if (Options::oxceToggleNightVisionType == 2)
	{
		// persisted per campaign
		_game->getSavedGame()->setToggleNightVision(_nightVisionOn);
	}
	else if (Options::oxceToggleNightVisionType == 1)
	{
		// persisted per battle
		_save->setToggleNightVision(_nightVisionOn);
	}

	if (Options::oxceToggleBrightnessType == 2)
	{
		// persisted per campaign
		_game->getSavedGame()->setToggleBrightness(_debugVisionMode);
	}
	else if (Options::oxceToggleBrightnessType == 1)
	{
		// persisted per battle
		_save->setToggleBrightness(_debugVisionMode);
	}

	_save->setToggleBrightnessTemp(_debugVisionMode);
}

/**
 * Handles fade-in and fade-out shade modification
 * @param original tile/item/unit shade
 */

int Map::reShade(Tile *tile)
{
	// when modders just don't know where to stop...
	if (_debugVisionMode > 0)
	{
		if (_debugVisionMode == 1)
		{
			// Reaver's tests
			return tile->getShade() / 2;
		}
		// Meridian's debug helper
		return 0;
	}

	// no night vision
	if (_nvColor == 0)
	{
		return tile->getShade();
	}

	// already bright enough
	if ((tile->getShade() <= NIGHT_VISION_SHADE))
	{
		return tile->getShade();
	}

	// hybrid night vision (local)
	for (const auto* bu : *_save->getUnits())
	{
		if (bu->getFaction() == FACTION_PLAYER && !bu->isOut())
		{
			if (Position::distance2dSq(tile->getPosition(), bu->getPosition()) <= bu->getMaxViewDistanceAtDarkSquared())
			{
				return tile->getShade() > _fadeShade ? _fadeShade : tile->getShade();
			}
		}
	}

	// hybrid night vision (global)
	return std::min(+NIGHT_VISION_MAX_SHADE, tile->getShade());
}

/**
 * Handles keyboard releases on the map.
 * @param action Pointer to an action.
 * @param state State that the action handlers belong to.
 */
void Map::keyboardRelease(Action *action, State *state)
{
	InteractiveSurface::keyboardRelease(action, state);
	_camera->keyboardRelease(action, state);
}

/**
 * Handles mouse over events on the map.
 * @param action Pointer to an action.
 * @param state State that the action handlers belong to.
 */
void Map::mouseOver(Action *action, State *state)
{
	InteractiveSurface::mouseOver(action, state);
	_camera->mouseOver(action, state);
	_mouseX = (int)action->getAbsoluteXMouse();
	_mouseY = (int)action->getAbsoluteYMouse();
	setSelectorPosition(_mouseX, _mouseY);
}


/**
 * Sets the selector to a certain tile on the map.
 * @param mx mouse x position.
 * @param my mouse y position.
 */
void Map::setSelectorPosition(int mx, int my)
{
	int oldX = _selectorX, oldY = _selectorY;

	_camera->convertScreenToMap(mx, my + _spriteHeight/4, &_selectorX, &_selectorY);

	if (oldX != _selectorX || oldY != _selectorY)
	{
		invalidateHd(HDR_SELECTOR);
	}
}

/**
 * Handles animating tiles. 8 Frames per animation.
 * @param redraw Redraw the battlescape?
 */
void Map::animate(bool redraw)
{
	_save->nextAnimFrame();
	_animFrame = _save->getAnimFrame();

	// random ambient sounds
	{
		if (!_save->getAmbienceRandom().empty())
		{
			_save->decreaseCurrentAmbienceDelay();
			if (_save->getCurrentAmbienceDelay() <= 0)
			{
				_save->resetCurrentAmbienceDelay();
				_save->playRandomAmbientSound();
			}
		}
	}

	// animate tiles
	for (int i = 0; i < _save->getMapSizeXYZ(); ++i)
	{
		_save->getTile(i)->animate();
	}

	// animate vapor
	for (auto i : Collections::rangeValueLess(_vaporParticles.size()))
	{
		auto& v = _vaporParticles[i];
		int posX = i % _camera->getMapSizeX();
		int posY = i / _camera->getMapSizeX();

		Collections::removeIf(
			v,
			[&](Particle& p)
			{
				if (p.animate())
				{
					Position tileOffset = p.updateScreenPosition();
					if (tileOffset != Position(0,0,0))
					{
						addVaporParticle(Position(posX,posY,0) + tileOffset, p);
						return true;
					}
					return false;
				}
				else
				{
					return true;
				}
			}
		);
	}

	// init vapor vector
	for (auto i : Collections::rangeValueLess(_vaporParticlesInit.size()))
	{
		auto& vi = _vaporParticlesInit[i];
		auto& vDest = _vaporParticles[i];
		if (vi.empty())
		{
			continue;
		}

		if (vDest.empty())
		{
			vi.swap(vDest);
		}
		else
		{
			vDest.insert(std::begin(vDest), std::begin(vi), std::end(vi));
		}


		Collections::removeAll(vi);
	}

	for (auto& tilePar : _vaporParticles)
	{
		if (tilePar.empty())
		{
			Collections::removeAll(tilePar);
		}
		else
		{
			std::sort(std::begin(tilePar), std::end(tilePar), [](const Particle& a, const Particle& b){ return a.getLayerZ() < b.getLayerZ(); });
		}
	}

	// animate certain units (large flying units have a propulsion animation)
	for (auto* bu : *_save->getUnits())
	{
		const Position pos = bu->getPosition();

		// skip units that do not have position
		if (pos == TileEngine::invalid)
		{
			continue;
		}

		if (_save->getDepth() > 0)
		{
			bu->setFloorAbove(false);

			// make sure this unit isn't obscured by the floor above him, otherwise it looks weird.
			if (_camera->getViewLevel() > pos.z)
			{
				for (int z = std::min(_camera->getViewLevel(), _save->getMapSizeZ() - 1); z != pos.z; --z)
				{
					if (!_save->getTile(Position(pos.x, pos.y, z))->hasNoFloor(0))
					{
						bu->setFloorAbove(true);
						break;
					}
				}
			}
		}

		bu->breathe();
	}

	if (redraw) invalidateHd(HDR_ANIMATION);
}

/**
 * Draws the rectangle selector.
 * @param pos Pointer to a position.
 */
void Map::getSelectorPosition(Position *pos) const
{
	pos->x = _selectorX;
	pos->y = _selectorY;
	pos->z = _camera->getViewLevel();
}

/**
 * Calculates the offset of a soldier, when it is walking in the middle of 2 tiles.
 * @param unit Pointer to BattleUnit.
 * @param offset Pointer to the offset to return the calculation.
 */
UnitWalkingOffset Map::calculateWalkingOffset(const BattleUnit *unit) const
{
	UnitWalkingOffset result = { };

	int offsetX[8] = { 1, 1, 1, 0, -1, -1, -1, 0 };
	int offsetY[8] = { 1, 0, -1, -1, -1, 0, 1, 1 };
	int phase = unit->getWalkingPhase() + unit->getDiagonalWalkingPhase();
	int dir = unit->getDirection();
	int midphase = 4 + 4 * (dir % 2);
	int endphase = 8 + 8 * (dir % 2);
	int size = unit->getArmor()->getSize();

	result.ScreenOffset.x = 0;
	result.ScreenOffset.y = 0;

	if (size > 1)
	{
		if (dir < 1 || dir > 5)
			midphase = endphase;
		else if (dir == 5)
			midphase = 12;
		else if (dir == 1)
			midphase = 5;
		else
			midphase = 1;
	}
	if (unit->getVerticalDirection())
	{
		midphase = 4;
		endphase = 8;
	}
	else if ((unit->getStatus() == STATUS_WALKING || unit->getStatus() == STATUS_FLYING))
	{
		if (phase < midphase)
		{
			result.ScreenOffset.x = phase * 2 * offsetX[dir];
			result.ScreenOffset.y = - phase * offsetY[dir];
		}
		else
		{
			result.ScreenOffset.x = (phase - endphase) * 2 * offsetX[dir];
			result.ScreenOffset.y = - (phase - endphase) * offsetY[dir];
		}
	}

	result.NormalizedMovePhase = endphase == 16 ? phase : phase * 2;

	// If we are walking in between tiles, interpolate it's terrain level.
	if (unit->getStatus() == STATUS_WALKING || unit->getStatus() == STATUS_FLYING)
	{
		const Position posCurr = unit->getPosition();
		const Position posDest = unit->getDestination();
		const Position posLast = unit->getLastPosition();
		if (phase < midphase)
		{
			int fromLevel = getTerrainLevel(posCurr, size);
			int toLevel = getTerrainLevel(posDest, size);
			if (posCurr.z > posDest.z)
			{
				// going down a level, so toLevel 0 becomes +24, -8 becomes  16
				toLevel += Position::TileZ*(posCurr.z - posDest.z);
			}
			else if (posCurr.z < posDest.z)
			{
				// going up a level, so toLevel 0 becomes -24, -8 becomes -16
				toLevel = -Position::TileZ*(posDest.z - posCurr.z) + abs(toLevel);
			}
			result.TerrainLevelOffset = Interpolate(fromLevel, toLevel, phase, endphase);
		}
		else
		{
			// from phase 4 onwards the unit behind the scenes already is on the destination tile
			// we have to get it's last position to calculate the correct offset
			int fromLevel = getTerrainLevel(posLast, size);
			int toLevel = getTerrainLevel(posDest, size);
			if (posLast.z > posDest.z)
			{
				// going down a level, so fromLevel 0 becomes -24, -8 becomes -32
				fromLevel -= Position::TileZ*(posLast.z - posDest.z);
			}
			else if (posLast.z < posDest.z)
			{
				// going up a level, so fromLevel 0 becomes +24, -8 becomes 16
				fromLevel = Position::TileZ*(posDest.z - posLast.z) - abs(fromLevel);
			}
			result.TerrainLevelOffset = Interpolate(fromLevel, toLevel, phase, endphase);
		}
	}
	else
	{
		result.TerrainLevelOffset = getTerrainLevel(unit->getPosition(), size);
	}
	// Logical walking offsets are always expressed in the native 32x40 grid.
	const int mapGraphicsScale = 1;
	result.ScreenOffset.x *= mapGraphicsScale;
	result.ScreenOffset.y *= mapGraphicsScale;
	result.ScreenOffset.y += result.TerrainLevelOffset * mapGraphicsScale;
	return result;
}


/**
  * Terrainlevel goes from 0 to -24. For a larger sized unit, we need to pick the highest terrain level, which is the lowest number...
  * @param pos Position.
  * @param size Size of the unit we want to get the level from.
  * @return terrainlevel.
  */
int Map::getTerrainLevel(const Position& pos, int size) const
{
	int lowestlevel = 0;

	for (int x = 0; x < size; x++)
	{
		for (int y = 0; y < size; y++)
		{
			int l = _save->getTile(pos + Position(x,y,0))->getTerrainLevel();
			if (l < lowestlevel)
				lowestlevel = l;
		}
	}

	return lowestlevel;
}

/**
 * Sets the 3D cursor to selection/aim mode.
 * @param type Cursor type.
 * @param size Size of cursor.
 */
void Map::setCursorType(CursorType type, int size)
{
	// reset cursor indicator cache
	_cacheActiveWeaponUfopediaArticleUnlocked = -1;
	_cacheIsCtrlPressed = false;
	_cacheCursorPosition = TileEngine::invalid;
	_cacheHasLOS = -1;

	_cursorType = type;
	if (_cursorType == CT_NORMAL)
		_cursorSize = size;
	else
		_cursorSize = 1;
}

/**
 * Gets the cursor type.
 * @return cursor type.
 */
CursorType Map::getCursorType() const
{
	return _cursorType;
}

/**
 * Puts a projectile sprite on the map.
 * @param projectile Projectile to place.
 */
void Map::setProjectile(Projectile *projectile)
{
	_projectile = projectile;
	if (projectile && Options::battleSmoothCamera)
	{
		_launch = true;
	}
}

/**
 * Gets the current projectile sprite on the map.
 * @return Projectile or 0 if there is no projectile sprite on the map.
 */
Projectile *Map::getProjectile() const
{
	return _projectile;
}

/**
 * Add new vapor particle.
 * @param pos Tile position of particle.
 * @param particle Particle to add.
 */
void Map::addVaporParticle(Position pos, Particle particle)
{
	if ((int)(_transparencies->size()) < (particle.getColor() + 1) * Mod::TransparenciesOpacityLevels * Mod::TransparenciesPaletteColors)
	{
		return;
	}
	if (pos.x >= _camera->getMapSizeX() || pos.y >= _camera->getMapSizeY())
	{
		return;
	}
	if (pos.x < 0 || pos.y < 0)
	{
		return;
	}

	auto& v = _vaporParticlesInit[_camera->getMapSizeX() * pos.y + pos.x];

	// as there will usually be more than one Particle, we prepare more space
	if (v.capacity() < 64)
	{
		v.reserve(64);
	}

	v.push_back(particle);
}

/**
 * Get all vapor for tile.
 * @param tile current tile.
 * @param topLayer if tile is top visible layer, if true then will return particles belongs to upper tiles.
 * @return range of particles that should be drawn.
 */
Collections::Range<const Particle*> Map::getVaporParticle(const Tile* tile, int topLayer) const
{
	Position pos = tile->getPosition();
	auto& v = _vaporParticles[_camera->getMapSizeX() * pos.y + pos.x];
	int startZ = pos.z * Particle::LayerAccuracy + (topLayer & 1);
	int endZ = startZ + Particle::LayerAccuracy / 2;
	auto* s = std::partition_point(v.data(), v.data() + v.size(), [&](const Particle& a){ return a.getLayerZ() < startZ; });
	auto* e = (topLayer & 2) ? v.data() + v.size() : std::partition_point(s, v.data() + v.size(), [&](const Particle& a){ return a.getLayerZ() < endZ; });
	return Collections::Range{ s, e };
}

/**
 * Gets a list of explosion sprites on the map.
 * @return A list of explosion sprites.
 */
std::list<Explosion*> *Map::getExplosions()
{
	return &_explosions;
}

/**
 * Gets the pointer to the camera.
 * @return Pointer to camera.
 */
Camera *Map::getCamera()
{
	return _camera;
}

/**
 * Timers only work on surfaces so we have to pass this on to the camera object.
 */
void Map::scrollMouse()
{
	_camera->scrollMouse();
}

/**
 * Timers only work on surfaces so we have to pass this on to the camera object.
 */
void Map::scrollKey()
{
	_camera->scrollKey();
}

/**
 * Modify the fade shade level if fade's in progress.
 */
void Map::fadeShade()
{
	bool hold = SDL_GetKeyState(NULL)[Options::keyNightVisionHold];
	if ((_nightVisionOn && !hold) || (!_nightVisionOn && hold))
	{
		_nvColor = Options::oxceNightVisionColor;
		_save->setToggleNightVisionTemp(true);
		_save->setToggleNightVisionColorTemp(_nvColor);
		if (_fadeShade > NIGHT_VISION_SHADE) // 0 = max brightness
		{
			--_fadeShade;
		}
	}
	else
	{
		if (_nvColor != 0)
		{
			if (_fadeShade < _save->getGlobalShade())
			{
				// gradually fade away
				++_fadeShade;
			}
			else
			{
				// and at the end turn off night vision
				_nvColor = 0;
				_save->setToggleNightVisionTemp(false);
				_save->setToggleNightVisionColorTemp(0);
			}
		}
	}
}

/**
 * Gets a list of waypoints on the map.
 * @return A list of waypoints.
 */
std::vector<Position> *Map::getWaypoints()
{
	return &_waypoints;
}

/**
 * Sets mouse-buttons' pressed state.
 * @param button Index of the button.
 * @param pressed The state of the button.
 */
void Map::setButtonsPressed(Uint8 button, bool pressed)
{
	setButtonPressed(button, pressed);
}

/**
 * Sets the unitDying flag.
 * @param flag True if the unit is dying.
 */
void Map::setUnitDying(bool flag)
{
	_unitDying = flag;
}

/**
 * Updates the selector to the last-known mouse position.
 */
void Map::refreshSelectorPosition()
{
	setSelectorPosition(_mouseX, _mouseY);
}

void Map::setHudHidden(bool hidden)
{
	_visibleMapHeight = getHeight() - (hidden ? 0 : _iconHeight);
	if (_camera) _camera->setVisibleMapHeight(_visibleMapHeight);
	if (_message)
	{
		_message->setHeight((_visibleMapHeight < 200) ? _visibleMapHeight : 200);
		_message->setY((_visibleMapHeight - _message->getHeight()) / 2);
	}
}

/**
 * Special handling for setting the height of the map viewport.
 * @param height the new base screen height.
 */
void Map::setHeight(int height)
{
	invalidateHd(HDR_RESIZE);
	Surface::setHeight(height);
	_visibleMapHeight = height - _iconHeight;
	_message->setHeight((_visibleMapHeight < 200)? _visibleMapHeight : 200);
	_message->setY((_visibleMapHeight - _message->getHeight()) / 2);
}

/**
 * Special handling for setting the width of the map viewport.
 * @param width the new base screen width.
 */
void Map::setWidth(int width)
{
	int dX = width - getWidth();
	invalidateHd(HDR_RESIZE);
	Surface::setWidth(width);
	_message->setX(_message->getX() + dX / 2);
}

/**
 * Get the hidden movement screen's vertical position.
 * @return the vertical position of the hidden movement window.
 */
int Map::getMessageY() const
{
	return _message->getY();
}

/**
 * Get the icon height.
 */
int Map::getIconHeight() const
{
	return _iconHeight;
}

/**
 * Get the icon width.
 */
int Map::getIconWidth() const
{
	return _iconWidth;
}

/**
 * Returns the angle(left/right balance) of a sound effect,
 * based off a map position.
 * @param pos the map position to calculate the sound angle from.
 * @return the angle of the sound (280 to 440).
 */
int Map::getSoundAngle(const Position& pos) const
{
	int midPoint = getWidth() / 2;
	Position relativePosition;

	_camera->convertMapToScreen(pos, &relativePosition);
	// cap the position to the screen edges relative to the center,
	// negative values indicating a left-shift, and positive values shifting to the right.
	relativePosition.x = Clamp((relativePosition.x + _camera->getMapOffset().x) - midPoint, -midPoint, midPoint);

	// convert the relative distance to a relative increment of an 80 degree angle
	// we use +- 80 instead of +- 90, so as not to go ALL the way left or right
	// which would effectively mute the sound out of one speaker.
	// since Mix_SetPosition uses modulo 360, we can't feed it a negative number, so add 360 instead.
	return 360 + (relativePosition.x / (midPoint / 80.0));
}

/**
 * Reset the camera smoothing bool.
 */
void Map::resetCameraSmoothing()
{
	_smoothingEngaged = false;
}

/**
 * Set the "explosion flash" bool.
 * @param flash should the screen be rendered in EGA this frame?
 */
void Map::setBlastFlash(bool flash)
{
	_flashScreen = flash;

	// Meridian: no frikin flashing!!
	_flashScreen = false;
}

/**
 * Checks if the screen is still being rendered in EGA.
 * @return if we are still in EGA mode.
 */
bool Map::getBlastFlash() const
{
	return _flashScreen;
}

/**
 * Resets obstacle markers.
 */
void Map::resetObstacles(void)
{
	for (int z = 0; z < _save->getMapSizeZ(); z++)
		for (int y = 0; y < _save->getMapSizeY(); y++)
			for (int x = 0; x < _save->getMapSizeX(); x++)
			{
				Tile *tile = _save->getTile(Position(x, y, z));
				if (tile) tile->resetObstacle();
			}
	_showObstacles = false;
}

/**
 * Enables obstacle markers.
 */
void Map::enableObstacles(void)
{
	_showObstacles = true;
	if (_obstacleTimer)
	{
		_obstacleTimer->stop();
		_obstacleTimer->start();
	}
}

/**
 * Disables obstacle markers.
 */
void Map::disableObstacles(void)
{
	_showObstacles = false;
	if (_obstacleTimer)
	{
		_obstacleTimer->stop();
	}
}

}
