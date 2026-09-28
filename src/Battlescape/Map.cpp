#include "HdStoreyBands.h"
#include "HdCausticReceiver.h"
#include "../Engine/HdCausticSettings.h"
#include "../Engine/HdRenderTrace.h"
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
#include <cmath>
#include <iomanip>
#include <sstream>
#include <map>
#include <set>
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
#include "BedrockRender.h"
#include "RealHdVisibilitySolver.h"
#include "RealHdWorldPresentation.h"
#include "RealHdLightingAuthority.h"
#include "OxceWorldAdapter.h"
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
#include "../Engine/HdPngResolver.h"
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
#include "../Savegame/GameTime.h"
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

struct HdLocalLightRule
{
	int r = 255;
	int g = 255;
	int b = 255;
	int radiusMilliTiles = 2500;
	int intensityPermille = 700;
	int pulsePermille = 0;
	int periodFrames = 16;
	int phaseFrames = 0;
};

// Compatibility helper retained at Map call sites while ownership moves to the
// common RealHdWorldPresentation contract.  Renderer/effect code must not grow
// new local discovery gates.
bool hdVisibilitySourceDiscovered(const OpenXcom::Tile *tile, OpenXcom::TilePart part)
{
	return OpenXcom::RealHdWorldPresentation::allows(
		tile, part, OpenXcom::RealHdWorldPresentationClass::PersistentGeometry);
}

// OXCE CURSOR VISIBILITY / PRESENTATION GATES.
//
// V=hasVisibleTile remains useful as an audit signal, but it is tile-space OXCE
// visibility and is not precise enough to own Real HD cursor presentation.
// Cursor V4 therefore follows the same discovery authority as the Real HD
// source renderer, and applies one shared decision to both cursor halves.
bool hdTileVisibleNowToPlayer(OpenXcom::SavedBattleGame *save, OpenXcom::Tile *tile)
{
	if (!save || !tile) return false;
	for (OpenXcom::BattleUnit *observer : *save->getUnits())
	{
		if (!observer || observer->isOut() || observer->getFaction() != OpenXcom::FACTION_PLAYER) continue;
		if (observer->hasVisibleTile(tile)) return true;
	}
	return false;
}

bool hdCanPresentLowerCursor(OpenXcom::SavedBattleGame *save, int x, int y, int viewZ, int targetZ)
{
	if (!save || targetZ >= viewZ) return false;

	// Every opening crossed on the way down must itself be known.  Never call
	// hasNoFloor() on black/unknown geometry and turn that fact into UI feedback.
	for (int cursorZ = viewZ; cursorZ > targetZ; --cursorZ)
	{
		OpenXcom::Tile *columnTile = save->getTile(OpenXcom::Position(x, y, cursorZ));
		if (!columnTile || !hdVisibilitySourceDiscovered(columnTile, OpenXcom::O_FLOOR) || !columnTile->hasNoFloor(save))
			return false;
	}

	// V3 missed this final ownership test: the lower target itself must also be
	// known to the Real HD presentation.  This blocks a ghost on an undiscovered
	// intermediate Z even when OXCE's coarse visibleTiles says V=1.
	OpenXcom::Tile *targetTile = save->getTile(OpenXcom::Position(x, y, targetZ));
	return targetTile && hdVisibilitySourceDiscovered(targetTile, OpenXcom::O_FLOOR);
}

// HD PRESENTATION BLACK V1. Palette index 0 is transparent in OXCE, so an HD
// tactical background cannot simply be cleared with index 0. Pick an opaque exact
// black entry when available, otherwise the darkest opaque palette entry.
Uint8 hdOpaqueBlackPaletteIndex(const SDL_Color *palette)
{
	if (!palette) return 1;
	int best = 1;
	int bestLuma = 0x7fffffff;
	for (int i = 1; i < 256; ++i)
	{
		const int luma = (int)palette[i].r + (int)palette[i].g + (int)palette[i].b;
		if (luma < bestLuma)
		{
			best = i;
			bestLuma = luma;
			if (luma == 0) break;
		}
	}
	return (Uint8)best;
}

const std::map<std::string, std::map<int, HdLocalLightRule>> &hdLocalLightRules()
{
	static bool loaded = false;
	static std::map<std::string, std::map<int, HdLocalLightRule>> rules;
	if (loaded) return rules;
	loaded = true;

	const std::string filename = "Ruleset/TFTD_HD_local_lights.yml";
	if (!OpenXcom::FileMap::fileExists(filename)) return rules;

	try
	{
		const OpenXcom::YAML::YamlRootNodeReader root = OpenXcom::FileMap::getYAML(filename);
		const auto datasets = root["hdLocalLights"];
		for (const auto &datasetReader : datasets.children())
		{
			const std::string dataset = OpenXcom::hdLower(std::string(datasetReader.key()));
			if (dataset.empty()) continue;
			const auto frames = datasetReader["frames"];
			for (const auto &frameReader : frames.children())
			{
				try
				{
					const int frame = std::stoi(std::string(frameReader.key()));
					HdLocalLightRule rule;
					frameReader.tryRead("r", rule.r);
					frameReader.tryRead("g", rule.g);
					frameReader.tryRead("b", rule.b);
					frameReader.tryRead("radiusMilliTiles", rule.radiusMilliTiles);
					frameReader.tryRead("intensityPermille", rule.intensityPermille);
					frameReader.tryRead("pulsePermille", rule.pulsePermille);
					frameReader.tryRead("periodFrames", rule.periodFrames);
					frameReader.tryRead("phaseFrames", rule.phaseFrames);
					rule.r = std::max(0, std::min(255, rule.r));
					rule.g = std::max(0, std::min(255, rule.g));
					rule.b = std::max(0, std::min(255, rule.b));
					rule.radiusMilliTiles = std::max(100, rule.radiusMilliTiles);
					rule.intensityPermille = std::max(0, rule.intensityPermille);
					rule.pulsePermille = std::max(0, std::min(1000, rule.pulsePermille));
					rule.periodFrames = std::max(1, rule.periodFrames);
					rules[dataset][frame] = rule;
				}
				catch (...)
				{
					// Ignore malformed frame keys; local-light presentation is optional.
				}
			}
		}
		OpenXcom::Logger().get(OpenXcom::LOG_INFO) << "[HD LOCAL LIGHTS V1] loaded " << rules.size()
			<< " dataset rule groups from " << filename;
	}
	catch (const std::exception &e)
	{
		OpenXcom::Logger().get(OpenXcom::LOG_WARNING) << "[HD LOCAL LIGHTS V1] failed to read " << filename << ": " << e.what();
	}
	return rules;
}


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

OpenXcom::HdEnvironmentProfile hdBedrockEnvironmentProfileForMaterial(const std::string &materialProfile)
{
	const std::string profile = OpenXcom::hdLower(materialProfile);
	if (profile == "coral_bubble" || profile == "coralbubble")
		return OpenXcom::hdEnvironmentProfileFromBlockMask(0x8000u); // Legacy F0 cyan block.

	return OpenXcom::hdEnvironmentProfileFromBlockMask(OpenXcom::hdTerrainDatasetBlockMask(profile));
}

/**
 * DEPTH LUMINANCE V1.
 *
 * This is intentionally NOT part of the colour LUT and NOT a caustic pattern.
 * It is a single mission-depth luminance multiplier for BEDROCK only, so the
 * visual contribution can be A/B tested independently with Ctrl+F7.
 * Depth 0 is surface/land; TFTD underwater depths are 1..3.  Shallow water
 * receives the strongest lift and the contribution tapers with depth.
 */
int hdDepthLuminancePermilleForDepth(int depth)
{
	// BEDROCK CALIBRATION UI V2: D1 and D3 are raw luminance multipliers
	// expressed in permille only for storage precision. The UI exposes the actual
	// renderer values (for example 1.180 and 1.060), never percentages.
	const int shallow = std::max(0, OpenXcom::Options::hdBedrockLumaD1Permille);
	const int deep = std::max(0, OpenXcom::Options::hdBedrockLumaD3Permille);
	if (depth <= 0) return 1000;
	if (depth <= 1) return shallow;
	if (depth >= 3) return deep;
	return (shallow + deep + 1) / 2;
}

void hdApplyMaterialGrade(Uint8 &r, Uint8 &g, Uint8 &b, const OpenXcom::HdMaterialGradeParams &p)
{
	float rf = r / 255.0f, gf = g / 255.0f, bf = b / 255.0f;
	rf *= OpenXcom::hdMaterialGradeFloat(p.tintR);
	gf *= OpenXcom::hdMaterialGradeFloat(p.tintG);
	bf *= OpenXcom::hdMaterialGradeFloat(p.tintB);
	const float exposure = OpenXcom::hdMaterialGradeFloat(p.exposure);
	rf *= exposure; gf *= exposure; bf *= exposure;
	const float contrast = OpenXcom::hdMaterialGradeFloat(p.contrast);
	rf = (rf - 0.5f) * contrast + 0.5f;
	gf = (gf - 0.5f) * contrast + 0.5f;
	bf = (bf - 0.5f) * contrast + 0.5f;
	const float saturation = OpenXcom::hdMaterialGradeFloat(p.saturation);
	const float luma = rf * 0.2126f + gf * 0.7152f + bf * 0.0722f;
	rf = luma + (rf - luma) * saturation;
	gf = luma + (gf - luma) * saturation;
	bf = luma + (bf - luma) * saturation;
	auto toByte = [](float v) -> Uint8 { return (Uint8)std::lround(std::max(0.0f, std::min(1.0f, v)) * 255.0f); };
	r = toByte(rf); g = toByte(gf); b = toByte(bf);
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
 *   Resources/TFTD_HD/RealHD/Datasets/<DATASET>/ -> Environment (canonical REAL/DEBUG provider namespace)
 *   Resources/TFTD_HD/Terrain/...                -> Environment (legacy authored-HD compatibility)
 *   Resources/TFTD_HD/LegacyIndexed/Terrain/...  -> IndexedLegacy (exact TFTD indices)
 *   Resources/TFTD_HD/Fixed/Terrain/...          -> Fixed (legacy diagnostic/UI-like colour)
 *
 * TFTD_REAL_HD_TEXTURES and TFTD_REAL_HD_DEBUG deliberately expose the same
 * RealHD virtual paths. FileMap/VFS priority selects the provider; renderer code
 * must never branch on either mod id.
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
	_projectile(0), _followProjectile(true), _projectileInFOV(false), _explosionInFOV(false), _launch(false), _visibleMapHeight(visibleMapHeight), _hudVisibleMapHeightOverride(-1),
	_unitDying(false), _smoothingEngaged(false), _flashScreen(false), _bgColor(15), _projectileSet(0),
	_hdBedrockCraterFieldW(0), _hdBedrockCraterFieldH(0), _hdBedrockCraterFieldSourceRevision(~0ULL), _hdBedrockCraterFieldUploadRevision(0), _hdBedrockWeaponFieldW(0), _hdBedrockWeaponFieldH(0), _hdBedrockWeaponFieldUploadRevision(0),
	_remasterWorldStateBuiltRevision(~0ULL), _drawSequence(0), _hdLegacyBridgeRanges(0), _hdLegacyGpuCompatCommands(0), _hdLastRealTick(SDL_GetTicks()),
	_hdPhysicalRevision(0), _hdPhysicalCachedRevision(~0ULL), _hdPhysicalCurrentCacheIndex(-1), _hdPhysicalUseCounter(0), _hdPhysicalCacheMaxEntries(0),
	_hdPhysicalCacheX(0), _hdPhysicalCacheY(0), _hdPhysicalCacheW(0), _hdPhysicalCacheH(0),
	_hdPhysicalCacheScaleX(0.0), _hdPhysicalCacheScaleY(0.0), _hdPhysicalLastMs(0), _hdPhysicalPixelsTested(0), _hdPhysicalPixelsWritten(0), _hdPhysicalLastCacheHit(false), _hdPhysicalMapSuppressed(false),
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
	_smoothCamera = Options::battleSmoothCamera || Options::hdGraphics; // REAL HD: never fall back to whole-screen projectile jumps.
	if (Options::traceAI)
	{
		// turn everything on because we want to see the markers.
		previewSetting = PATH_ARROW_TU;
	}
	_previewSettingArrows = previewSetting & PATH_ARROWS;
	_previewSettingTu     = previewSetting & PATH_TU_COST;
	_previewSettingEnergy = previewSetting & PATH_ENERGY_COST;

	_save = _game->getSavedGame()->getSavedBattle();
	Log(LOG_INFO) << "[OXCE CURSOR PRESENTATION GATE V4] lowerLevelGhost=REAL_HD_PRESENTATION_AUTHORITY cursorHalves=UNIFIED coarseVisibleTiles=AUDIT_ONLY discoveryGateV2=RETAINED";
	Log(LOG_INFO) << "[REAL HD DATASET PROVIDER V1] canonicalRoot=Resources/TFTD_HD/RealHD/Datasets/<DATASET>/"
		<< " providerSelection=VFS_PRIORITY modNameDependency=NONE"
		<< " legacyTerrainFallback=ON legacyBedrockFallback=ON";
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
	// Lighting must be current before the static REAL HD producer resolves
	// shades. A camera-only redraw retains the previous light field.
	const unsigned lightingRelevantMask =
		(1u << (unsigned)HDR_EXTERNAL) |
		(1u << (unsigned)HDR_ANIMATION) |
		(1u << (unsigned)HDR_STATE_QUEUE) |
		(1u << (unsigned)HDR_ATTACK);
	if (_realHdLightingRevision == 0 || (_hdRedrawLastMask & lightingRelevantMask) != 0)
		++_realHdLightingRevision;

	// OXCE historically clears the tactical surface with the ruleset background colour
	// (TFTD surface missions use the familiar dark blue). REAL-HD deliberately uses an
	// opaque black tactical backdrop instead: it matches unexplored space, removes the
	// blue sliver at map edges and gives the source-owned REAL-HD FOV one coherent unexplored backdrop.
	// Classic/non-HD rendering keeps the original ruleset background unchanged.
	_redraw = false;
	const Uint8 hdBackground = Options::hdGraphics
		? hdOpaqueBlackPaletteIndex(getPalette())
		: (Uint8)(Palette::blockOffset(0) + _bgColor);
	ShaderDrawFunc(
		[](Uint8& dest, Uint8 color)
		{
			dest = color;
		},
		ShaderSurface(this),
		ShaderScalar<Uint8>(hdBackground)
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
		_hdPhysicalMapSuppressed = false;
		if (Options::hdGraphics)
		{
			updateHdProjectileCamera();
			// Authority-first chantier: camera changes never call drawTerrain or
			// rebuild the static world. Geometry/animation changes recapture it.
			const unsigned sceneReasons = (1u << HDR_EXTERNAL) |
				(1u << HDR_ANIMATION) | (1u << HDR_STATE_QUEUE) |
				(1u << HDR_ATTACK);
			if (!_hdStaticSceneCommandsReady || (_hdRedrawLastMask & sceneReasons))
				rebuildHdStaticSceneCommands();
			_hdDrawCommands.resize(_hdStaticCommandCount);
			appendHdDynamicUnitCommands();
			appendHdCombatEffectsCommands();
			appendHdTacticalIndicators();
			appendHdCursorCommand();
			static bool reportedP2D = false;
			if (!reportedP2D)
			{
				Log(LOG_INFO) << "[REAL_HD_REWIRE_P2F][ACTIVE] units=WHOLE_VIEWPORT_CLIP projectiles=STATE explosions=STATE groundItems=STATE smokeFire=STATE vapor=RGBA_TINT indicators=HD_UI cursor=KNOWN_SURFACE_OR_NEUTRAL impactRelief=PARALLAX_WORLD_GRADIENT underwaterCloud=HD drawTerrain=OFF";
				reportedP2D = true;
			}
		}
		else drawTerrain(this);
	}
	else
	{
		_hdPhysicalMapSuppressed = true;
		// Hidden-movement/end-turn screens replace the tactical map entirely.
		// HD assets are composited after the legacy map surface, so commands
		// cached from the previous visible tactical frame must not survive here.
		// Otherwise a full-body RGBA unit would appear as a ghost over the
		// hidden-movement background even though legacy units are correctly hidden.
		_hdDrawCommands.clear();
		_hdStaticSceneCommandsReady = false;
		_hdStaticCommandCount = 0;
		_drawOrderBuffer.clear();
		_drawSequence = 0;
		if (!Options::hdGraphics) _message->blit(this->getSurface());
	}

	// Map::draw() is OXCE's authoritative scene invalidation point. Every
	// successful redraw gets one physical revision. PERF FOUNDATION V1 keeps
	// lighting on a narrower revision: camera/selector/resize are presentation
	// changes and must not trigger a LOS-blocked helmet-light rebuild.
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
		if (const_cast<Map*>(this)->_hdImageCache.usable(path)) return path;
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
	if (Options::hdGraphics && source == _arrow)
	{
		// Semantic marker -> HD asset command. Never read the native raster on success.
		drawHdSurfaceSetOrLegacy(destination, "SelectionArrow", 0, _arrow,
			x, y, shade, half, newBaseColor, true);
		return;
	}
	GraphSubset area(source->getWidth(), source->getHeight());
	area = area.offset(x, y);
	const std::vector<Uint8> before = snapshotArea(area);
	const unsigned order = nextDrawSequence();
	source->blitNShade(destination, x, y, shade, half, newBaseColor);
	markChangedDrawOrder(before, area, order);
}

void Map::rebuildHdStaticSceneCommands()
{
	// This producer consumes world facts, never the Legacy screen painter or
	// _drawOrderBuffer. It is intentionally a chantier rendering path: dynamic
	// entities and final HD depth/occlusion are migrated in later steps.
	const bool initialBuild = _remasterStaticScene.revision == 0;
	// A frame advance changes sprites and light, not the tile inventory. Keep
	// the world snapshot for that common case; explicit state changes recapture.
	const unsigned structuralReasons = (1u << HDR_EXTERNAL) |
		(1u << HDR_STATE_QUEUE) | (1u << HDR_ATTACK);
	if (initialBuild || (_hdRedrawLastMask & structuralReasons))
	{
		OxceWorldAdapter::capture(_save, _hdPhysicalRevision + 1, _remasterWorldState);
		_remasterStaticScene.rebuild(_remasterWorldState);
	}
	_hdDrawCommands.clear();
	_drawSequence = 0;
	_drawOrderBuffer.clear();
	std::vector<const RemasterStaticInstance*> ordered;
	ordered.reserve(_remasterStaticScene.instanceCount());
	for (const auto &chunk : _remasterStaticScene.chunks)
		for (const auto &instance : chunk.second.instances)
			ordered.push_back(&instance);
	std::sort(ordered.begin(), ordered.end(), [](const RemasterStaticInstance *a, const RemasterStaticInstance *b)
	{
		if (a->worldTile.z != b->worldTile.z) return a->worldTile.z < b->worldTile.z;
		if (a->worldTile.y != b->worldTile.y) return a->worldTile.y < b->worldTile.y;
		if (a->worldTile.x != b->worldTile.x) return a->worldTile.x < b->worldTile.x;
		return (int)a->part < (int)b->part;
	});
	const int mapScale = std::max(1, _spriteWidth / 32);
	const BedrockMaterial bedrock = BedrockRenderPolicy::resolve(_save);
    const auto *roofGeometry = _save->getTileEngine()->getRealHdPhysicalGeometry();
    const auto geometryRevision = _save->getRealHdGeometryRevision();
    if (_hdRoofGeometryRevision != geometryRevision) {
        _hdRoofSunCache.clear(); _hdRoofGeometryRevision=geometryRevision;
    }

	for (const RemasterStaticInstance *instance : ordered)
	{
		const Position position(instance->worldTile.x, instance->worldTile.y, instance->worldTile.z);
		Tile *tile = _save->getTile(position);
		if (!tile) continue;
		const TilePart part = (TilePart)(int)instance->part;
		if (!hdVisibilitySourceDiscovered(tile, part)) continue;
		if (bedrock != BedrockMaterial::None && BedrockRenderPolicy::ownsTilePart(bedrock, tile, part)) continue;
		SurfaceRaw<const Uint8> legacy = tile->getSprite(part);
		if (!legacy && !findHdTerrainVisualRule(tile, part)) continue;
		Position projected;
		_camera->convertMapToScreen(position, &projected);
		const int x = projected.x;
		const int y = projected.y - instance->yOffset * mapScale;
		int shade = tile->isDiscovered(O_FLOOR) ? reShade(tile) : 16;
		if (part == O_WESTWALL || part == O_NORTHWALL) shade = getWallShade(part, tile);
		const bool rightHalf = part == O_NORTHWALL && tile->getSprite(O_WESTWALL);

        std::array<unsigned,16> roofRows{};
        const auto *floor = part == O_FLOOR ? tile->getMapData(O_FLOOR) : nullptr;
        bool flatRoof = floor && !floor->isNoFloor() && !floor->getBigWall() && floor->getTerrainLevel()==0;
        if (flatRoof) for(int slice=1;slice<12;++slice) if(floor->getLoftID(slice)!=0) flatRoof=false;
        if (flatRoof && roofGeometry && roofGeometry->ready()) {
            const int key=position.x+_save->getMapSizeX()*(position.y+_save->getMapSizeY()*position.z);
            auto cached=_hdRoofSunCache.find(key);
            if(cached==_hdRoofSunCache.end()) {
                roofRows=hdSunExposureRows(*roofGeometry,position.x,position.y,position.z,_save->getMapSizeZ());
                _hdRoofSunCache.emplace(key,roofRows);
            } else roofRows=cached->second;
        }
		const size_t begin = _hdDrawCommands.size();
		if (!queueHdTerrainOrConvention(tile, part, x, y, shade, rightHalf, 0) && legacy &&
			hdVisibilitySourceDiscovered(tile, part))
			queueLegacyIndexedAsset(legacy, x, y, shade, rightHalf, 0, _nvColor);
		for (size_t i = begin; i < _hdDrawCommands.size(); ++i)
		{
            _hdDrawCommands[i].roofSunRows=roofRows;
            _hdDrawCommands[i].roofCaustic=std::any_of(roofRows.begin(),roofRows.end(),[](unsigned row){return row!=0;});
			_hdDrawCommands[i].worldAnchored = true;
			_hdDrawCommands[i].worldZ = position.z;
			_hdDrawCommands[i].worldX = position.x;
			_hdDrawCommands[i].worldY = position.y;
			_hdDrawCommands[i].worldLayer = part == O_FLOOR ? 0 :
				(part == O_OBJECT && tile->isBackTileObject(O_OBJECT) ? 2 :
				(part == O_OBJECT ? 4 : 1));
		}
	}
	_hdStaticSceneCommandsReady = true;
	_hdStaticCommandCount = _hdDrawCommands.size();
	if (initialBuild)
	{
		Log(LOG_INFO) << "[REAL_HD_STATIC_AUTHORITY] worldInstances="
			<< _remasterStaticScene.instanceCount() << " gpuCommands=" << _hdDrawCommands.size()
			<< " producer=RemasterWorldState drawTerrain=OFF drawOrderBuffer=OFF camera=separate";
	}
}

void Map::appendHdDynamicUnitCommands()
{
	// The game supplies unit state; this producer supplies presentation. The
	// scene remains independent of drawTerrain() and its pixel order buffer.
	if (!_save || !_camera) return;
	_isAltPressed = _game->isAltPressed(true);
	_isCtrlPressed = _game->isCtrlPressed(true);
	UnitSprite unitSprite(this, _game->getMod(), _save, _animFrame,
		_save->getDepth() != 0, _isTFTD ? ArrowColorsTFTD[1] : ArrowColorsUFO[1],
		_isTFTD ? ArrowColorsTFTD[2] : ArrowColorsUFO[2]);
	_hdProducingDynamic = true;
	unsigned visibleUnits = 0;
	unsigned emittedCommands = 0;
	for (BattleUnit *unit : *_save->getUnits())
	{
		if (!unit || unit->isOut()) continue;
		const Position pos = unit->getPosition();
		if (pos.z > _camera->getViewLevel() && !_camera->getShowAllLayers()) continue;
		if (_camera->getShowSingleLayer() && pos.z != _camera->getViewLevel()) continue;
		for (int partY = 0; partY < unit->getArmor()->getSize(); ++partY)
		for (int partX = 0; partX < unit->getArmor()->getSize(); ++partX)
		{
		const Position partPos = pos + Position(partX, partY, 0);
		Tile *tile = _save->getTile(partPos);
		if (!tile || tile->getUnit() != unit) continue;
		Position screen;
		_camera->convertMapToScreen(partPos, &screen);
		screen += _camera->getMapOffset();
		const size_t begin = _hdDrawCommands.size();
		drawUnit(unitSprite, tile, tile, screen,
			pos.z == _camera->getViewLevel(), nullptr);
		if (_hdDrawCommands.size() == begin) continue;
		++visibleUnits;
		const size_t unitEnd = _hdDrawCommands.size();
		for (size_t i = begin; i < unitEnd; ++i)
		{
			auto &cmd = _hdDrawCommands[i];
			cmd.worldDynamic = true;
			cmd.worldX = partPos.x;
			cmd.worldY = partPos.y;
			cmd.worldZ = pos.z;
			cmd.worldLayer = 3;
			++emittedCommands;
		}
		// A billboard (including the rendered 3D model) may cross an open
		// storey while its gameplay tile remains below it. Partition this HD
		// command group at projected world-level planes, not Legacy pixels.
		// Keep disjoint half-open masks, so every pixel is composed once.
		const UnitWalkingOffset walking = calculateWalkingOffset(unit);
		const int scale = std::max(1, _spriteWidth / 32);
		const int planeOriginY = screen.y + walking.ScreenOffset.y - walking.TerrainLevelOffset + 32 * scale;
		const int lastLevel = _camera->getShowAllLayers() ? _save->getMapSizeZ()-1 : _camera->getViewLevel();
		const std::vector<HdDrawCommand> group(_hdDrawCommands.begin()+begin, _hdDrawCommands.begin()+unitEnd);
		_hdDrawCommands.resize(begin);
		for (int level = 0; level <= lastLevel; ++level)
		{
            // A real closed floor over this footprint remains an occluder.
            // Do not confuse support inherited from stairs below with a floor.
            bool closedAbove=false;
            for(int aboveZ=pos.z+1;aboveZ<=level;++aboveZ) {
                const Tile *above=_save->getTile(Position(partPos.x,partPos.y,aboveZ));
                if(above && !above->hasNoFloor(nullptr)){closedAbove=true;break;}
            }
            if(closedAbove)continue;
            const auto bounds=hdStoreyBand(level,lastLevel,pos.z,planeOriginY,_visibleMapHeight,scale);
            const int top=bounds.top, bottom=bounds.bottom;
			if (bottom <= top) continue;
			// Native unit frames occupy 40 pixels; allow weapon/indicator
			// overhang without submitting remote, empty storeys.
			const int unitTop = screen.y + walking.ScreenOffset.y - 32*scale;
			const int unitBottom = screen.y + walking.ScreenOffset.y + 64*scale;
			if (bottom <= unitTop || top >= unitBottom) continue;
			GraphSubset band(getWidth(), bottom-top);
			band = band.offset(0, top);
			for (const auto &original : group)
			{
				auto cmd = original;
				cmd.worldZ = level;
				cmd.clipMask = cmd.hasClipMask ? GraphSubset::intersection(cmd.clipMask, band) : band;
				cmd.hasClipMask = true;
				if (!cmd.unit3DModel.empty())
					cmd.unit3DMask = GraphSubset::intersection(cmd.unit3DMask, band);
				_hdDrawCommands.push_back(std::move(cmd));
			}
		}
		}
	}
	_hdProducingDynamic = false;
	static bool logged = false;
	if (!logged)
	{
		Log(LOG_INFO) << "[REAL_HD_DYNAMIC_AUTHORITY] visibleUnits=" << visibleUnits
			<< " gpuCommands=" << emittedCommands
			<< " producer=unit-state drawTerrain=OFF";
		logged = true;
	}
}

void Map::queueHdSemanticFrame(const std::string &family, int frame, const Position &world,
	int x, int y, int shade, int color, bool overlay)
{
	if (frame < 0) return;
	const size_t begin = _hdDrawCommands.size();
	if (!queueHdSurfaceSetOrConvention(family, frame, "semantic:" + family, x, y, shade, false, 0))
		queueLegacyIndexedAsset(_game->getMod()->getSurfaceSet(family)->getFrame(frame), x, y, shade, false, 0, color);
	for (size_t i = begin; i < _hdDrawCommands.size(); ++i)
	{
		auto &cmd = _hdDrawCommands[i];
		cmd.worldDynamic = true;
		cmd.worldX = world.x; cmd.worldY = world.y; cmd.worldZ = world.z;
		cmd.worldLayer = 5;
		cmd.legacyBaseColor = color;
		cmd.postVisibility = overlay;
	}
}

void Map::updateHdProjectileCamera()
{
    Position bulletPositionScreen;
    int bulletLowX=16000, bulletLowY=16000, bulletLowZ=16000, bulletHighX=0, bulletHighY=0, bulletHighZ=0;
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
				// HD PROJECTILE CAMERA SMOOTH FOLLOW V1. Legacy either waits for the projectile
				// to leave the viewport and jumps by a complete screen, or (old smooth mode) snaps
				// the projectile straight to center. In HD, engage before the edge and ease the
				// camera toward the projectile in bounded pixel steps. Projectile speed/trajectory
				// are untouched; this is presentation-only camera motion.
				if (_launch)
				{
					_launch = false;
					if ((bulletPositionScreen.x < 1 || bulletPositionScreen.x > getWidth() - 1 ||
						bulletPositionScreen.y < 1 || bulletPositionScreen.y > _visibleMapHeight - 1))
					{
						// Only the exceptional case where the shot is already off-screen at creation may
						// recenter immediately. Normal visible shots never use this jump.
						_camera->centerOnPosition(Position(bulletLowX, bulletLowY, bulletHighZ), false);
						_camera->convertVoxelToScreen(_projectile->getPosition(), &bulletPositionScreen);
					}
				}

				const int safeMarginX = std::max(_spriteWidth * 2, getWidth() / 5);
				const int safeMarginY = std::max(_spriteHeight * 2, _visibleMapHeight / 5);
				if (!_smoothingEngaged &&
					(bulletPositionScreen.x < safeMarginX || bulletPositionScreen.x > getWidth() - safeMarginX ||
					 bulletPositionScreen.y < safeMarginY || bulletPositionScreen.y > _visibleMapHeight - safeMarginY))
				{
					_smoothingEngaged = true;
				}

				if (_smoothingEngaged)
				{
					const int errorX = getWidth() / 2 - bulletPositionScreen.x;
					const int errorY = _visibleMapHeight / 2 - bulletPositionScreen.y;
					const int maxStepX = std::max(2, getWidth() / 18);
					const int maxStepY = std::max(2, _visibleMapHeight / 18);
					auto easedStep = [](int error, int maxStep) -> int
					{
						if (std::abs(error) <= 2) return 0;
						int step = (int)std::lround((double)error * 0.28);
						if (step == 0) step = error > 0 ? 1 : -1;
						return std::max(-maxStep, std::min(maxStep, step));
					};
					const int stepX = easedStep(errorX, maxStepX);
					const int stepY = easedStep(errorY, maxStepY);
					if (stepX || stepY)
					{
						_camera->jumpXY(stepX, stepY);
						_camera->convertVoxelToScreen(_projectile->getPosition(), &bulletPositionScreen);
					}
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
						_camera->jumpXY(+getWidth(), 0);
						enough = false;
					}
					else if (bulletPositionScreen.x > getWidth())
					{
						_camera->jumpXY(-getWidth(), 0);
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

}

void Map::appendHdCombatEffectsCommands()
{
	const int scale = std::max(1, _spriteWidth / 32);
	_hdExplosionFlash = _explosionInFOV && _flashScreen;
	if (_hdExplosionFlash) _flashScreen = false;
	if (_projectile && _projectileInFOV)
	{
		BattleItem *item = _projectile->getItem();
		const int count = item ? 1 : BULLET_SPRITES;
		for (int n = 0; n < count; ++n)
		{
			const int i = !item && _projectile->isReversed() ? count - 1 - n : n;
			const Position voxel = item ? _projectile->getPosition() : _projectile->getPosition(1-i);
			Tile *tile = _save->getTile(voxel.toTile());
			if (!tile || (!_save->getDebugMode() && !_save->getTileEngine()->isVoxelVisible(voxel))) continue;
			Position screen;
			_camera->convertVoxelToScreen(voxel, &screen);
			const int shade = item ? reShade(tile) : 0;
			const int frame = item ? item->getFloorSpriteIndex(_save, _animFrame, shade) : _projectile->getParticle(i);
			const std::string family = item ? "FLOOROB.PCK" : (_save->getDepth() ? "UnderwaterProjectiles" : "Projectiles");
			SurfaceRaw<const Uint8> sprite = _game->getMod()->getSurfaceSet(family)->getFrame(frame);
			if (!sprite) continue;
			Position shadowVoxel = voxel;
			shadowVoxel.z = _save->getTileEngine()->castedShade(voxel);
			if (_save->getTileEngine()->isVoxelVisible(shadowVoxel))
			{
				Position shadow; _camera->convertVoxelToScreen(shadowVoxel, &shadow);
				queueHdSemanticFrame(family, frame, shadowVoxel.toTile(),
					shadow.x-(item ? 16*scale : sprite.getWidth()/2), shadow.y-(item ? 26*scale : sprite.getHeight()/2), 16, _nvColor);
			}
			queueHdSemanticFrame(family, frame, voxel.toTile(),
				screen.x - (item ? 16*scale : sprite.getWidth()/2),
				screen.y - (item ? 26*scale : sprite.getHeight()/2), shade, _nvColor);
		}
	}
	for (const Explosion *explosion : _explosions)
	{
		const int frame = explosion->getCurrentFrame();
		if (frame < 0) continue;
		const Position voxel = explosion->getPosition();
		Tile *tile = _save->getTile(voxel.toTile());
		if (!explosion->isBig() && !_save->getDebugMode() && (!tile || !tile->getVisible())) continue;
		const std::string family = explosion->isBig() ? "X1.PCK" : (explosion->isHit() ? "HIT.PCK" : "SMOKE.PCK");
		SurfaceRaw<const Uint8> sprite = _game->getMod()->getSurfaceSet(family)->getFrame(frame);
		if (!sprite) continue;
		Position screen;
		_camera->convertVoxelToScreen(voxel, &screen);
		queueHdSemanticFrame(family, frame, voxel.toTile(),
			screen.x - (explosion->isBig() ? sprite.getWidth()/2 : 15*scale),
			screen.y - (explosion->isBig() ? sprite.getHeight()/2 : (explosion->isHit() ? 25 : 15)*scale), 0, _nvColor);
	}
	// Objects and environmental animations are live world facts, not terrain-cache pixels.
	_hdPresentationTiles.clear();
	int beginX, beginY, endX, endY, unused;
	_camera->convertScreenToMap(-2*_spriteWidth, -2*_spriteHeight, &beginX, &unused);
	_camera->convertScreenToMap(getWidth()+2*_spriteWidth, -2*_spriteHeight, &unused, &beginY);
	_camera->convertScreenToMap(getWidth()+2*_spriteWidth, _visibleMapHeight+2*_spriteHeight, &endX, &unused);
	_camera->convertScreenToMap(-2*_spriteWidth, _visibleMapHeight+2*_spriteHeight, &unused, &endY);
	const int zMargin = 2*_save->getMapSizeZ();
	beginX = std::max(0, beginX-zMargin); beginY = std::max(0, beginY-zMargin);
	endX = std::min(_save->getMapSizeX()-1, endX+zMargin); endY = std::min(_save->getMapSizeY()-1, endY+zMargin);
	for (int z = 0; z < _save->getMapSizeZ(); ++z)
	for (int y = beginY; y <= endY; ++y)
	for (int x = beginX; x <= endX; ++x)
	{
		Tile *tile = _save->getTile(Position(x,y,z));
		const Position pos = tile->getPosition();
		if (!tile->isDiscovered(O_FLOOR) || (!_camera->getShowAllLayers() && pos.z > _camera->getViewLevel()) ||
			(_camera->getShowSingleLayer() && pos.z != _camera->getViewLevel())) continue;
		Position screen;
		_camera->convertMapToScreen(pos, &screen); screen += _camera->getMapOffset();
		if (screen.x < -2*_spriteWidth || screen.x > getWidth()+_spriteWidth || screen.y < -2*_spriteHeight || screen.y > _visibleMapHeight+_spriteHeight) continue;
		_hdPresentationTiles.push_back(tile);
		const int shade = reShade(tile);
		if (BattleItem *item = tile->getTopItem())
		{
			const size_t itemBegin = _hdDrawCommands.size();
			queueHdSemanticFrame("FLOOROB.PCK", item->getFloorSpriteIndex(_save, _animFrame, shade), pos,
				screen.x, screen.y + tile->getTerrainLevel()*scale, shade, _nvColor);
			for (size_t i = itemBegin; i < _hdDrawCommands.size(); ++i) _hdDrawCommands[i].worldLayer = 2;
			applyRealHdLightingTintToCommands(itemBegin, tile);
		}
		const bool waterCloud = _save->getDepth() > 0 && Options::hdUnderwaterSmokeEnabled;
		const bool volumeOwns = (waterCloud || (Options::hdSmokeVolumeEnabled && Options::hdSmokeReplaceLegacy)) && HdGpuBackend::instance().directWorldReady() && !tile->getFire();
		if (tile->getSmoke() && !volumeOwns)
		{
			int frame = 0;
			if (!tile->getFire())
			{
				frame = _save->getDepth() ? Mod::UNDERWATER_SMOKE_OFFSET : Mod::SMOKE_OFFSET;
				frame += Mod::EXTENDED_SMOKE_OFFSET == 2 ? (tile->getSmoke()-1)/5*4 : int(std::floor(tile->getSmoke()/6.0-0.1))*(Mod::EXTENDED_SMOKE_OFFSET == 1 ? 4 : 1);
			}
			frame += ((_animFrame/2)%4 + tile->getAnimationOffset())%4;
			queueHdSemanticFrame("SMOKE.PCK", frame, pos, screen.x, screen.y, tile->getFire() ? 0 : shade, _nvColor);
		}
		for (int side = 0; side < 2; ++side)
		for (const Particle &particle : getVaporParticle(tile, side ? (pos.z == _camera->getViewLevel() ? 3 : 1) : 0))
		{
			const SDL_Color tint = _game->getMod()->getTransparencyTint(particle.getColor(), particle.getOpacity());
			if (!tint.unused || particle.getSize() > 3) continue;
			HdDrawCommand cmd;
			cmd.generatedRgba.assign(16,0);
			const int coverage[4] = {0,2,1,3};
			for (int pixel=0; pixel<4; ++pixel)
			if (particle.getSize() <= coverage[pixel])
			{
				cmd.generatedRgba[pixel*4] = tint.r;
				cmd.generatedRgba[pixel*4+1] = tint.g;
				cmd.generatedRgba[pixel*4+2] = tint.b;
				cmd.generatedRgba[pixel*4+3] = 255-tint.unused;
			}
			cmd.assetPath = "REAL_HD_VAPOR:"+std::to_string(tint.r)+":"+std::to_string(tint.g)+":"+std::to_string(tint.b)+":"+std::to_string(tint.unused)+":"+std::to_string(particle.getSize());
			cmd.x = screen.x+_spriteWidth/2+particle.getOffsetX()*scale;
			cmd.y = screen.y+_spriteHeight-_spriteWidth/2+pos.toVoxel().z*scale+particle.getOffsetY()*scale;
			cmd.worldDynamic = true; cmd.worldX=pos.x; cmd.worldY=pos.y; cmd.worldZ=pos.z; cmd.worldLayer=side ? 5 : 2;
			_hdDrawCommands.push_back(std::move(cmd));
		}
	}
}

void Map::appendHdTacticalIndicators()
{
	_hdTacticalNumbers.clear();
	if (_cursorType == CT_NONE) return;
	const int scale = std::max(1, _spriteWidth / 32);
	if (_isAltPressed && _save->isPreview())
	for (const Position &pos : _save->getCraftTiles())
	{
		if (pos.z != _camera->getViewLevel()) continue;
		Position screen; _camera->convertMapToScreen(pos, &screen); screen += _camera->getMapOffset();
		queueLegacyIndexedAsset(SurfaceRaw<const Uint8>(_arrow), screen.x+_spriteWidth/2-_arrow->getWidth()/2,
			screen.y+2*scale-_arrow->getHeight()+getArrowBobForFrame(_animFrame)*scale, 0, false, 0);
		_hdDrawCommands.back().postVisibility = true;
	}
	for (BattleUnit *unit : *_save->getUnits())
	{
		const bool selected = unit == _save->getSelectedUnit() && (_save->getSide() == FACTION_PLAYER || _save->getDebugMode()) && unit->getPosition().z <= _camera->getViewLevel();
		const bool scanned = _isAltPressed && _save->getSide() == FACTION_PLAYER && unit->getScannedTurn() == _save->getTurn() && unit->getFaction() != FACTION_PLAYER && !unit->isOut();
		const bool custom = _isAltPressed && _save->getSide() == FACTION_PLAYER && unit->getCustomMarker() > 0 && unit->getFaction() == FACTION_PLAYER && !unit->isOut();
		if (!selected && !scanned && !custom) continue;
		Position pos = unit->getPosition(), screen;
		if (!selected) pos.z = _camera->getViewLevel();
		_camera->convertMapToScreen(pos, &screen); screen += _camera->getMapOffset();
		Position offset = selected ? calculateWalkingOffset(unit).ScreenOffset : Position();
		offset.y += (unit->isBigUnit() ? 4 : 0) + Position::TileZ - (scanned ? 21 : unit->getHeight()+unit->getFloatHeight()) - (unit->isKneeled() ? 2 : 0);
		queueLegacyIndexedAsset(SurfaceRaw<const Uint8>(_arrow), screen.x+offset.x+_spriteWidth/2-_arrow->getWidth()/2,
			screen.y+offset.y-_arrow->getHeight()+getArrowBobForFrame(_animFrame)*scale, 0, false, 0,
			custom && !selected ? (_isTFTD ? ArrowColorsTFTD[unit->getCustomMarker()%4] : ArrowColorsUFO[unit->getCustomMarker()%4]) : 0);
		_hdDrawCommands.back().postVisibility = true;
	}
	int waypointId = 0;
	std::map<int,int> waypointOffsets;
	for (const Position &pos : _waypoints)
	{
		++waypointId;
		Tile *tile = _save->getTile(pos);
		if (!tile || !tile->isDiscovered(O_FLOOR) || pos.z > _camera->getViewLevel()) continue;
		Position screen; _camera->convertMapToScreen(pos, &screen); screen += _camera->getMapOffset();
		queueHdSemanticFrame("CURSOR.PCK", 7, pos, screen.x, screen.y, 0, 0, true);
		const int key = (pos.z*_save->getMapSizeY()+pos.y)*_save->getMapSizeX()+pos.x;
		const int slot = waypointOffsets[key]++;
		_hdTacticalNumbers.push_back({waypointId, screen.x+(2+(slot%3)*8)*scale, screen.y+(2+(slot/3)*8)*scale, 0});
	}
	for (Tile *tile : _hdPresentationTiles)
	{
		const Position pos = tile->getPosition();
		if (tile->getPreview() < 0 || !tile->isDiscovered(O_FLOOR) || pos.z > _camera->getViewLevel()) continue;
		Position screen; _camera->convertMapToScreen(pos, &screen); screen += _camera->getMapOffset();
		if (_previewSettingArrows)
			queueHdSemanticFrame("Pathfinding", tile->getPreview()+12, pos, screen.x, screen.y+tile->getTerrainLevel()*scale, 0, tile->getMarkerColor(), true);
		const int adjustment = -tile->getTerrainLevel()*scale + (_previewSettingArrows ? 7 : 0);
		const int color = _previewSettingArrows ? 0 : tile->getMarkerColor();
		if (_previewSettingTu && tile->getTUMarker() >= 0)
			_hdTacticalNumbers.push_back({tile->getTUMarker(), screen.x+16*scale-(tile->getTUMarker()>9 ? 5 : 3), screen.y+(_previewSettingEnergy ? 22 : 29)*scale-adjustment, color});
		if (_previewSettingEnergy && tile->getEnergyMarker() >= 0)
			_hdTacticalNumbers.push_back({tile->getEnergyMarker(), screen.x+16*scale-(tile->getEnergyMarker()>9 ? 5 : 3), screen.y+29*scale-adjustment, color});
	}
}

void Map::appendHdCursorCommand()
{
    _hdCursorInfoVisible = false;
    if (_cursorType == CT_NONE || !_camera || !_save || _save->getBattleState()->getMouseOverIcons()) return;
    // The old terrain traversal also produced the relief cursor. Keep that
    // producer in the direct scene path, using the same physical surface quads.
    // Unknown ground retains the neutral interface cursor below: its shape must
    // never disclose geometry that the player has not discovered.
    const BedrockMaterial material = BedrockRenderPolicy::resolve(_save);
    if (_cursorType == CT_NORMAL && _cursorSize == 1 && material != BedrockMaterial::None)
    {
        const int viewZ = _camera->getViewLevel();
        // Surface snapping only owns the selected logical level. Searching
        // downwards here swallowed the elevated cursor and its lower guides.
        Tile *tile = _save->getTile(Position(_selectorX, _selectorY, viewZ));
        if (tile && hdVisibilitySourceDiscovered(tile, O_FLOOR) &&
            BedrockRenderPolicy::hasSurface(material, tile))
        {
            HdDrawCommand cursor;
            cursor.surfaceCursor = true;
            cursor.cursorTile = tile->getPosition();
            cursor.cursorYellow = tile->getUnit() &&
                (tile->getUnit()->getVisible() || _save->getDebugMode());
            cursor.worldDynamic = true;
            cursor.worldX = _selectorX;
            cursor.worldY = _selectorY;
            cursor.worldZ = viewZ;
            cursor.worldLayer = 0; // Visible scenery in front still covers the marker.
            cursor.drawOrder = nextDrawSequence();
            _hdDrawCommands.push_back(cursor);
            // Match the two semantic CURSOR.PCK halves: the back precedes
            // the unit, the front follows it, while nearer tiles still occlude.
            cursor.surfaceCursorFront = true;
            cursor.worldLayer = 6;
            cursor.drawOrder = nextDrawSequence();
            _hdDrawCommands.push_back(std::move(cursor));
            return;
        }
    }
    for (int dx=0; dx<_cursorSize; ++dx)
    for (int dy=0; dy<_cursorSize; ++dy)
    for (int z=0; z<=_camera->getViewLevel(); ++z)
    {
        const Position pos(_selectorX+dx,_selectorY+dy,z);
        Tile *tile=_save->getTile(pos);
        if (!tile) continue;
        const bool top=z==_camera->getViewLevel();
        if (!top && (_camera->getShowSingleLayer() || !hdCanPresentLowerCursor(_save,pos.x,pos.y,_camera->getViewLevel(),z))) continue;
        // Lower guides share the discovered terrain geometry with the main
        // cursor. Keep neutral sprite guides only where no surface is known.
        if (!top && _cursorType == CT_NORMAL && _cursorSize == 1 &&
            material != BedrockMaterial::None && hdVisibilitySourceDiscovered(tile, O_FLOOR) &&
            BedrockRenderPolicy::hasSurface(material, tile))
        {
            HdDrawCommand guide;
            guide.surfaceCursor = true;
            guide.surfaceCursorGuide = true;
            guide.cursorTile = pos;
            guide.worldDynamic = true;
            guide.worldX = pos.x; guide.worldY = pos.y; guide.worldZ = pos.z;
            guide.worldLayer = 0;
            guide.drawOrder = nextDrawSequence();
            _hdDrawCommands.push_back(guide);
            guide.surfaceCursorFront = true;
            guide.worldLayer = 6;
            guide.drawOrder = nextDrawSequence();
            _hdDrawCommands.push_back(std::move(guide));
            continue;
        }
        Position screen; _camera->convertMapToScreen(pos,&screen); screen+=_camera->getMapOffset();
        const bool yellow=tile->getUnit() && (tile->getUnit()->getVisible() || _save->getDebugMode());
        if (top && _cursorType==CT_AIM)
            queueHdSemanticFrame("CURSOR.PCK",yellow ? 7+((_animFrame/2)%4) : 6,pos,screen.x,screen.y,0,0,true);
        else
        {
            const int back=top ? (yellow ? _animFrame%2 : 0) : 2;
            queueHdSemanticFrame("CURSOR.PCK",back,pos,screen.x,screen.y,0,0,true);
            queueHdSemanticFrame("CURSOR.PCK",back+3,pos,screen.x,screen.y,0,0,true);
            if (top && !_isAltPressed && _cursorType>CT_AIM)
            {
                const int frames[6]={0,0,0,11,13,15};
                queueHdSemanticFrame("CURSOR.PCK",frames[_cursorType]+(_animFrame/4)%2,pos,screen.x,screen.y,0,0,true);
            }
        }
    }
    updateHdCursorInfo();
}

void Map::updateHdCursorInfo()
{
    if (_cursorType < CT_AIM || !_showInfoOnCursor || (_cursorType == CT_THROW && Options::oxceDisableInfoOnThrowCursor)) return;
    BattleAction *current = _save->getBattleGame()->getCurrentAction();
    if (!current || !current->actor || !current->weapon) return;
    const int itX = _selectorX, itY = _selectorY, itZ = _camera->getViewLevel();
    Tile *tile = _save->getTile(Position(itX,itY,itZ));
    if (!tile) return;
    BattleUnit *unit = tile->getUnit();
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
    _hdCursorInfoVisible = true;
}

void Map::rebuildRemasterWorldState()
{
	// REMASTER WORLD STATE V0: semantic extraction is deliberately performed
	// outside drawTerrain() and before any Legacy raster traversal. This state
	// is diagnostic-only in V0; rendering/gameplay remain unchanged.
	OxceWorldAdapter::capture(_save, _hdPhysicalRevision, _remasterWorldState);
	_remasterWorldStateBuiltRevision = _hdPhysicalRevision;

	static bool logged = false;
	if (!logged)
	{
		Log(LOG_INFO) << "[REMASTER WORLD STATE V0][ACTIVE] revision=" << _remasterWorldState.revision
			<< " tiles=" << _remasterWorldState.tiles.size()
			<< " terrainParts=" << _remasterWorldState.terrainParts.size()
			<< " units=" << _remasterWorldState.units.size()
			<< " items=" << _remasterWorldState.items.size()
			<< " source=OXCE_WORLD_ADAPTER rasterDependency=NONE drawOrderDependency=NONE rendererConsumer=DIAGNOSTIC_ONLY";
		logged = true;
	}
}

std::string Map::getRemasterWorldTrace()
{
	// V0 is diagnostic-only. Capture the OXCE snapshot only when the diagnostic
	// actually requests it, never on every tactical redraw.
	if (_remasterWorldStateBuiltRevision != _hdPhysicalRevision)
		rebuildRemasterWorldState();
	std::ostringstream out;
	unsigned visibleTiles = 0, knownTiles = 0, smokeTiles = 0, fireTiles = 0;
	for (const RemasterTileState &tile : _remasterWorldState.tiles)
	{
		if (tile.visibleNow > 0) ++visibleTiles;
		bool discovered = false;
		for (bool bit : tile.discovered) discovered = discovered || bit;
		if (discovered) ++knownTiles;
		if (tile.smoke > 0) ++smokeTiles;
		if (tile.fire > 0) ++fireTiles;
	}
	out << "RWS rev=" << _remasterWorldState.revision
		<< " tiles=" << _remasterWorldState.tiles.size()
		<< " parts=" << _remasterWorldState.terrainParts.size()
		<< " units=" << _remasterWorldState.units.size()
		<< " items=" << _remasterWorldState.items.size()
		<< " known/vis=" << knownTiles << "/" << visibleTiles
		<< " smoke/fire=" << smokeTiles << "/" << fireTiles
		<< " extract=PRE_RASTER"
		<< " legOrderPx=";
	unsigned legacyOrderPixels = 0;
	for (unsigned order : _drawOrderBuffer) if (order != 0) ++legacyOrderPixels;
	out << legacyOrderPixels
		<< " bridgeRanges=" << _hdLegacyBridgeRanges
		<< " legacyGpuCmd=" << _hdLegacyGpuCompatCommands
		<< "\n";
	return out.str();
}

void Map::queueHdAsset(const std::string &assetPath, int nativeScale, int offsetX, int offsetY, int x, int y, int shade, bool rightHalfOnly, unsigned order, const GraphSubset *clipMask, const std::string &colorMode, const std::string &materialProfile)
{
	// Validate the chosen resource before accepting the command. A damaged
	// replacement must not disappear silently when the compositor reaches it.
	_hdImageCache.require(assetPath);
	// Successful routing is already resolved and cached before this call. Avoid
	// rebuilding a diagnostic string and taking the trace mutex for every visible
	// sprite on every camera redraw.
	HdDrawCommand cmd;
	cmd.assetPath = assetPath;
	cmd.nativeScale = nativeScale;
	cmd.colorMode = hdColorModeForAssetPath(assetPath, colorMode);
	cmd.materialProfile = materialProfile.empty() ? HdMaterialProfile::Auto : hdMaterialProfileFromString(materialProfile);
	// Authored true-colour HD assets no longer use legacyBaseColor for grading.
	// Legacy/raw commands still use it for historical palette recolour semantics.
	cmd.legacyBaseColor = 0;
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
	++getHdPerfStats().current.mapHdGpuCommands;
}

void Map::queueLegacyIndexedAsset(SurfaceRaw<const Uint8> source, int x, int y, int shade, bool rightHalfOnly, unsigned order, int newBaseColor, const GraphSubset *clipMask)
{
	if (!source) return;
	HdDrawCommand cmd;
	cmd.legacyRaw = true;
	cmd.legacyIndices = source.getBuffer();
	cmd.legacyWidth = (unsigned)source.getWidth();
	cmd.legacyHeight = (unsigned)source.getHeight();
	cmd.legacyPitch = (unsigned)source.getPitch();
	cmd.legacyBaseColor = newBaseColor;
	std::ostringstream key;
	key << "LEGACY_RAW:" << (const void*)source.getBuffer()
		<< ':' << source.getWidth() << 'x' << source.getHeight() << ':' << source.getPitch();
	cmd.legacyKey = key.str();
	cmd.colorMode = "indexedLegacy";
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
	++getHdPerfStats().current.mapLegacyGpuCommands;
}

bool Map::queueLegacyIfGpuReady(SurfaceRaw<const Uint8> source, int x, int y, int shade, bool rightHalfOnly, unsigned order, int newBaseColor, const GraphSubset *clipMask, const Tile *visibilityTile, TilePart visibilityPart)
{
	if (!Options::hdGraphics || !source || !HdGpuBackend::instance().directWorldReady()) return false;
	// VISIBILITY PRESENTATION TAKEOVER V3: hidden source geometry is still culled
	// before lighting/compositing, but the cull now follows OXCE's per-part discovery.
	const bool takeover = visibilityTile != nullptr;
	if (takeover && !hdVisibilitySourceDiscovered(visibilityTile, visibilityPart))
		return true;
	const size_t tintBegin = _hdDrawCommands.size();
	queueLegacyIndexedAsset(source, x, y, shade, rightHalfOnly, order, newBaseColor, clipMask);
	if (visibilityTile)
	{
		applyRealHdLightingTintToCommands(tintBegin, visibilityTile);
	}
	return true;
}

void Map::reloadHdResources()
{
	// HD artist hot-reload: both CPU decoded images and D3D11 SRV/mipmap chains
	// are keyed by the virtual asset path. Replacing PNG bytes under the same name
	// therefore requires invalidating both caches explicitly.
	_hdImageCache.clear();
	_hdPngLayerFallbacks.clear();
	_hdTerrainAssetResolveCache.clear();
	_hdTerrainRuleResolveCache.clear();
	_hdSurfaceSetAssetResolveCache.clear();
	_hdSurfaceSetRuleResolveCache.clear();
	_hdBedrockCraterFieldSourceRevision = ~0ULL;
	_hdBedrockWeaponField.clear();
	_hdBedrockWeaponFieldW = _hdBedrockWeaponFieldH = 0;
	_hdBedrockWeaponFieldUploadRevision = 0;
	HdGpuBackend::instance().clearHdImageCache();

	const BedrockMaterial material = BedrockRenderPolicy::resolve(_save);
	if (material != BedrockMaterial::None)
	{
		const std::string paths[] =
		{
			BedrockRenderPolicy::assetPath(material), BedrockRenderPolicy::normalPath(material),
			BedrockRenderPolicy::roughnessPath(material), BedrockRenderPolicy::aoPath(material),
			BedrockRenderPolicy::craterCoreMaskPath(material), BedrockRenderPolicy::craterRimMaskPath(material),
			BedrockRenderPolicy::blastHaloMaskPath(material), BedrockRenderPolicy::craterMaskPath(material),
			BedrockRenderPolicy::verticalAssetPath(material), BedrockRenderPolicy::verticalNormalPath(material),
			BedrockRenderPolicy::verticalRoughnessPath(material), BedrockRenderPolicy::verticalAoPath(material)
		};
		for (const std::string &path : paths)
		{
			if (path.empty()) continue;
			if (FileMap::fileExists(path))
			{
				const FileMap::FileRecord *record = FileMap::at(path);
				Log(LOG_INFO) << "[HD RESOURCE RELOAD][BEDROCK] " << path
					<< " <- " << (record ? record->fullpath : std::string("<unknown>"));
			}
			else
			{
				Log(LOG_WARNING) << "[HD RESOURCE RELOAD][BEDROCK-MISSING] " << path;
			}
		}
		for (int variant = 0; variant < 7; ++variant)
		{
			const std::string variantPaths[] =
			{
				BedrockRenderPolicy::blastCoreVariantPath(material, variant),
				BedrockRenderPolicy::blastRimVariantPath(material, variant),
				BedrockRenderPolicy::blastHaloVariantPath(material, variant),
				BedrockRenderPolicy::weaponImpactVariantPath(material, variant)
			};
			for (const std::string &path : variantPaths)
			{
				if (path.empty() || !FileMap::fileExists(path)) continue;
				const FileMap::FileRecord *record = FileMap::at(path);
				Log(LOG_INFO) << "[HD RESOURCE RELOAD][BEDROCK-VARIANT] " << path
					<< " <- " << (record ? record->fullpath : std::string("<unknown>"));
			}
		}
		for (int variant = 0; variant < 3; ++variant)
		{
			const std::string tripletPaths[] =
			{
				BedrockRenderPolicy::weaponImpactCoreVariantPath(material, variant),
				BedrockRenderPolicy::weaponImpactRimVariantPath(material, variant),
				BedrockRenderPolicy::weaponImpactHaloVariantPath(material, variant)
			};
			for (const std::string &path : tripletPaths)
			{
				if (path.empty() || !FileMap::fileExists(path)) continue;
				const FileMap::FileRecord *record = FileMap::at(path);
				Log(LOG_INFO) << "[HD RESOURCE RELOAD][BEDROCK-WEAPON-TRIPLET] " << path
					<< " <- " << (record ? record->fullpath : std::string("<unknown>"));
			}
		}
		for (int variant = 0; variant < 4; ++variant)
		{
			const std::string path = BedrockRenderPolicy::weaponDustVariantPath(material, variant);
			if (path.empty() || !FileMap::fileExists(path)) continue;
			const FileMap::FileRecord *record = FileMap::at(path);
			Log(LOG_INFO) << "[HD RESOURCE RELOAD][BEDROCK-WEAPON-DUST] " << path
				<< " <- " << (record ? record->fullpath : std::string("<unknown>"));
		}
		for (int variant = 0; variant < 5; ++variant)
		{
			const std::string path = BedrockRenderPolicy::blastDustVariantPath(material, variant);
			if (path.empty() || !FileMap::fileExists(path)) continue;
			const FileMap::FileRecord *record = FileMap::at(path);
			Log(LOG_INFO) << "[HD RESOURCE RELOAD][BEDROCK-BLAST-DUST] " << path
				<< " <- " << (record ? record->fullpath : std::string("<unknown>"));
		}
	}

	invalidateHd(HDR_EXTERNAL);
	prewarmHdMissionResources();
}

void Map::prewarmHdMissionResources()
{
	if (!Options::hdGraphics || !_save || !_game || !_game->getMod()) return;

	std::set<std::string> terrainAssets;
	std::set<std::string> surfaceAssets;
	std::set<std::string> unitAssets;
	std::map<MapDataSet*, std::set<int>> terrainSeeds;
	std::set<std::string> unitPartDirs;

	auto remember = [](std::set<std::string> &bucket, const std::string &path)
	{
		if (!path.empty()) bucket.insert(path);
	};

	auto rememberRule = [&](std::set<std::string> &bucket, const HdVisualRule *rule)
	{
		if (!rule) return;
		for (const HdVisualLayer &layer : rule->layers)
		{
			for (const auto &statePair : layer.states)
			{
				for (const std::string &frame : statePair.second.frames)
				{
					const std::string path = hdJoinPath(layer.root, frame);
					if (!path.empty() && _hdImageCache.usable(path)) bucket.insert(path);
				}
			}
		}
	};


	auto rememberSurfaceFrame = [&](const std::string &setName, int frame)
	{
		if (frame < 0) return;
		rememberRule(surfaceAssets, findHdSurfaceSetVisualRule(setName, frame));
		remember(surfaceAssets, findHdSurfaceSetAsset(setName, frame));
	};

	auto rememberSurfaceSet = [&](const std::string &setName)
	{
		if (SurfaceSet *set = _game->getMod()->getSurfaceSet(setName, false))
		{
			for (size_t frame = 0; frame < set->getTotalFrames(); ++frame)
				rememberSurfaceFrame(setName, (int)frame);
		}
	};
	rememberSurfaceSet("HANDOB.PCK");
	rememberSurfaceSet("BREATH-1.PCK");
	rememberSurfaceSet("DETBLOB.DAT");

	const BedrockMaterial bedrockMaterial = BedrockRenderPolicy::resolve(_save);
	if (bedrockMaterial != BedrockMaterial::None)
	{
		const std::string bedrockAssets[] =
		{
			BedrockRenderPolicy::assetPath(bedrockMaterial),
			BedrockRenderPolicy::normalPath(bedrockMaterial),
			BedrockRenderPolicy::roughnessPath(bedrockMaterial),
			BedrockRenderPolicy::aoPath(bedrockMaterial),
			BedrockRenderPolicy::craterCoreMaskPath(bedrockMaterial),
			BedrockRenderPolicy::craterRimMaskPath(bedrockMaterial),
			BedrockRenderPolicy::blastHaloMaskPath(bedrockMaterial),
			BedrockRenderPolicy::craterMaskPath(bedrockMaterial),
			BedrockRenderPolicy::verticalAssetPath(bedrockMaterial),
			BedrockRenderPolicy::verticalNormalPath(bedrockMaterial),
			BedrockRenderPolicy::verticalRoughnessPath(bedrockMaterial),
			BedrockRenderPolicy::verticalAoPath(bedrockMaterial)
		};
		for (const std::string &bedrockAsset : bedrockAssets)
		{
			if (!bedrockAsset.empty() && _hdImageCache.usable(bedrockAsset)) terrainAssets.insert(bedrockAsset);
		}
		for (int variant = 0; variant < 7; ++variant)
		{
			const std::string variantAssets[] =
			{
				BedrockRenderPolicy::blastCoreVariantPath(bedrockMaterial, variant),
				BedrockRenderPolicy::blastRimVariantPath(bedrockMaterial, variant),
				BedrockRenderPolicy::blastHaloVariantPath(bedrockMaterial, variant),
				BedrockRenderPolicy::weaponImpactVariantPath(bedrockMaterial, variant)
			};
			for (const std::string &asset : variantAssets)
				if (!asset.empty() && _hdImageCache.usable(asset)) terrainAssets.insert(asset);
		}
		for (int variant = 0; variant < 3; ++variant)
		{
			const std::string tripletAssets[] =
			{
				BedrockRenderPolicy::weaponImpactCoreVariantPath(bedrockMaterial, variant),
				BedrockRenderPolicy::weaponImpactRimVariantPath(bedrockMaterial, variant),
				BedrockRenderPolicy::weaponImpactHaloVariantPath(bedrockMaterial, variant)
			};
			for (const std::string &asset : tripletAssets)
				if (!asset.empty() && _hdImageCache.usable(asset)) terrainAssets.insert(asset);
		}
		for (int variant = 0; variant < 4; ++variant)
		{
			const std::string asset = BedrockRenderPolicy::weaponDustVariantPath(bedrockMaterial, variant);
			if (!asset.empty() && _hdImageCache.usable(asset)) terrainAssets.insert(asset);
		}
		for (int variant = 0; variant < 5; ++variant)
		{
			const std::string asset = BedrockRenderPolicy::blastDustVariantPath(bedrockMaterial, variant);
			if (!asset.empty() && _hdImageCache.usable(asset)) terrainAssets.insert(asset);
		}
	}

	// Seed the manifest from the mission as generated. Keep the exact current
	// assets, but also remember which MCD records can legally change state later.
	for (int i = 0; i < _save->getMapSizeXYZ(); ++i)
	{
		Tile *tile = _save->getTile(i);
		if (!tile) continue;
		for (int p = O_FLOOR; p < O_MAX; ++p)
		{
			const TilePart part = static_cast<TilePart>(p);
			MapData *data = tile->getMapData(part);
			if (!data) continue;
			if (!BedrockRenderPolicy::ownsMapData(bedrockMaterial, data))
			{
				rememberRule(terrainAssets, findHdTerrainVisualRule(tile, part));
				remember(terrainAssets, findHdTerrainAsset(tile, part));
			}
			int mapDataId = -1, mapDataSetId = -1;
			tile->getMapData(&mapDataId, &mapDataSetId, part);
			if (mapDataId >= 0 && data->getDataset()) terrainSeeds[data->getDataset()].insert(mapDataId);
		}

		// Floor objects use an 8-frame presentation cycle.
		if (std::vector<BattleItem*> *inventory = tile->getInventory())
		{
			for (BattleItem *item : *inventory)
			{
				if (!item || !item->getRules()) continue;
				for (int anim = 0; anim < 8; ++anim)
					rememberSurfaceFrame("FLOOROB.PCK", item->getFloorSpriteIndex(_save, anim, tile->getShade()));
			}
		}

		// Existing smoke/fire contributes its complete four-frame cycle.
		if (tile->getSmoke() > 0 || tile->getFire() > 0)
		{
			int base = 0;
			if (!tile->getFire())
			{
				base += _save->getDepth() > 0 ? Mod::UNDERWATER_SMOKE_OFFSET : Mod::SMOKE_OFFSET;
				if (Mod::EXTENDED_SMOKE_OFFSET == 0)
					base += int(floor((tile->getSmoke() / 6.0) - 0.1));
				else if (Mod::EXTENDED_SMOKE_OFFSET == 1)
					base += int(floor((tile->getSmoke() / 6.0) - 0.1)) * 4;
				else
					base += (tile->getSmoke() - 1) / 5 * 4;
			}
			for (int phase = 0; phase < 4; ++phase)
				rememberSurfaceFrame("SMOKE.PCK", base + ((phase + tile->getAnimationOffset()) & 3));
		}
	}

	// V2: expand only the MCD records already present in this mission through
	// their legal animation/dead/alternate states. This catches doors, animated
	// craft/base pieces and semantic MCD_x_Fy overrides without loading whole
	// unrelated terrain datasets.
	size_t terrainRecordCount = 0;
	for (auto &datasetPair : terrainSeeds)
	{
		MapDataSet *dataset = datasetPair.first;
		if (!dataset) continue;
		std::vector<int> pending(datasetPair.second.begin(), datasetPair.second.end());
		std::set<int> visited;
		while (!pending.empty())
		{
			const int id = pending.back();
			pending.pop_back();
			if (id < 0 || (size_t)id >= dataset->getSize() || !visited.insert(id).second) continue;
			MapData *data = dataset->getObject((size_t)id);
			if (!data) continue;
			++terrainRecordCount;

			// BEDROCK owns the graphical representation of this dataset. Keep
			// traversing its MCD graph for gameplay/state completeness, but do not
			// resolve or preload individual visual frames.
			if (BedrockRenderPolicy::ownsMapData(bedrockMaterial, data))
			{
				const int alt = data->getAltMCD();
				const int dead = data->getDieMCD();
				if (alt >= 0 && alt != id) pending.push_back(alt);
				if (dead >= 0 && dead != id) pending.push_back(dead);
				continue;
			}

			const char *partName = "OBJECT";
			switch (data->getObjectType())
			{
			case O_FLOOR: partName = "FLOOR"; break;
			case O_WESTWALL: partName = "WESTWALL"; break;
			case O_NORTHWALL: partName = "NORTHWALL"; break;
			case O_OBJECT: partName = "OBJECT"; break;
			default: break;
			}

			for (int frame = 0; frame < 8; ++frame)
			{
				const int sprite = data->getSprite(frame);
				if (sprite < 0) continue;
				std::ostringstream spriteNumber, mcdName;
				spriteNumber << std::setw(3) << std::setfill('0') << sprite;
				mcdName << "MCD_" << std::setw(3) << std::setfill('0') << id << "_F" << frame << ".png";

				const std::string roots[] =
				{
					"Resources/TFTD_HD/RealHD/Datasets/" + dataset->getName() + "/",
					"Resources/TFTD_HD/Fixed/Terrain/" + dataset->getName() + "/",
					"Resources/TFTD_HD/Terrain/" + dataset->getName() + "/",
					"Resources/TFTD_HD/LegacyIndexed/Terrain/" + dataset->getName() + "/"
				};
				for (const std::string &root : roots)
				{
					const std::string semantic = root + partName + "/" + mcdName.str();
					const std::string specific = root + partName + "/" + spriteNumber.str() + ".png";
					const std::string flat = root + spriteNumber.str() + ".png";
					if (_hdImageCache.usable(semantic)) { terrainAssets.insert(semantic); break; }
					if (_hdImageCache.usable(specific)) { terrainAssets.insert(specific); break; }
					if (_hdImageCache.usable(flat)) { terrainAssets.insert(flat); break; }
				}
			}

			const int alt = data->getAltMCD();
			const int dead = data->getDieMCD();
			if (alt >= 0 && alt != id) pending.push_back(alt);
			if (dead >= 0 && dead != id) pending.push_back(dead);
		}
	}

	// Cursor changes immediately with selection/targeting. HIT.PCK is the small
	// transient impact set observed cold after Inventory; warm the complete sets.
	rememberSurfaceSet("CURSOR.PCK");
	rememberSurfaceSet("HIT.PCK");

	// DIAGNOSTIC/PREWARM V2: these two SurfaceSets are known mission-hot resources.
	// Their former [HD-PREWARM MISS] lines were late-load diagnostics, not VFS
	// failures. Put them in the initial manifest so a successful mission no longer
	// looks like it fell through the HD provider chain.
	rememberSurfaceSet("Pathfinding");
	rememberSurfaceFrame("SelectionArrow", 0);

	// Current mission units only. Full-body overlays remain state-aware; UnitParts
	// needs all body frames for the armor's actual Legacy sprite sheet. V1 tried
	// to recurse VFS directories, but FileMap virtual folders expose direct files,
	// not child-directory names; V2 therefore enumerates the exact dataset folder.
	if (std::vector<BattleUnit*> *units = _save->getUnits())
	{
		for (BattleUnit *unit : *units)
		{
			if (!unit || !unit->getArmor()) continue;
			const Armor *armor = unit->getArmor();
			remember(unitAssets, findHdUnitCombinedAsset(unit));
			remember(unitAssets, findHdUnitBodyAsset(unit));
			if (!armor->getFullBodySprite().empty() && _hdImageCache.usable(armor->getFullBodySprite()))
				remember(unitAssets, armor->getFullBodySprite());
			const BattleItem *left = unit->getLeftHandWeapon();
			const BattleItem *right = unit->getRightHandWeapon();
			remember(unitAssets, findHdUnitOverlayAsset(unit, "arms", "left", left));
			remember(unitAssets, findHdUnitOverlayAsset(unit, "arms", "right", right));
			remember(unitAssets, findHdUnitOverlayAsset(unit, "weapons", "left", left));
			remember(unitAssets, findHdUnitOverlayAsset(unit, "weapons", "right", right));

			if (!armor->getHdUnitPartsRoot().empty())
			{
				std::string dataset = armor->getSpriteSheet();
				const std::size_t dot = dataset.find_last_of('.');
				if (dot != std::string::npos) dataset.erase(dot);
				if (!dataset.empty())
				{
					unitPartDirs.insert(armor->getHdUnitPartsRoot() + "/" + dataset);
					unitPartDirs.insert("Resources/TFTD_HD/Fixed/UnitParts/" + dataset);
					unitPartDirs.insert("Resources/TFTD_HD/UnitParts/" + dataset);
					unitPartDirs.insert("Resources/TFTD_HD/LegacyIndexed/UnitParts/" + dataset);
				}
			}
		}
	}
	// REAL HD residency: UnitParts directories contain every pose and direction
	// for every armor in the mission. The old 512-sprite prewarm decoded all of
	// them, including invisible enemies and PNG fallback poses for GPU 3D units.
	// The measured mission decoded 2784 unit PNGs and retained 5.8 GiB on the
	// CPU before a soldier moved. Keep the directory set for asset routing, but
	// let the actual visual command request only the frames it draws. The HD
	// resolver and authored PNG route stay intact; this never selects Legacy.
	size_t deferredUnitFrames = 0;
	for (const std::string &dir : unitPartDirs)
	{
		for (const std::string &name : FileMap::getVFolderContents(dir))
			if (name.size() >= 4 && hdLower(name.substr(name.size() - 4)) == ".png")
				++deferredUnitFrames;
	}

	const uint64_t startUs = hdPerfNowUs();
	size_t terrainLoaded = 0, surfaceLoaded = 0, unitLoaded = 0;
	for (const std::string &path : terrainAssets) if (_hdImageCache.get(path)) ++terrainLoaded;
	for (const std::string &path : surfaceAssets) if (_hdImageCache.get(path)) ++surfaceLoaded;
	for (const std::string &path : unitAssets) if (_hdImageCache.get(path)) ++unitLoaded;
	const uint64_t elapsedUs = hdPerfNowUs() - startUs;

	Log(LOG_INFO) << "[HD-PREWARM V2] mission manifest="
		<< (terrainAssets.size() + surfaceAssets.size() + unitAssets.size())
		<< " loaded=" << (terrainLoaded + surfaceLoaded + unitLoaded)
		<< " terrain=" << terrainLoaded << "/" << terrainAssets.size()
		<< " surface=" << surfaceLoaded << "/" << surfaceAssets.size()
		<< " unit=" << unitLoaded << "/" << unitAssets.size()
		<< " mcdRecords=" << terrainRecordCount
		<< " unitSets=" << unitPartDirs.size()
		<< " unitFramesDeferred=" << deferredUnitFrames
		<< " decodeMs=" << (elapsedUs / 1000.0);
	_hdImageCache.markPrewarmComplete();
}

std::string Map::findHdTerrainAsset(const Tile *tile, TilePart part) const
{
	if (!Options::hdGraphics || !tile) return std::string();
	MapData *data = tile->getMapData(part);
	if (!data || !data->getDataset()) return std::string();

	const int visualFrame = tile->getCurrentFrame(part);
	const int sprite = data->getSprite(visualFrame);
	if (sprite < 0) return std::string();

	int mapDataId = -1, mapDataSetId = -1;
	tile->getMapData(&mapDataId, &mapDataSetId, part);
	const int currentSprite = tile->getCurrentSpriteIndex(part);
	const std::string dataset = data->getDataset()->getName();
	const std::string resolveKey = dataset + "|" + std::to_string((int)part) + "|" +
		std::to_string(mapDataId) + "|" + std::to_string(visualFrame) + "|" +
		std::to_string(sprite) + "|" + std::to_string(currentSprite);

	auto &perf = getHdPerfStats().current;
	++perf.mapResolveQueries;
	auto cached = _hdTerrainAssetResolveCache.find(resolveKey);
	if (cached != _hdTerrainAssetResolveCache.end())
	{
		++perf.mapResolveCacheHits;
		return cached->second;
	}
	const uint64_t resolveStart = hdPerfNowUs();

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

	auto tryRoot = [&](const std::string &root) -> std::string
	{
		// Most-specific convention: two MCD records may reuse one PCK frame.
		if (mapDataId >= 0)
		{
			std::ostringstream mcd;
			mcd << "MCD_" << std::setw(3) << std::setfill('0') << mapDataId << "_F" << visualFrame << ".png";
			const std::string semantic = root + partName + "/" + mcd.str();
			if (const_cast<Map*>(this)->_hdImageCache.usable(semantic)) return semantic;
		}

		const std::string specific = root + partName + "/" + number + ".png";
		if (const_cast<Map*>(this)->_hdImageCache.usable(specific)) return specific;
		const std::string flat = root + number + ".png";
		if (const_cast<Map*>(this)->_hdImageCache.usable(flat)) return flat;

		// A higher-priority authored-HD namespace may intentionally contain only
		// part of an animation. Keep its gameplay-synchronised frame before
		// falling through to the LegacyIndexed reference layer.
		if (currentSprite >= 0 && currentSprite != sprite)
		{
			std::ostringstream fallbackNumber;
			fallbackNumber << std::setw(3) << std::setfill('0') << currentSprite;
			const std::string fallbackSpecific = root + partName + "/" + fallbackNumber.str() + ".png";
			if (const_cast<Map*>(this)->_hdImageCache.usable(fallbackSpecific)) return fallbackSpecific;
			const std::string fallbackFlat = root + fallbackNumber.str() + ".png";
			if (const_cast<Map*>(this)->_hdImageCache.usable(fallbackFlat)) return fallbackFlat;
		}
		return std::string();
	};

	std::string resolved;
	// Canonical V1 provider namespace. REAL and DEBUG mirror each other here;
	// FileMap/VFS ordering decides which mod supplies the actual bytes.
	const std::string realHdRoot = "Resources/TFTD_HD/RealHD/Datasets/" + dataset + "/";
	if (resolved.empty()) resolved = tryRoot(realHdRoot);
	// Historical authored-HD namespaces remain read-only compatibility fallbacks
	// until the additive migration is fully validated.
	const std::string fixedRoot = "Resources/TFTD_HD/Fixed/Terrain/" + dataset + "/";
	if (resolved.empty()) resolved = tryRoot(fixedRoot);
	const std::string authoredRoot = "Resources/TFTD_HD/Terrain/" + dataset + "/";
	if (resolved.empty()) resolved = tryRoot(authoredRoot);
	const std::string legacyRoot = "Resources/TFTD_HD/LegacyIndexed/Terrain/" + dataset + "/";
	if (resolved.empty()) resolved = tryRoot(legacyRoot);

	_hdTerrainAssetResolveCache.emplace(resolveKey, resolved);
	perf.mapResolveUs += hdPerfNowUs() - resolveStart;
	return resolved;
}


std::string Map::findHdSurfaceSetAsset(const std::string &setName, int frame) const
{
	if (!Options::hdGraphics || frame < 0) return std::string();
	const std::string resolveKey = setName + "|" + std::to_string(frame);
	auto &perf = getHdPerfStats().current;
	++perf.mapResolveQueries;
	auto cached = _hdSurfaceSetAssetResolveCache.find(resolveKey);
	if (cached != _hdSurfaceSetAssetResolveCache.end())
	{
		++perf.mapResolveCacheHits;
		return cached->second;
	}
	const uint64_t resolveStart = hdPerfNowUs();

	const std::string resolved = hdResolveSurfacePng(
		[&](const std::string &path) { return const_cast<Map*>(this)->_hdImageCache.usable(path); },
		setName, frame).path;

	_hdSurfaceSetAssetResolveCache.emplace(resolveKey, resolved);
	perf.mapResolveUs += hdPerfNowUs() - resolveStart;
	return resolved;
}


const HdVisualRule *Map::findHdTerrainVisualRule(const Tile *tile, TilePart part) const
{
	if (!Options::hdGraphics || !tile) return nullptr;
	MapData *data = tile->getMapData(part);
	if (!data || !data->getDataset()) return nullptr;
	int mapDataId = -1, mapDataSetId = -1;
	tile->getMapData(&mapDataId, &mapDataSetId, part);
	const int sprite = tile->getCurrentSpriteIndex(part);
	const std::string dataset = data->getDataset()->getName();
	const std::string resolveKey = dataset + "|" + std::to_string((int)part) + "|" +
		std::to_string(mapDataId) + "|" + std::to_string(sprite) + "|d=" + std::to_string(_save->getDepth());

	auto &perf = getHdPerfStats().current;
	++perf.mapResolveQueries;
	auto cached = _hdTerrainRuleResolveCache.find(resolveKey);
	if (cached != _hdTerrainRuleResolveCache.end())
	{
		++perf.mapResolveCacheHits;
		return cached->second;
	}
	const uint64_t resolveStart = hdPerfNowUs();

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
		if (!rule.dataset.empty() && rule.dataset != dataset) continue;
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
	_hdTerrainRuleResolveCache.emplace(resolveKey, best);
	perf.mapResolveUs += hdPerfNowUs() - resolveStart;
	return best;
}

const HdVisualRule *Map::findHdSurfaceSetVisualRule(const std::string &setName, int frame) const
{
	if (!Options::hdGraphics) return nullptr;
	const std::string resolveKey = setName + "|" + std::to_string(frame) + "|d=" + std::to_string(_save->getDepth());
	auto &perf = getHdPerfStats().current;
	++perf.mapResolveQueries;
	auto cached = _hdSurfaceSetRuleResolveCache.find(resolveKey);
	if (cached != _hdSurfaceSetRuleResolveCache.end())
	{
		++perf.mapResolveCacheHits;
		return cached->second;
	}
	const uint64_t resolveStart = hdPerfNowUs();

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
	_hdSurfaceSetRuleResolveCache.emplace(resolveKey, best);
	perf.mapResolveUs += hdPerfNowUs() - resolveStart;
	return best;
}

bool Map::queueHdVisualRule(const HdVisualRule &rule, const std::string &instanceKey, int x, int y, int shade, bool rightHalfOnly, unsigned order, const GraphSubset *clipMask, long long epochTimeMs, int epochTurn)
{
	bool queuedAny = false;
	bool missingLayer = false;
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
		if (path.empty() || !_hdImageCache.usable(path))
		{
			missingLayer = true;
			hdTraceRoute("world-layer", path.empty() ? instanceKey : path, "TRY_NEXT_PROVIDER",
				"Configured HD layer missing or unreadable; retain lower-provider base");
			continue;
		}
		queueHdAsset(path, layer.nativeScale, layer.offsetX, layer.offsetY, x, y, shade, rightHalfOnly, order, clipMask, layer.colorMode, layer.materialProfile);
		queuedAny = true;
	}
	return queuedAny && !missingLayer && rule.replaceLegacy;
}

bool Map::queueHdTerrainOrConvention(const Tile *tile, TilePart part, int x, int y, int shade, bool rightHalfOnly, unsigned order, const GraphSubset *clipMask)
{
	const size_t overlayBegin = _hdDrawCommands.size();
	if (!Options::hdGraphics || !tile) return false;
	// REAL HD FOV V1 source ownership. Floors/objects follow floor discovery; wall parts
	// follow their own discovery bits. Hidden world geometry is consumed here, before
	// lighting/compositing, so the renderer no longer needs a mono-Z screen-space
	// blackout mask to hide undiscovered map content.
	const bool takeover = HdGpuBackend::instance().directWorldReady();
	if (takeover && !hdVisibilitySourceDiscovered(tile, part))
		return true;
	const int presentationShade = shade;
	const Tile *aboveForGrade = _save ? _save->getAboveTile(tile) : nullptr;
	// A -24 terrain support below is not an overhead floor (Tile::hasNoFloor).
	const bool coveredForGrade = aboveForGrade && !aboveForGrade->hasNoFloor(nullptr);
	if (const HdVisualRule *rule = findHdTerrainVisualRule(tile, part))
	{
		int mapDataId = -1, mapDataSetId = -1;
		tile->getMapData(&mapDataId, &mapDataSetId, part);
		const Position p = tile->getPosition();
		std::ostringstream key;
		key << "tile:" << p.x << ',' << p.y << ',' << p.z << ":part:" << (int)part << ":mcd:" << mapDataId;
		SavedBattleGame::HdVisualEpoch epoch;
		const bool hasEpoch = _save->getHdVisualTileEpoch(p, part, mapDataId, epoch);
		const size_t gradeBegin = _hdDrawCommands.size();
		const bool replaced = queueHdVisualRule(*rule, key.str(), x, y, presentationShade, rightHalfOnly, order, clipMask,
			hasEpoch ? (long long)epoch.timeMs : -1, hasEpoch ? epoch.turn : -1);
		MapData *ruleData = tile->getMapData(part);
		for (size_t i = gradeBegin; i < _hdDrawCommands.size(); ++i)
		{
			if (_hdDrawCommands[i].materialProfile == HdMaterialProfile::Auto)
			{
				if (ruleData && ruleData->getDataset())
					_hdDrawCommands[i].materialProfile = hdMaterialProfileForDataset(ruleData->getDataset()->getName(), coveredForGrade);
				else
					_hdDrawCommands[i].materialProfile = hdMaterialProfileForAssetPath(_hdDrawCommands[i].assetPath);
			}
		}
		if (replaced)
		{
			applyRealHdLightingTintToCommands(overlayBegin, tile);
			return true;
		}
		// Additive rules supplement the PNG base instead of forcing native pixels.
	}
	const std::string hd = findHdTerrainAsset(tile, part);
	if (!hd.empty())
	{
		queueHdAsset(hd, 0, 0, 0, x, y, presentationShade, rightHalfOnly, order, clipMask);
		if (!_hdDrawCommands.empty())
			_hdDrawCommands.back().materialProfile = hdMaterialProfileForAssetPath(hd);

		// The gameplay dataset remains untouched. Only the presentation command receives
		// a true-colour HD material profile. Dataset/frame routing is presentation-only.
		MapData *data = tile->getMapData(part);
		if (data && data->getDataset() && !_hdDrawCommands.empty())
		{
			const int visualFrame = tile->getCurrentFrame(part);
			const int sprite = data->getSprite(visualFrame);
			const std::string materialProfile = hdMaterialProfileForTerrain(data->getDataset()->getName(), sprite);
			HdMaterialProfile resolvedProfile = hdMaterialProfileFromString(materialProfile);
			if (resolvedProfile == HdMaterialProfile::Auto)
				resolvedProfile = hdMaterialProfileForDataset(data->getDataset()->getName(), coveredForGrade);
			_hdDrawCommands.back().materialProfile = resolvedProfile;
		}
		if (_hdDrawCommands.size() > overlayBegin + 1)
			std::rotate(_hdDrawCommands.begin() + overlayBegin, _hdDrawCommands.end() - 1, _hdDrawCommands.end());
		applyRealHdLightingTintToCommands(overlayBegin, tile);
		return true;
	}
	applyRealHdLightingTintToCommands(overlayBegin, tile);
	MapData *missing = tile->getMapData(part);
	if (missing && missing->getDataset())
		hdTraceRoute("terrain", missing->getDataset()->getName() + ":" +
			std::to_string(missing->getSprite(tile->getCurrentFrame(part))) + ":part:" + std::to_string(int(part)),
			"LEGACY_NATIVE", "No usable REAL_HD, Remastered or enlarged PNG for this terrain part", true);
	return false;
}

bool Map::queueHdSurfaceSetOrConvention(const std::string &setName, int frame, const std::string &instanceKey, int x, int y, int shade, bool rightHalfOnly, unsigned order, const GraphSubset *clipMask)
{
	const size_t overlayBegin = _hdDrawCommands.size();
	if (const HdVisualRule *rule = findHdSurfaceSetVisualRule(setName, frame))
	{
		const bool replaced = queueHdVisualRule(*rule, instanceKey, x, y, shade, rightHalfOnly, order, clipMask);
		if (replaced) return true;
		// Additive rules supplement the PNG base instead of forcing native pixels.
	}
	const std::string hd = findHdSurfaceSetAsset(setName, frame);
	if (!hd.empty())
	{
		// The transition SurfaceSets convention is x16, including 128x64 X1
		// frames. Inferring scale from a presumed 32-pixel width shrinks X1 x4.
		queueHdAsset(hd, 16, 0, 0, x, y, shade, rightHalfOnly, order, clipMask);
		if (!_hdDrawCommands.empty() && _hdDrawCommands.back().materialProfile == HdMaterialProfile::Auto)
			_hdDrawCommands.back().materialProfile = hdMaterialProfileForSurfaceSet(setName);
		if (_hdDrawCommands.size() > overlayBegin + 1)
			std::rotate(_hdDrawCommands.begin() + overlayBegin, _hdDrawCommands.end() - 1, _hdDrawCommands.end());
		return true;
	}
	hdTraceRoute("surface-set", setName + ":" + std::to_string(frame), "LEGACY_NATIVE",
		"No usable configured HD layer, Remastered or enlarged PNG", true);
	return false;
}

void Map::drawHdSurfaceSetOrLegacy(SurfaceRaw<Uint8> destination, const std::string &setName, int frame, SurfaceRaw<const Uint8> legacy, int x, int y, int shade, bool rightHalfOnly, int newBaseColor, bool postVisibility)
{
	const unsigned order = nextDrawSequence();
	const size_t commandBegin = _hdDrawCommands.size();
	std::ostringstream key;
	key << "set:" << setName << ":xy:" << x << ',' << y;
	if (!queueHdSurfaceSetOrConvention(setName, frame, key.str(), x, y, shade, rightHalfOnly, order))
	{
		hdTraceRoute("surface-set", setName + ":" + std::to_string(frame), "LEGACY_NATIVE",
            "HD providers unavailable or additive native base required", true);
		if (!queueLegacyIfGpuReady(legacy, x, y, shade, rightHalfOnly, order, newBaseColor))
		{
			Surface::blitRaw(destination, legacy, x, y, shade, rightHalfOnly, newBaseColor);
			markSourceDrawOrder(legacy, x, y, rightHalfOnly, order);
			++getHdPerfStats().current.mapCpuLegacyBlits;
		}
	}
	for (size_t i = commandBegin; i < _hdDrawCommands.size(); ++i)
		_hdDrawCommands[i].legacyBaseColor = newBaseColor;
	if (postVisibility)
	{
		for (size_t i = commandBegin; i < _hdDrawCommands.size(); ++i)
			_hdDrawCommands[i].postVisibility = true;
	}
}

void Map::blitHdOverlays(SDL_Surface *destination)
{
	if (!Options::hdGraphics || _hdDrawCommands.empty() || !destination || destination->format->BitsPerPixel != 32) return;
	const int mapScale = std::max(1, _spriteWidth / 32);
	SDL_Rect mapClip = { (Sint16)getX(), (Sint16)getY(), (Uint16)getWidth(), (Uint16)std::min(getHeight(), _visibleMapHeight) };
	const SDL_Color *activePalette = getPalette();

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
		// REAL HD LIGHTING V2: compatibility CPU composition uses the same
		// linear presentation light as the D3D11 world path.
		const double lightFactor = legacyLight;

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
					const int shade = std::max(0, std::min(16, cmd.shade));
					Uint8 finalIndex;
					if (cmd.legacyBaseColor > 0)
					{
						const int newShade = (srcIndex & 0x0F) + shade;
						const int base = std::max(0, std::min(16, cmd.legacyBaseColor));
						finalIndex = (newShade & 0xF0) ? 0x0F : (((base - 1) << 4) | newShade);
					}
					else
					{
						const Uint8 shaded = (Uint8)(srcIndex + shade);
						finalIndex = ((shaded ^ srcIndex) & 0xF0) ? 0x0F : shaded;
					}
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
					// Modern true-colour material grade is applied before OXCE local shade.
					// The HD material profile never quantizes through the Legacy palette.
					if (colorMode == HdColorMode::Environment && Options::hdMaterialGrade)
					{
						HdMaterialProfile profile = cmd.materialProfile;
						if (profile == HdMaterialProfile::Auto) profile = hdMaterialProfileForAssetPath(cmd.assetPath);
						hdApplyMaterialGrade(sr, sg, sb, hdMaterialGradeGet(profile, _save ? _save->getDepth() : 0));
					}
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
    if (_save) HdCausticSettings::instance().context(_save->getDepth(), _save->getGlobalShade());

	HdGpuBackend &gpu = HdGpuBackend::instance();
	if (!Options::hdGraphics || !screen || !gpu.frameActive() || physicalWidth <= 0 || physicalHeight <= 0) return false;

	// Map::draw() may have replaced the entire tactical view with the OXCE
	// hidden-movement/end-turn message.  The GPU BEDROCK path is generated
	// independently from _hdDrawCommands, so clearing the sprite queue alone is
	// not enough: drawing here would leak the tactical world over that message.
	if (_hdPhysicalMapSuppressed) return true;

	const uint64_t perfStart = hdPerfNowUs();
	auto &perf = getHdPerfStats().current;
	perf.mapSeedUs = 0;
	perf.mapCompositeUs = 0;
	perf.mapCacheBlitUs = 0;
	perf.mapCacheHit = false;
	perf.mapImageCpuCacheBytes = _hdImageCache.estimatedCpuBytes();
	perf.mapImageCpuCacheCount = (unsigned)_hdImageCache.imageCount();
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

	// The final physical base still contains P4's correctly scaled CPU fallback map,
	// but its painter-order raster is no longer an authority for authored HD. It is
	// uploaded only for the explicit compatibility bridge. The Battlescape frame owns
	// one independent geometric-depth range shared by every real-geometry provider.
	const int hdMapW = _save ? _save->getMapSizeX() : 0;
	const int hdMapH = _save ? _save->getMapSizeY() : 0;
	const int hdMapZCount = _save ? std::max(1, _save->getMapSizeZ()) : 1;
	const float worldDepthMin = -64.0f;
	const float worldDepthMax = (float)(hdMapW * Position::TileXY + hdMapH * Position::TileXY + hdMapZCount * Position::TileZ) + 64.0f;
	const SDL_Color *activePalette = getPalette();
	const SDL_Color *neutralPalette = hdNeutralBattlescapePalette(_game, activePalette);
	const uint64_t perfBeginMapStart = hdPerfNowUs();
	if (!gpu.beginMap(getWidth(), getHeight(), worldDepthMin, worldDepthMax,
		activePalette, neutralPalette, px0, py0, px1, py1))
		return false;
	perf.mapBeginMapUs += hdPerfNowUs() - perfBeginMapStart;

	// Collision LOFTs have no visible depth ownership in REAL HD. Do not expand
	// their geometry on the CPU for every redraw; only colour/depth providers
	// submit geometry to this GPU pass.

	const int mapScale = std::max(1, _spriteWidth / 32);
	const double renderToPhysicalX = std::max(0.0001, sx / (double)HdRenderSpace::Scale);
	const double renderToPhysicalY = std::max(0.0001, sy / (double)HdRenderSpace::Scale);

	// HD LOCAL LIGHTS V2 — UNIVERSAL BATTLESCAPE PRESENTATION. Build the source list
	// once per physical pass, independently from BEDROCK. Every subsequent map sprite
	// (Legacy indexed or authored HD) receives this same list, so future texture families
	// inherit local lights automatically without material-specific plumbing.
	const int hdViewLevel = _camera ? std::max(0, std::min(hdMapZCount - 1, _camera->getViewLevel())) : 0;
	const uint64_t perfLightBuildStart = hdPerfNowUs();
	std::vector<HdGpuLocalLight> hdLocalLights;
	const auto &localLightRules = hdLocalLightRules();
	if (Options::hdLocalLightsEnabled && _save && _camera && !localLightRules.empty())
	{
		const Position lightCameraPos = _camera->getMapOffset();
		for (int lz = 0; lz <= hdViewLevel && hdLocalLights.size() < 24u; ++lz)
		{
			for (int ly = 0; ly < hdMapH && hdLocalLights.size() < 24u; ++ly)
			{
				for (int lx = 0; lx < hdMapW && hdLocalLights.size() < 24u; ++lx)
				{
					Tile *lt = _save->getTile(Position(lx, ly, lz));
					if (!lt) continue;
					// TAKEOVER V3: an undiscovered lamp must not brighten already-visible
					// pixels outside its hidden cell and thereby disclose hidden geometry.
					if (!RealHdWorldPresentation::allows(
						lt, O_FLOOR, RealHdWorldPresentationClass::DynamicWorldEffect)) continue;
					for (int p = O_FLOOR; p < O_MAX && hdLocalLights.size() < 24u; ++p)
					{
						const TilePart part = static_cast<TilePart>(p);
						MapData *data = lt->getMapData(part);
						if (!data || !data->getDataset()) continue;
						int mapDataId = -1, mapDataSetId = -1;
						lt->getMapData(&mapDataId, &mapDataSetId, part);
						if (mapDataId < 0) continue;
						const std::string dataset = hdLower(data->getDataset()->getName());
						const auto ds = localLightRules.find(dataset);
						if (ds == localLightRules.end()) continue;
						const auto fr = ds->second.find(mapDataId);
						if (fr == ds->second.end()) continue;

						const HdLocalLightRule &rule = fr->second;
						const float phase = (float)(_animFrame + rule.phaseFrames) / (float)rule.periodFrames;
						const float pulse = 1.0f + (rule.pulsePermille / 1000.0f) * std::sin(phase * 6.28318530718f);
						HdGpuLocalLight light;
						light.x = lx + 0.5f;
						light.y = ly + 0.5f;
						light.z = lz + 0.5f;
						const float localRadiusScale = std::max(0.0f, Options::hdLocalLightRadiusPermille / 1000.0f);
						const float localIntensityScale = std::max(0.0f, Options::hdLocalLightIntensityPermille / 1000.0f);
						light.radius = rule.radiusMilliTiles / 1000.0f * localRadiusScale;
						light.r = rule.r / 255.0f;
						light.g = rule.g / 255.0f;
						light.b = rule.b / 255.0f;
						light.intensity = std::max(0.0f, rule.intensityPermille / 1000.0f * pulse * localIntensityScale);

						Position lightScreen;
						_camera->convertMapToScreen(Position(lx, ly, lz), &lightScreen);
						lightScreen += lightCameraPos;
						const float logicalCenterX = (float)(logicalMapX + lightScreen.x + _spriteWidth / 2);
						const float logicalCenterY = (float)(logicalMapY + lightScreen.y + _spriteHeight - _spriteWidth / 4);
						light.screenX = (float)screen->logicalToPhysicalX((int)std::lround(logicalCenterX));
						light.screenY = (float)screen->logicalToPhysicalY((int)std::lround(logicalCenterY));
						light.screenRadiusX = std::max(1.0f, light.radius * (_spriteWidth * 0.5f) * (float)sx);
						light.screenRadiusY = std::max(1.0f, light.radius * (_spriteWidth * 0.25f) * (float)sy);
						hdLocalLights.push_back(light);
					}
				}
			}
		}
	}
	perf.mapLocalLightBuildUs += hdPerfNowUs() - perfLightBuildStart;

	// HD SMOKE VOLUME V1 source list. The pass is visual-only and consumes exactly
	// OXCE Tile::getSmoke(); it never writes density, LOS, damage or discovery state.
	const uint64_t perfSmokeBuildStart = hdPerfNowUs();
	std::vector<HdGpuSmokeBlob> hdSmokeBlobs;
	std::vector<HdGpuSmokeBlob> hdSedimentBlobs;
	const bool underwaterCloud = _save && _save->getDepth() > 0 && Options::hdUnderwaterSmokeEnabled;
	const bool smokeVolumeActive = Options::hdSmokeVolumeEnabled || underwaterCloud;
	if (smokeVolumeActive && _save && _camera)
	{
		const Position smokeCameraPos = _camera->getMapOffset();
		const float smokeRadiusScale = std::max(0.0f, Options::hdSmokeRadiusPermille / 1000.0f);
		const float smokeOpacityScale = std::max(0.0f, Options::hdSmokeOpacityPermille / 1000.0f);
		for (Tile *st : _hdPresentationTiles)
		{
			if(!st || st->getSmoke()<=0 || st->getFire() ||
				!RealHdWorldPresentation::allows(st, O_FLOOR, RealHdWorldPresentationClass::DynamicWorldEffect)) continue;
			const Position pos = st->getPosition();
			if ((!_camera->getShowAllLayers() && pos.z > hdViewLevel) ||
				(_camera->getShowSingleLayer() && pos.z != hdViewLevel)) continue;
			Position ss; _camera->convertMapToScreen(pos,&ss); ss+=smokeCameraPos;
			if (ss.x < -2*_spriteWidth || ss.x > getWidth()+_spriteWidth ||
				ss.y < -2*_spriteHeight || ss.y > _visibleMapHeight+_spriteHeight) continue;
			if (hdSmokeBlobs.size() >= 256u) break;
			HdGpuSmokeBlob b;
			b.screenX=(float)screen->logicalToPhysicalX(logicalMapX+ss.x+_spriteWidth/2);
			b.screenY=(float)screen->logicalToPhysicalY(logicalMapY+ss.y+_spriteHeight-_spriteWidth/3);
			const float density=std::max(0.0f,std::min(1.0f,st->getSmoke()/15.0f));
			b.screenRadiusX=std::max(1.0f,(0.72f+0.38f*density)*_spriteWidth*(float)sx*smokeRadiusScale);
			b.screenRadiusY=std::max(1.0f,(0.52f+0.34f*density)*(_spriteWidth*0.5f)*(float)sy*smokeRadiusScale);
			b.density=density; b.opacity=(underwaterCloud ? 0.42f : 0.58f)*smokeOpacityScale;
			b.phase=SDL_GetTicks()*0.001f+pos.x*0.71f+pos.y*1.13f+pos.z*2.3f;
			if (underwaterCloud)
			{
				// Suspended particles disperse around the floor, not as a tall plume.
				b.screenY += (float)st->getTerrainLevel()*mapScale*(float)sy;
				b.screenRadiusX *= 1.15f;
				b.screenRadiusY *= 1.25f;
				const float light = std::clamp(realHdLightingAt(st), 0.0f, 1.0f);
				float tr=1, tg=1, tb=1; realHdLightingTintAt(st,tr,tg,tb);
				b.r=0.47f*light*tr; b.g=0.57f*light*tg; b.b=0.59f*light*tb;
			}
			hdSmokeBlobs.push_back(b);
		}
	}

	// P2ZB: visual-only impact sediment. OXCE remains the sole gameplay authority.
	// The visual cloud is derived from the physical blast footprint instead of using
	// the crater diameter as a radius. Direct weapon-impact markers are decoded back
	// to their real 0.8-tile diameter before any sediment radius is calculated.
	auto decodeSedimentRadius = [](int storedDiameterMilliTiles, bool &directImpact, float &damageRadius, float &sedimentRadius)
	{
		directImpact = storedDiameterMilliTiles >= 1000000;
		const int diameterMilliTiles = directImpact
			? std::max(1, storedDiameterMilliTiles - 1000000)
			: std::max(1, storedDiameterMilliTiles);
		const float diameterTiles = diameterMilliTiles / 1000.0f;
		if (directImpact)
		{
			// A direct hit stores its true presentation diameter after the 1,000,000 marker.
			damageRadius = std::max(0.10f, diameterTiles * 0.5f);
		}
		else
		{
			// Blast stamps store the complete OXCE footprint width 2R+1.
			damageRadius = std::max(0.0f, (diameterTiles - 1.0f) * 0.5f);
		}
		sedimentRadius = std::max(0.50f, damageRadius * 1.15f);
	};

	// Seed on first presentation: loading old craters must not replay explosions.
	if (_save)
	{
		const auto &stamps = _save->getBedrockCraterStamps();
		const auto &diameters = _save->getBedrockCraterDiametersMilliTiles();
		const Uint32 now = SDL_GetTicks();
		if (!_hdCraterEventsInitialized || stamps.size() < _hdObservedCraterCount)
		{
			_hdObservedCraterCount = stamps.size();
			_hdCraterEventsInitialized = true;
			_hdSedimentBursts.clear();
		}
		for (size_t k = _hdObservedCraterCount; k < stamps.size(); ++k)
		{
			Tile *source = _save->getTile(stamps[k].toTile());
			if (!underwaterCloud || !source) continue;
			if (_hdSedimentBursts.size() >= 64u) _hdSedimentBursts.erase(_hdSedimentBursts.begin());
			const int storedDiameter = k < diameters.size() ? diameters[k] : 1000;
			_hdSedimentBursts.push_back({stamps[k], now, storedDiameter, _save->getTurn(), false});
			bool directImpact = false;
			float damageRadius = 0.0f, sedimentRadius = 0.0f;
			decodeSedimentRadius(storedDiameter, directImpact, damageRadius, sedimentRadius);
			Log(LOG_INFO) << "[REAL HD P2ZB SEDIMENT] crater=" << k
				<< " kind=" << (directImpact ? "DIRECT_IMPACT" : "BLAST")
				<< " damageRadius=" << damageRadius
				<< " sedimentRadius=" << sedimentRadius
				<< " scale=1.15 density=1.20 opacityScale=0.55 expansion=FIXED lifetime=CREATION_AND_NEXT_TURN source=OXCE_IMPACT visualOnly=1";
		}
		_hdObservedCraterCount = stamps.size();
		_hdSedimentBursts.erase(std::remove_if(_hdSedimentBursts.begin(), _hdSedimentBursts.end(),
			[this](const HdSedimentBurst &e) { return _save->getTurn()-e.turn >= 2; }), _hdSedimentBursts.end());

		if (underwaterCloud && _camera)
		for (auto &e : _hdSedimentBursts)
		{
			const Position pos = e.voxel.toTile();
			Tile *st = _save->getTile(pos);
			if (!st) continue;

			bool directImpact = false;
			float damageRadius = 0.0f, sedimentRadius = 0.0f;
			decodeSedimentRadius(e.diameter, directImpact, damageRadius, sedimentRadius);

			// Keep events until expiry, even when simulation smoke hides the center.
			// A visible peripheral tile can establish observation of a broad cloud.
			if (!e.observed)
			{
				const int reach = std::clamp((int)std::ceil(sedimentRadius), 1, 16);
				for (int dy=-reach; dy<=reach && !e.observed; ++dy)
				for (int dx=-reach; dx<=reach && !e.observed; ++dx)
				{
					if (dx*dx+dy*dy>reach*reach) continue;
					Tile *seen=_save->getTile(Position(pos.x+dx,pos.y+dy,pos.z));
					if (seen && seen->isDiscovered(O_FLOOR) && (seen->getVisible() || _save->getDebugMode())) e.observed=true;
				}
			}
			if (!e.observed || !st->isDiscovered(O_FLOOR)) continue;
			if ((!_camera->getShowAllLayers() && pos.z > hdViewLevel) || (_camera->getShowSingleLayer() && pos.z != hdViewLevel)) continue;
			Position ss; _camera->convertMapToScreen(pos, &ss); ss += _camera->getMapOffset();
			if (ss.x < -64*_spriteWidth || ss.x > getWidth()+64*_spriteWidth ||
				ss.y < -64*_spriteHeight || ss.y > _visibleMapHeight+64*_spriteHeight) continue;

			const float elapsed = Uint32(now-e.started)*0.001f;
			const float age = std::min(elapsed, 4.0f);
			const float fade = std::min(1.0f, elapsed/0.12f)*(_save->getTurn() > e.turn ? 0.65f : 1.0f);
			// P2ZB: no 45% -> 100% four-second geometric growth. The cloud starts
			// at its final envelope immediately; only the small vertical drift/noise remains.
			const float size = sedimentRadius;
			const float light = 0.22f + 0.78f*std::clamp((15.0f-_save->getGlobalShade())/15.0f, 0.0f, 1.0f);
			const float sedimentRadiusScale = std::max(0.0f, Options::hdSmokeRadiusPermille / 1000.0f);
			const float screenRadiusX = std::max(1.0f, _spriteWidth*(float)sx*size*0.72f*sedimentRadiusScale);
			const float screenRadiusY = std::max(1.0f, screenRadiusX*0.62f*(float)(sy/sx));
			const float lobeOffsetX = screenRadiusX*0.38f;
			for (int lobe=0; lobe<3 && hdSedimentBlobs.size()<192u; ++lobe)
			{
				const float side = float(lobe-1);
				HdGpuSmokeBlob b;
				b.screenX = (float)screen->logicalToPhysicalX(logicalMapX+ss.x+_spriteWidth/2) + side*lobeOffsetX;
				b.screenY = (float)screen->logicalToPhysicalY(logicalMapY+ss.y+_spriteHeight-_spriteWidth/3)
					+ ((float)st->getTerrainLevel()*mapScale-age*1.8f)*(float)sy;
				b.screenRadiusX = screenRadiusX;
				b.screenRadiusY = screenRadiusY;
				b.density=1.20f; b.opacity=0.55f*fade*Options::hdSmokeOpacityPermille/1000.0f;
				b.phase=elapsed*0.65f+pos.x*0.71f+pos.y*1.13f+lobe*2.7f;
				b.r=0.38f*light; b.g=0.43f*light; b.b=0.40f*light;
				hdSedimentBlobs.push_back(b);
			}
		}
	}
	perf.mapSmokeBuildUs += hdPerfNowUs() - perfSmokeBuildStart;
	if (smokeVolumeActive)
	{
		static int lastSmokeBlobCount = -1;
		static int lastSedimentBlobCount = -1;
		const int smokeBlobCount = (int)hdSmokeBlobs.size();
		const int sedimentBlobCount = (int)hdSedimentBlobs.size();
		if (smokeBlobCount != lastSmokeBlobCount || sedimentBlobCount != lastSedimentBlobCount)
		{
			Log(LOG_INFO) << "[REAL HD P2ZA SMOKE BUDGET] simulationBlobs=" << smokeBlobCount
				<< " sedimentBlobs=" << sedimentBlobCount
				<< " total=" << (smokeBlobCount + sedimentBlobCount)
				<< " simulationCap=256 sedimentCap=192"
				<< " opacity=" << (Options::hdSmokeOpacityPermille / 1000.0)
				<< " radius=" << (Options::hdSmokeRadiusPermille / 1000.0)
				<< " replaceLegacy=" << (Options::hdSmokeReplaceLegacy ? "ON" : "OFF")
				<< " underwaterCloud=" << (underwaterCloud ? 1 : 0)
				<< " underwaterDissipation=" << Options::underwaterSmokeDissipation
				<< " densitySource=SIMULATION_PLUS_VISUAL_SEDIMENT_SEPARATE_BUDGETS";
			lastSmokeBlobCount = smokeBlobCount;
			lastSedimentBlobCount = sedimentBlobCount;
		}
	}

	// BEDROCK MATERIAL SETS + EXPOSED FACES V1:
	// OXCE remains the gameplay authority. REAL HD owns presentation only when a
	// complete MaterialSet exists. Terrain profiles are treated as the exterior skin
	// of a solid implicit substrate: continuous edges stay open internally, while
	// mismatched shared edges expose a vertical substrate face. This deliberately
	// avoids the Legacy illusion of a hollow half-cell beneath slopes.
	const uint64_t perfBedrockOwnerStart = hdPerfNowUs();
	const BedrockMaterial bedrockMaterial = BedrockRenderPolicy::resolve(_save);
	const std::string bedrockAssetPath = BedrockRenderPolicy::assetPath(bedrockMaterial);
	bool bedrockDrawn = false;
	if (bedrockMaterial != BedrockMaterial::None && !bedrockAssetPath.empty())
	{
		HdImage *bedrockImage = _hdImageCache.get(bedrockAssetPath);
		const std::string bedrockNormalPath = BedrockRenderPolicy::normalPath(bedrockMaterial);
		const std::string bedrockRoughnessPath = BedrockRenderPolicy::roughnessPath(bedrockMaterial);
		const std::string bedrockAoPath = BedrockRenderPolicy::aoPath(bedrockMaterial);
		const std::string bedrockCraterCoreMaskPath = BedrockRenderPolicy::craterCoreMaskPath(bedrockMaterial);
		const std::string bedrockCraterRimMaskPath = BedrockRenderPolicy::craterRimMaskPath(bedrockMaterial);
		const std::string bedrockBlastHaloMaskPath = BedrockRenderPolicy::blastHaloMaskPath(bedrockMaterial);
		const std::string bedrockCraterMaskPath = BedrockRenderPolicy::craterMaskPath(bedrockMaterial); // V1-V3 fallback only
		const std::string bedrockVerticalAssetPath = BedrockRenderPolicy::verticalAssetPath(bedrockMaterial);
		const std::string bedrockVerticalNormalPath = BedrockRenderPolicy::verticalNormalPath(bedrockMaterial);
		const std::string bedrockVerticalRoughnessPath = BedrockRenderPolicy::verticalRoughnessPath(bedrockMaterial);
		const std::string bedrockVerticalAoPath = BedrockRenderPolicy::verticalAoPath(bedrockMaterial);

		HdImage *bedrockNormal = bedrockNormalPath.empty() ? nullptr : _hdImageCache.get(bedrockNormalPath);
		HdImage *bedrockRoughness = bedrockRoughnessPath.empty() ? nullptr : _hdImageCache.get(bedrockRoughnessPath);
		HdImage *bedrockAo = bedrockAoPath.empty() ? nullptr : _hdImageCache.get(bedrockAoPath);
		auto optionalHdMask = [&](const std::string &path) -> HdImage*
		{
			return (!path.empty() && FileMap::fileExists(path)) ? _hdImageCache.get(path) : nullptr;
		};
		HdImage *bedrockCraterCoreMask = optionalHdMask(bedrockCraterCoreMaskPath);
		HdImage *bedrockCraterRimMask = optionalHdMask(bedrockCraterRimMaskPath);
		HdImage *bedrockBlastHaloMask = optionalHdMask(bedrockBlastHaloMaskPath);
		HdImage *bedrockCraterMask = optionalHdMask(bedrockCraterMaskPath);

		// BEDROCK impact authored pools. Blasts stay A..G. Weapon impacts now prefer
		// explicit CORE/RIM/HALO triplets A..C, with legacy IMPACT_A..G kept only as fallback.
		std::string blastCoreVariantPaths[7], blastRimVariantPaths[7], blastHaloVariantPaths[7], weaponImpactVariantPaths[7];
		std::string weaponImpactCoreVariantPaths[3], weaponImpactRimVariantPaths[3], weaponImpactHaloVariantPaths[3];
		HdImage *blastCoreVariants[7] = { nullptr }, *blastRimVariants[7] = { nullptr }, *blastHaloVariants[7] = { nullptr };
		HdImage *weaponImpactVariants[7] = { nullptr };
		HdImage *weaponImpactCoreVariants[3] = { nullptr }, *weaponImpactRimVariants[3] = { nullptr }, *weaponImpactHaloVariants[3] = { nullptr };
		for (int variant = 0; variant < 7; ++variant)
		{
			blastCoreVariantPaths[variant] = BedrockRenderPolicy::blastCoreVariantPath(bedrockMaterial, variant);
			blastRimVariantPaths[variant] = BedrockRenderPolicy::blastRimVariantPath(bedrockMaterial, variant);
			blastHaloVariantPaths[variant] = BedrockRenderPolicy::blastHaloVariantPath(bedrockMaterial, variant);
			weaponImpactVariantPaths[variant] = BedrockRenderPolicy::weaponImpactVariantPath(bedrockMaterial, variant);
			blastCoreVariants[variant] = optionalHdMask(blastCoreVariantPaths[variant]);
			blastRimVariants[variant] = optionalHdMask(blastRimVariantPaths[variant]);
			blastHaloVariants[variant] = optionalHdMask(blastHaloVariantPaths[variant]);
			weaponImpactVariants[variant] = optionalHdMask(weaponImpactVariantPaths[variant]);
		}
		for (int variant = 0; variant < 3; ++variant)
		{
			weaponImpactCoreVariantPaths[variant] = BedrockRenderPolicy::weaponImpactCoreVariantPath(bedrockMaterial, variant);
			weaponImpactRimVariantPaths[variant] = BedrockRenderPolicy::weaponImpactRimVariantPath(bedrockMaterial, variant);
			weaponImpactHaloVariantPaths[variant] = BedrockRenderPolicy::weaponImpactHaloVariantPath(bedrockMaterial, variant);
			weaponImpactCoreVariants[variant] = optionalHdMask(weaponImpactCoreVariantPaths[variant]);
			weaponImpactRimVariants[variant] = optionalHdMask(weaponImpactRimVariantPaths[variant]);
			weaponImpactHaloVariants[variant] = optionalHdMask(weaponImpactHaloVariantPaths[variant]);
		}
		HdImage *bedrockVertical = bedrockVerticalAssetPath.empty() ? nullptr : _hdImageCache.get(bedrockVerticalAssetPath);
		HdImage *bedrockVerticalNormal = bedrockVerticalNormalPath.empty() ? nullptr : _hdImageCache.get(bedrockVerticalNormalPath);
		HdImage *bedrockVerticalRoughness = bedrockVerticalRoughnessPath.empty() ? nullptr : _hdImageCache.get(bedrockVerticalRoughnessPath);
		HdImage *bedrockVerticalAo = bedrockVerticalAoPath.empty() ? nullptr : _hdImageCache.get(bedrockVerticalAoPath);

		const bool bedrockPbrReady = bedrockNormal && bedrockNormal->loaded && bedrockNormal->hasVisiblePixels
			&& bedrockRoughness && bedrockRoughness->loaded && bedrockRoughness->hasVisiblePixels
			&& bedrockAo && bedrockAo->loaded && bedrockAo->hasVisiblePixels;
		const bool bedrockVerticalReady = bedrockVertical && bedrockVertical->loaded
			&& bedrockVertical->width && bedrockVertical->height && bedrockVertical->hasVisiblePixels;
		const bool bedrockVerticalPbrReady = bedrockVerticalNormal && bedrockVerticalNormal->loaded && bedrockVerticalNormal->hasVisiblePixels
			&& bedrockVerticalRoughness && bedrockVerticalRoughness->loaded && bedrockVerticalRoughness->hasVisiblePixels
			&& bedrockVerticalAo && bedrockVerticalAo->loaded && bedrockVerticalAo->hasVisiblePixels;

		if (bedrockImage && bedrockImage->loaded && bedrockImage->width && bedrockImage->height
			&& bedrockImage->hasVisiblePixels && bedrockVerticalReady)
		{
			const int mapW = _save->getMapSizeX();
			const int mapH = _save->getMapSizeY();
			const int mapZCount = std::max(1, _save->getMapSizeZ());
			const int maxMapZ = std::max(0, mapZCount - 1);
			const int viewLevel = std::max(0, std::min(maxMapZ, _camera->getViewLevel()));
			const size_t cellCount = (size_t)mapW * (size_t)mapH;

			// BEDROCK IMPACT VARIANTS V5. One HD world-space material field now carries:
			// R = blast crater core, G = displaced-sand rim, B = blast halo/ripples,
			// A = direct weapon impact. Nothing here changes OXCE gameplay geometry.
			const uint64_t perfImpactMaskPrepStart = hdPerfNowUs();
			uint64_t impactMaskScanPixels = 0;
			unsigned impactMaskScans = 0;
			auto maskReady = [](const HdImage *img) -> bool
			{
				return img && img->loaded && img->width && img->height && img->hasVisiblePixels;
			};

			// V4 root masks remain Set A for backward-compatible runtime packs. Explicit
			// BlastSets/CORE_A etc. override them when present.
			if (!maskReady(blastCoreVariants[0]))
				blastCoreVariants[0] = maskReady(bedrockCraterCoreMask) ? bedrockCraterCoreMask : (maskReady(bedrockCraterMask) ? bedrockCraterMask : nullptr);
			if (!maskReady(blastRimVariants[0]) && maskReady(bedrockCraterRimMask)) blastRimVariants[0] = bedrockCraterRimMask;
			if (!maskReady(blastHaloVariants[0]) && maskReady(bedrockBlastHaloMask)) blastHaloVariants[0] = bedrockBlastHaloMask;

			struct ImpactMaskBounds
			{
				HdImage *master = nullptr;
				unsigned minX = 0, minY = 0, maxX = 0, maxY = 0, spanX = 1, spanY = 1;
				bool ready = false;
			};
			struct BlastMaskSet
			{
				HdImage *core = nullptr, *rim = nullptr, *halo = nullptr;
				ImpactMaskBounds bounds;
			};
			struct WeaponMaskSet
			{
				HdImage *core = nullptr, *rim = nullptr, *halo = nullptr, *legacy = nullptr;
				ImpactMaskBounds bounds;
				bool authoredTriplet = false;
			};

			auto influenceAt = [](const HdImage *img, unsigned x, unsigned y) -> unsigned
			{
				if (!img || x >= img->width || y >= img->height) return 0u;
				const size_t src = ((size_t)y * img->width + x) * 4u;
				const unsigned r = img->rgba[src + 0], g = img->rgba[src + 1], b = img->rgba[src + 2], a = img->rgba[src + 3];
				const unsigned luma = (54u * r + 183u * g + 19u * b) >> 8;
				return (luma * a + 127u) / 255u;
			};
			auto influenceAtUv = [&](const HdImage *img, float u, float v) -> unsigned
			{
				if (!maskReady(img)) return 0u;
				const unsigned x = std::min(img->width - 1u, (unsigned)std::max(0, (int)std::floor(u * img->width)));
				const unsigned y = std::min(img->height - 1u, (unsigned)std::max(0, (int)std::floor(v * img->height)));
				return influenceAt(img, x, y);
			};
			auto buildBounds = [&](HdImage *master) -> ImpactMaskBounds
			{
				ImpactMaskBounds out;
				out.master = master;
				if (!maskReady(master)) return out;
				// PERF FOUNDATION V1: the exact luma*alpha>=4 bounds are computed
				// once by HdImageCache while the PNG is decoded. No mask pixels are
				// scanned on the render thread anymore.
				++perf.mapImpactBoundsCacheHits;
				out.minX = master->influenceMinX;
				out.minY = master->influenceMinY;
				out.maxX = master->influenceMaxX;
				out.maxY = master->influenceMaxY;
				out.spanX = std::max(1u, out.maxX - out.minX + 1u);
				out.spanY = std::max(1u, out.maxY - out.minY + 1u);
				out.ready = true;
				return out;
			};

			BlastMaskSet blastSets[7];
			WeaponMaskSet weaponTriplets[3];
			ImpactMaskBounds legacyWeaponBounds[7];
			int firstBlastVariant = -1, firstWeaponTripletVariant = -1, firstLegacyWeaponVariant = -1;
			int loadedBlastVariants = 0, loadedWeaponTripletVariants = 0, loadedLegacyWeaponVariants = 0;
			int availableBlastVariants[7] = {0}, availableWeaponTripletVariants[3] = {0}, availableLegacyWeaponVariants[7] = {0};
			for (int variant = 0; variant < 7; ++variant)
			{
				blastSets[variant].core = maskReady(blastCoreVariants[variant]) ? blastCoreVariants[variant] : nullptr;
				blastSets[variant].rim = maskReady(blastRimVariants[variant]) ? blastRimVariants[variant] : nullptr;
				blastSets[variant].halo = maskReady(blastHaloVariants[variant]) ? blastHaloVariants[variant] : nullptr;
				HdImage *blastMaster = blastSets[variant].halo ? blastSets[variant].halo : (blastSets[variant].rim ? blastSets[variant].rim : blastSets[variant].core);
				if (blastSets[variant].core && blastMaster)
				{
					blastSets[variant].bounds = buildBounds(blastMaster);
					if (blastSets[variant].bounds.ready)
					{
						if (firstBlastVariant < 0) firstBlastVariant = variant;
						availableBlastVariants[loadedBlastVariants++] = variant;
					}
				}
				if (maskReady(weaponImpactVariants[variant]))
				{
					legacyWeaponBounds[variant] = buildBounds(weaponImpactVariants[variant]);
					if (legacyWeaponBounds[variant].ready)
					{
						if (firstLegacyWeaponVariant < 0) firstLegacyWeaponVariant = variant;
						availableLegacyWeaponVariants[loadedLegacyWeaponVariants++] = variant;
					}
				}
			}
			for (int variant = 0; variant < 3; ++variant)
			{
				weaponTriplets[variant].core = maskReady(weaponImpactCoreVariants[variant]) ? weaponImpactCoreVariants[variant] : nullptr;
				weaponTriplets[variant].rim = maskReady(weaponImpactRimVariants[variant]) ? weaponImpactRimVariants[variant] : nullptr;
				weaponTriplets[variant].halo = maskReady(weaponImpactHaloVariants[variant]) ? weaponImpactHaloVariants[variant] : nullptr;
				weaponTriplets[variant].legacy = maskReady(weaponImpactVariants[variant]) ? weaponImpactVariants[variant] : nullptr;
				HdImage *weaponMaster = weaponTriplets[variant].halo ? weaponTriplets[variant].halo : (weaponTriplets[variant].rim ? weaponTriplets[variant].rim : weaponTriplets[variant].core);
				if (weaponTriplets[variant].core && weaponMaster)
				{
					weaponTriplets[variant].bounds = buildBounds(weaponMaster);
					if (weaponTriplets[variant].bounds.ready)
					{
						weaponTriplets[variant].authoredTriplet = true;
						if (firstWeaponTripletVariant < 0) firstWeaponTripletVariant = variant;
						availableWeaponTripletVariants[loadedWeaponTripletVariants++] = variant;
					}
				}
			}
			perf.mapImpactMaskPrepUs += hdPerfNowUs() - perfImpactMaskPrepStart;
			perf.mapImpactMaskScans += impactMaskScans;
			perf.mapImpactMaskScanPixels += impactMaskScanPixels;

			const auto &impactStamps = _save->getBedrockCraterStamps();
			const auto &impactDiametersMilliTiles = _save->getBedrockCraterDiametersMilliTiles();
			const unsigned long long impactSourceRevision = _save->getBedrockCraterRevision();
			int craterSamplesPerTile = 32;
			while (craterSamplesPerTile > 4 &&
				((long long)mapW * craterSamplesPerTile > 16384LL ||
				 (long long)mapH * mapZCount * craterSamplesPerTile > 16384LL))
				craterSamplesPerTile /= 2;
			const unsigned craterFieldW = (unsigned)std::max(1, mapW * craterSamplesPerTile);
			const unsigned craterFieldH = (unsigned)std::max(1, mapH * mapZCount * craterSamplesPerTile);

			const bool craterFieldShapeChanged = _hdBedrockCraterFieldW != craterFieldW || _hdBedrockCraterFieldH != craterFieldH
				|| _hdBedrockWeaponFieldW != craterFieldW || _hdBedrockWeaponFieldH != craterFieldH;
			if (_hdBedrockCraterFieldSourceRevision != impactSourceRevision || craterFieldShapeChanged)
			{
				const uint64_t perfImpactFieldRebuildStart = hdPerfNowUs();
				_hdBedrockCraterFieldSourceRevision = impactSourceRevision;
				_hdBedrockCraterFieldW = craterFieldW;
				_hdBedrockCraterFieldH = craterFieldH;
				_hdBedrockWeaponFieldW = craterFieldW;
				_hdBedrockWeaponFieldH = craterFieldH;
				_hdBedrockCraterField.clear();
				_hdBedrockWeaponField.clear();
				unsigned renderedBlastStamps = 0, renderedWeaponStamps = 0, renderedTripletWeaponStamps = 0, renderedLegacyWeaponStamps = 0;

				if (!impactStamps.empty() && (firstBlastVariant >= 0 || firstWeaponTripletVariant >= 0 || firstLegacyWeaponVariant >= 0))
				{
					// Two RGBA8 world-space fields: blast RGB, and weapon-impact CORE/RIM/HALO with A fallback coverage.
					_hdBedrockCraterField.assign((size_t)craterFieldW * craterFieldH * 4u, 0);
					_hdBedrockWeaponField.assign((size_t)craterFieldW * craterFieldH * 4u, 0);

					auto transformUv = [](float &u, float &v, int transform)
					{
						if (transform & 4) u = 1.0f - u;
						if (transform & 8) v = 1.0f - v;
						const float oldU = u, oldV = v;
						switch (transform & 3)
						{
							case 1: u = oldV; v = 1.0f - oldU; break;
							case 2: u = 1.0f - oldU; v = 1.0f - oldV; break;
							case 3: u = 1.0f - oldV; v = oldU; break;
							default: break;
						}
					};
					auto impactStyleHash = [](const Position &p, size_t ordinal, int diameterMilliTiles, int kind) -> unsigned
					{
						unsigned h = 2166136261u;
						auto mix = [&](unsigned v) { h ^= v; h *= 16777619u; };
						mix((unsigned)p.x); mix((unsigned)p.y); mix((unsigned)p.z);
						mix((unsigned)ordinal); mix((unsigned)diameterMilliTiles); mix((unsigned)kind);
						h ^= h >> 16; h *= 0x7feb352du; h ^= h >> 15; h *= 0x846ca68bu; h ^= h >> 16;
						return h;
					};

					for (size_t stampIndex = 0; stampIndex < impactStamps.size(); ++stampIndex)
					{
						const Position &voxelCenter = impactStamps[stampIndex];
						const int z = voxelCenter.z / Position::TileZ;
						if (z < 0 || z >= mapZCount) continue;
						const int storedDiameterMilliTiles = stampIndex < impactDiametersMilliTiles.size() ? impactDiametersMilliTiles[stampIndex] : 1000;
						const int kind = storedDiameterMilliTiles >= 1000000 ? 1 : 0;
						const int diameterMilliTiles = kind == 1 ? std::max(1, storedDiameterMilliTiles - 1000000) : storedDiameterMilliTiles;
						const unsigned styleHash = impactStyleHash(voxelCenter, stampIndex, storedDiameterMilliTiles, kind);
						int variant = 0;
						if (kind == 1)
						{
							if (loadedWeaponTripletVariants > 0) variant = availableWeaponTripletVariants[styleHash % (unsigned)loadedWeaponTripletVariants];
							else if (loadedLegacyWeaponVariants > 0) variant = availableLegacyWeaponVariants[styleHash % (unsigned)loadedLegacyWeaponVariants];
						}
						else if (loadedBlastVariants > 0) variant = availableBlastVariants[styleHash % (unsigned)loadedBlastVariants];
						const int transform = (int)((styleHash >> 8) & 15u);

						ImpactMaskBounds bounds;
						WeaponMaskSet *weaponSet = nullptr;
						HdImage *weaponLegacyMask = nullptr;
						BlastMaskSet *blastSet = nullptr;
						const bool useWeaponTriplet = kind == 1 && loadedWeaponTripletVariants > 0;
						if (kind == 1)
						{
							if (useWeaponTriplet)
							{
								if (variant < 0 || variant >= 3 || !weaponTriplets[variant].bounds.ready) variant = firstWeaponTripletVariant;
								if (variant < 0 || variant >= 3 || !weaponTriplets[variant].bounds.ready) continue;
								weaponSet = &weaponTriplets[variant]; bounds = weaponSet->bounds; ++renderedTripletWeaponStamps;
							}
							else
							{
								if (variant < 0 || variant >= 7 || !legacyWeaponBounds[variant].ready) variant = firstLegacyWeaponVariant;
								if (variant < 0 || variant >= 7 || !legacyWeaponBounds[variant].ready) continue;
								weaponLegacyMask = weaponImpactVariants[variant]; bounds = legacyWeaponBounds[variant]; ++renderedLegacyWeaponStamps;
							}
							++renderedWeaponStamps;
						}
						else
						{
							if (!blastSets[variant].bounds.ready) variant = firstBlastVariant;
							if (variant < 0 || !blastSets[variant].bounds.ready) continue;
							blastSet = &blastSets[variant]; bounds = blastSet->bounds; ++renderedBlastStamps;
						}

						const float diameterTiles = std::max(0.10f, diameterMilliTiles / 1000.0f);
						const int stampSize = std::max(1, (int)std::lround(diameterTiles * craterSamplesPerTile));
						const float worldX = (float)voxelCenter.x / (float)Position::TileXY;
						const float worldY = (float)voxelCenter.y / (float)Position::TileXY;
						const int centerPx = (int)std::lround(worldX * craterSamplesPerTile);
						const int centerPy = (int)std::lround(((float)z * mapH + worldY) * craterSamplesPerTile);
						const int left = centerPx - stampSize / 2;
						const int top = centerPy - stampSize / 2;

						for (int dy = 0; dy < stampSize; ++dy)
						{
							const int fy = top + dy;
							if (fy < z * mapH * craterSamplesPerTile || fy >= (z + 1) * mapH * craterSamplesPerTile || fy < 0 || fy >= (int)craterFieldH) continue;
							for (int dx = 0; dx < stampSize; ++dx)
							{
								const int fx = left + dx;
								if (fx < 0 || fx >= (int)craterFieldW) continue;
								float localU = ((float)dx + 0.5f) / (float)stampSize;
								float localV = ((float)dy + 0.5f) / (float)stampSize;
								transformUv(localU, localV, transform);
								const unsigned sxMaster = bounds.minX + std::min(bounds.spanX - 1u, (unsigned)std::max(0, (int)std::floor(localU * bounds.spanX)));
								const unsigned syMaster = bounds.minY + std::min(bounds.spanY - 1u, (unsigned)std::max(0, (int)std::floor(localV * bounds.spanY)));
								const float uAuthor = ((float)sxMaster + 0.5f) / (float)bounds.master->width;
								const float vAuthor = ((float)syMaster + 0.5f) / (float)bounds.master->height;
								const size_t dstIndex = ((size_t)fy * craterFieldW + (size_t)fx) * 4u;
								if (kind == 1)
								{
									if (weaponSet)
									{
										const unsigned core = influenceAtUv(weaponSet->core, uAuthor, vAuthor);
										const unsigned rim = influenceAtUv(weaponSet->rim, uAuthor, vAuthor);
										const unsigned halo = influenceAtUv(weaponSet->halo, uAuthor, vAuthor);
										// A is reserved for legacy-only masks. Reusing triplet coverage
										// as a depression cancels the raised rim and dents the halo.
										const unsigned legacy = 0;
										_hdBedrockWeaponField[dstIndex + 0] = (unsigned char)std::max<unsigned>(_hdBedrockWeaponField[dstIndex + 0], core);
										_hdBedrockWeaponField[dstIndex + 1] = (unsigned char)std::max<unsigned>(_hdBedrockWeaponField[dstIndex + 1], rim);
										_hdBedrockWeaponField[dstIndex + 2] = (unsigned char)std::max<unsigned>(_hdBedrockWeaponField[dstIndex + 2], halo);
										_hdBedrockWeaponField[dstIndex + 3] = (unsigned char)std::max<unsigned>(_hdBedrockWeaponField[dstIndex + 3], legacy);
									}
									else
									{
										const unsigned impact = influenceAtUv(weaponLegacyMask, uAuthor, vAuthor);
										_hdBedrockWeaponField[dstIndex + 3] = (unsigned char)std::max<unsigned>(_hdBedrockWeaponField[dstIndex + 3], impact);
									}
								}
								else
								{
									const unsigned core = influenceAtUv(blastSet->core, uAuthor, vAuthor);
									const unsigned rim = influenceAtUv(blastSet->rim, uAuthor, vAuthor);
									const unsigned halo = influenceAtUv(blastSet->halo, uAuthor, vAuthor);
									_hdBedrockCraterField[dstIndex + 0] = (unsigned char)std::max<unsigned>(_hdBedrockCraterField[dstIndex + 0], core);
									_hdBedrockCraterField[dstIndex + 1] = (unsigned char)std::max<unsigned>(_hdBedrockCraterField[dstIndex + 1], rim);
									_hdBedrockCraterField[dstIndex + 2] = (unsigned char)std::max<unsigned>(_hdBedrockCraterField[dstIndex + 2], halo);
								}
							}
						}
					}
				}
				++_hdBedrockCraterFieldUploadRevision;
				++_hdBedrockWeaponFieldUploadRevision;
				Log(LOG_INFO) << "[BEDROCK WEAPON IMPACT TRIPLET V7W][REBUILD] stamps=" << impactStamps.size()
					<< " blastStamps=" << renderedBlastStamps << " weaponStamps=" << renderedWeaponStamps
					<< " weaponTripletStamps=" << renderedTripletWeaponStamps << " weaponLegacyStamps=" << renderedLegacyWeaponStamps
					<< " blastVariants=" << loadedBlastVariants << " weaponTriplets=" << loadedWeaponTripletVariants << " legacyWeaponVariants=" << loadedLegacyWeaponVariants
					<< " field=" << craterFieldW << "x" << craterFieldH << "xRGBA"
					<< " channels=blastRGB + weapon(core/rim/halo + legacyA) samplesPerTile=" << craterSamplesPerTile
					<< " merge=MAX-per-channel hdPipeline=1";
				perf.mapImpactFieldRebuildUs += hdPerfNowUs() - perfImpactFieldRebuildStart;
			}

			const uint64_t perfBedrockGeometryStart = hdPerfNowUs();
			std::vector<HdGpuBedrockVertex> &vertices = _hdBedrockGeometry;
			const bool rebuildBedrockGeometry = _hdBedrockGeometryRevision != _hdPhysicalRevision
				|| _hdBedrockGeometryViewLevel != viewLevel
				|| _hdBedrockGeometryMapW != mapW || _hdBedrockGeometryMapH != mapH;
			// 6 top vertices plus at most two visible exposed faces (12 vertices) per cell.
			if (rebuildBedrockGeometry)
			{
				vertices.clear();
				_hdSurfaceCursorQuads.clear();
				_hdFogSurfaceQuads.clear();
				vertices.reserve(cellCount * (size_t)(viewLevel + 1) * 18u);
				_hdSurfaceCursorQuads.reserve(cellCount * (size_t)(viewLevel + 1));
				_hdFogSurfaceQuads.reserve(cellCount * (size_t)(viewLevel + 1));
			}
			std::vector<unsigned> layerSurfaceTiles((size_t)viewLevel + 1u, 0u);
			std::vector<unsigned> layerUndiscoveredTiles((size_t)viewLevel + 1u, 0u);
			unsigned surfaceTiles = 0;
			unsigned undiscoveredBlackTiles = 0;
			unsigned explicitSemanticTiles = 0;
			unsigned fallbackScalarTiles = 0;
			unsigned semanticDebrisTiles = 0;
			unsigned continuousEdges = 0;
			unsigned exposedEdges = 0;
			unsigned crossingEdgeConflicts = 0;
			unsigned verticalVertices = 0;
			int maxExposedDelta = 0;
			unsigned sharedCornerConflicts = 0;
			unsigned hardBreakCorners = 0;
			int maxSharedCornerConflict = 0;
			std::map<int, unsigned> semanticDebrisMcdIds;

			const Position cameraPos = _camera->getMapOffset();
			const float uvPeriod = 8.0f; // world cells per top-material repeat.
			const float verticalUvPixels = 16.0f; // one vertical repeat per Legacy cell-height unit.
			int currentFogLayer = 0;
			auto emit = [&](float x, float y, float u, float v, float craterU, float craterV, float light,
				float tintR, float tintG, float tintB, float role, float worldX, float worldY, float worldZ)
			{
				HdGpuBedrockVertex vv;
				vv.logicalX = x; vv.logicalY = y; vv.worldU = u; vv.worldV = v;
				vv.craterU = craterU; vv.craterV = craterV; vv.light = light;
				vv.lightTintR = tintR; vv.lightTintG = tintG; vv.lightTintB = tintB; vv.materialRole = role;
				// Camera::convertVoxelToScreen projects (x-y, (x+y)/2-z).  Therefore
				// x+y+z is the missing camera-ray coordinate: a real geometric depth,
				// independent of Legacy PCK/raster painter order.
				vv.viewDepth = worldX + worldY + worldZ;
				vv.fogX = worldX / (float)Position::TileXY;
				vv.fogY = worldY / (float)Position::TileXY;
				vv.fogLayer = (float)currentFogLayer;
				vertices.push_back(vv);
			};
			auto cellIndex = [mapW](int x, int y) -> size_t { return (size_t)y * (size_t)mapW + (size_t)x; };
			auto cornerIndex = [mapW](int gx, int gy) -> size_t { return (size_t)gy * (size_t)(mapW + 1) + (size_t)gx; };
			auto sameSignOrZero = [](int a, int b) -> bool
			{
				return a == 0 || b == 0 || (a < 0) == (b < 0);
			};

			// HD SURFACE SUNLIGHT V2 — presentation-only geometric self-lighting.
			// OXCE keeps every logical light/shadow/LOS rule, but BEDROCK no longer uses
			// per-tile Legacy shade values to decide which dune face is visually dark.
			// Instead the authored BEDROCK geometry is lit by a continuous sun vector.
			// New Battle is a calibration bench: force 09:00 so at least two dune
			// orientations are readable. Campaign missions still use the game clock;
			// globe/longitude correction remains a later refinement.
			const SavedGame *hdSavedGame = (_game ? _game->getSavedGame() : nullptr);
			const GameTime *missionTime = hdSavedGame ? hdSavedGame->getTime() : nullptr;
			const bool hdNewBattleCalibration = hdSavedGame && hdSavedGame->getMonthsPassed() == -1;
			const float clockHour = hdNewBattleCalibration ? 9.0f
				: (missionTime ? ((float)missionTime->getHour() + (float)missionTime->getMinute() / 60.0f) : 12.0f);
			const float dayPhase = std::max(-1.0f, std::min(1.0f, (clockHour - 12.0f) / 6.0f));
			float sunX = -0.70f * std::sin(dayPhase * 1.57079632679f); // morning E(+x), afternoon W(-x)
			float sunY = std::sqrt(std::max(0.0f, 1.0f - sunX * sunX)); // logical south (+y)
			float sunZ = 0.70f + 0.20f * (1.0f - std::abs(dayPhase));
			const float sunInvLen = 1.0f / std::sqrt(std::max(0.0001f, sunX * sunX + sunY * sunY + sunZ * sunZ));
			sunX *= sunInvLen; sunY *= sunInvLen; sunZ *= sunInvLen;
			const float flatSunDot = sunZ;
			const float sunStrength = std::max(0.0f, std::min(1.0f, (8.0f - (float)_save->getGlobalShade()) / 8.0f));
			const float sunDirectScale = std::max(0.0f, Options::hdSunlightIntensityPermille / 1000.0f);
			const float shadowScale = std::max(0.0f, Options::hdShadowStrengthPermille / 1000.0f);
			// BEDROCK SURFACE CONTINUITY REPAIR V1. Sun response is now evaluated from
			// the shared-corner height field instead of independently per Legacy triangle.
			// This keeps OXCE/MCD as the semantic authority while removing the visible
			// diagonal lighting crease that made each logical diamond look faceted.
			auto surfaceSunFactorFromGradient = [&](float dHdx, float dHdy) -> float
			{
				if (!Options::hdSurfaceSunlightEnabled || sunStrength <= 0.0f) return 1.0f;
				float nx = -dHdx, ny = -dHdy, nz = 8.0f;
				const float invLen = 1.0f / std::sqrt(std::max(0.0001f, nx * nx + ny * ny + nz * nz));
				nx *= invLen; ny *= invLen; nz *= invLen;
				const float ndl = std::max(0.0f, nx * sunX + ny * sunY + nz * sunZ);
				const float delta = ndl - flatSunDot;
				const float response = delta >= 0.0f ? sunDirectScale : shadowScale;
				const float factor = 1.0f + delta * 0.95f * sunStrength * response;
				return std::max(0.35f, std::min(1.65f, factor));
			};

			if (rebuildBedrockGeometry)
			for (int mapZ = 0; mapZ <= viewLevel; ++mapZ)
			{
				currentFogLayer = mapZ;
				std::vector<unsigned char> covered(cellCount, 0);
				std::vector<unsigned char> gradeCovered(cellCount, 0);
				std::vector<unsigned char> cellDiscovered(cellCount, 0);
				std::vector<float> cellLights(cellCount, 0.0f);
				std::vector<float> cellTintR(cellCount, 1.0f), cellTintG(cellCount, 1.0f), cellTintB(cellCount, 1.0f);
				std::vector<BedrockCellGeometry> geometries(cellCount);

				// Phase A: semantic surface extraction. Nothing is averaged here. A cell's
				// profile is its own exterior surface; adjacency is classified afterwards.
				for (int y = 0; y < mapH; ++y)
				{
					for (int x = 0; x < mapW; ++x)
					{
						Tile *t = _save->getTile(Position(x, y, mapZ));
						if (!t || !BedrockRenderPolicy::hasSurface(bedrockMaterial, t)) continue;

						const size_t idx = cellIndex(x, y);
						covered[idx] = 1;
						// MATERIAL GRADE V1: coverage only selects a milder colour response.
						// It intentionally does NOT darken the surface; roof/interior illumination
						// belongs to the later REAL-HD lighting layer.
						const Tile *aboveForGrade = _save->getAboveTile(t);
						// Ignore virtual walk support supplied by this dune to the empty cell above.
						gradeCovered[idx] = (aboveForGrade && !aboveForGrade->hasNoFloor(nullptr)) ? 1 : 0;
						const bool discovered = RealHdWorldPresentation::allows(
							t, O_FLOOR, RealHdWorldPresentationClass::PersistentGeometry);
						// REAL HD LIGHTING V2: use the same presentation light authority as
						// authored/transition sprites. LL_UNITS is excluded inside
						// realHdLightingAt(); helmet light is directional and LOS-blocked.
						const float light = realHdLightingAt(t);
						cellDiscovered[idx] = discovered ? 1 : 0;
						cellLights[idx] = light;
						realHdLightingTintAt(t, cellTintR[idx], cellTintG[idx], cellTintB[idx]);
						if (!discovered)
						{
							++layerUndiscoveredTiles[(size_t)mapZ];
							++undiscoveredBlackTiles;
						}

						const BedrockCellGeometry geometry = BedrockRenderPolicy::cellGeometry(bedrockMaterial, t);
						geometries[idx] = geometry;
						if (geometry.explicitGeometry)
						{
							++explicitSemanticTiles;
							if (geometry.semanticDebris)
							{
								++semanticDebrisTiles;
								++semanticDebrisMcdIds[geometry.sourceMcdId];
							}
						}
						else ++fallbackScalarTiles;

						++layerSurfaceTiles[(size_t)mapZ];
						++surfaceTiles;
					}
				}

				if (layerSurfaceTiles[(size_t)mapZ] == 0) continue;

				// BEDROCK SURFACE CONTINUITY REPAIR V1. Restore the pre-EXPOSED-FACES
				// shared-corner contract for the top surface, but preserve later hard
				// exposed-edge semantics. All physical neighbours contribute. The
				// material's per-pixel fog sampler prevents unknown geometry leaking.
				const size_t cornerCount = (size_t)(mapW + 1) * (size_t)(mapH + 1);
				std::vector<float> cornerHeightSum(cornerCount, 0.0f);
				std::vector<unsigned short> cornerHeightCount(cornerCount, 0);
				std::vector<int> cornerMin(cornerCount, 0);
				std::vector<int> cornerMax(cornerCount, 0);
				std::vector<unsigned char> cornerHardBreak(cornerCount, 0);

				auto proposeCornerHeight = [&](int gx, int gy, int legacyHeight)
				{
					const size_t ci = cornerIndex(gx, gy);
					cornerHeightSum[ci] += (float)legacyHeight;
					if (cornerHeightCount[ci] == 0) cornerMin[ci] = cornerMax[ci] = legacyHeight;
					else
					{
						cornerMin[ci] = std::min(cornerMin[ci], legacyHeight);
						cornerMax[ci] = std::max(cornerMax[ci], legacyHeight);
					}
					++cornerHeightCount[ci];
				};

				for (int y = 0; y < mapH; ++y)
				{
					for (int x = 0; x < mapW; ++x)
					{
						const size_t idx = cellIndex(x, y);
						if (!covered[idx]) continue;
						const BedrockCellGeometry &g = geometries[idx];
						proposeCornerHeight(x, y, g.top);
						proposeCornerHeight(x + 1, y, g.right);
						proposeCornerHeight(x + 1, y + 1, g.bottom);
						proposeCornerHeight(x, y + 1, g.left);
					}
				}

				// Preserve true exposed faces introduced after the original smoother. A
				// corner touched by a divergent shared edge keeps each cell's authored
				// fallback height instead of being averaged through the discontinuity.
				for (int y = 0; y < mapH; ++y)
				{
					for (int x = 0; x < mapW; ++x)
					{
						const size_t idx = cellIndex(x, y);
						if (!covered[idx]) continue;
						const BedrockCellGeometry &g = geometries[idx];
						if (x + 1 < mapW)
						{
							const size_t nidx = cellIndex(x + 1, y);
							if (covered[nidx])
							{
								const BedrockCellGeometry &n = geometries[nidx];
								if (g.right != n.top || g.bottom != n.left)
								{
									cornerHardBreak[cornerIndex(x + 1, y)] = 1;
									cornerHardBreak[cornerIndex(x + 1, y + 1)] = 1;
								}
							}
						}
						if (y + 1 < mapH)
						{
							const size_t nidx = cellIndex(x, y + 1);
							if (covered[nidx])
							{
								const BedrockCellGeometry &n = geometries[nidx];
								if (g.left != n.top || g.bottom != n.right)
								{
									cornerHardBreak[cornerIndex(x, y + 1)] = 1;
									cornerHardBreak[cornerIndex(x + 1, y + 1)] = 1;
								}
							}
						}
					}
				}

				for (size_t ci = 0; ci < cornerCount; ++ci)
				{
					if (cornerHardBreak[ci]) ++hardBreakCorners;
					if (cornerHeightCount[ci] > 1 && cornerMax[ci] != cornerMin[ci])
					{
						++sharedCornerConflicts;
						maxSharedCornerConflict = std::max(maxSharedCornerConflict, cornerMax[ci] - cornerMin[ci]);
					}
				}

				auto averageCornerHeight = [&](int gx, int gy, float fallback) -> float
				{
					if (gx < 0 || gy < 0 || gx > mapW || gy > mapH) return fallback;
					const size_t ci = cornerIndex(gx, gy);
					return cornerHeightCount[ci] ? cornerHeightSum[ci] / (float)cornerHeightCount[ci] : fallback;
				};
				auto sharedHeight = [&](int gx, int gy, float fallback) -> float
				{
					if (gx < 0 || gy < 0 || gx > mapW || gy > mapH) return fallback;
					const size_t ci = cornerIndex(gx, gy);
					if (cornerHardBreak[ci]) return fallback;
					return cornerHeightCount[ci] ? cornerHeightSum[ci] / (float)cornerHeightCount[ci] : fallback;
				};
				auto cornerSunFactor = [&](int gx, int gy) -> float
				{
					if (!Options::hdSurfaceSunlightEnabled || gx < 0 || gy < 0 || gx > mapW || gy > mapH) return 1.0f;
					const size_t ci = cornerIndex(gx, gy);
					if (!cornerHeightCount[ci] || cornerHardBreak[ci]) return 1.0f;
					const float centerUp = -averageCornerHeight(gx, gy, 0.0f);
					const float leftUp = gx > 0 ? -averageCornerHeight(gx - 1, gy, -centerUp) : centerUp;
					const float rightUp = gx < mapW ? -averageCornerHeight(gx + 1, gy, -centerUp) : centerUp;
					const float topUp = gy > 0 ? -averageCornerHeight(gx, gy - 1, -centerUp) : centerUp;
					const float bottomUp = gy < mapH ? -averageCornerHeight(gx, gy + 1, -centerUp) : centerUp;
					const float dHdx = (rightUp - leftUp) * (gx > 0 && gx < mapW ? 0.5f : 1.0f);
					const float dHdy = (bottomUp - topUp) * (gy > 0 && gy < mapH ? 0.5f : 1.0f);
					return surfaceSunFactorFromGradient(dHdx, dHdy);
				};

				// HD CONTINUOUS LIGHT FIELD V1. OXCE remains authoritative for logical
				// shade/discovery. Presentation interpolates light at shared corners;
				// the independent per-pixel fog sampler controls disclosure.
				auto cornerSmoothLight = [&](int cx, int cy) -> float
				{
					float lightSum = 0.0f;
					int samples = 0;
					for (int oy = -1; oy <= 0; ++oy)
					{
						for (int ox = -1; ox <= 0; ++ox)
						{
							const int sx = cx + ox, sy = cy + oy;
							if (sx < 0 || sy < 0 || sx >= mapW || sy >= mapH) continue;
							const size_t sidx = cellIndex(sx, sy);
							if (!covered[sidx]) continue;
							lightSum += cellLights[sidx];
							++samples;
						}
					}
					return samples ? (lightSum / (float)samples) : 0.0f;
				};
				auto cornerSmoothTint = [&](int cx, int cy, const std::vector<float> &values) -> float
				{
					float sum = 0.0f;
					int samples = 0;
					for (int oy = -1; oy <= 0; ++oy)
					{
						for (int ox = -1; ox <= 0; ++ox)
						{
							const int sx = cx + ox, sy = cy + oy;
							if (sx < 0 || sy < 0 || sx >= mapW || sy >= mapH) continue;
							const size_t sidx = cellIndex(sx, sy);
							if (!covered[sidx]) continue;
							sum += values[sidx];
							++samples;
						}
					}
					return samples ? (sum / (float)samples) : 1.0f;
				};

				// HD CONTINUOUS VISIBILITY MASK V2 is applied once after all world sprites.


				// Emit one exposed edge as a closed vertical skin between two semantic
				// profiles. Only the two camera-visible grid directions (east/south) are
				// emitted. Hidden internal faces never exist in the render mesh.
				auto emitVerticalEdge = [&](float x0, float y0Base, int hA0, int hB0,
					float x1, float y1Base, int hA1, int hB1, float u0, float u1, float light0, float light1,
					float tint0R, float tint0G, float tint0B, float tint1R, float tint1G, float tint1B, float role,
					float worldX0, float worldY0, float worldX1, float worldY1, float worldBaseZ)
				{
					const int highH0 = std::min(hA0, hB0), lowH0 = std::max(hA0, hB0);
					const int highH1 = std::min(hA1, hB1), lowH1 = std::max(hA1, hB1);
					const float high0 = y0Base + (float)highH0 * mapScale, low0 = y0Base + (float)lowH0 * mapScale;
					const float high1 = y1Base + (float)highH1 * mapScale, low1 = y1Base + (float)lowH1 * mapScale;
					const float vHigh0 = -(float)highH0 / verticalUvPixels;
					const float vLow0 = -(float)lowH0 / verticalUvPixels;
					const float vHigh1 = -(float)highH1 / verticalUvPixels;
					const float vLow1 = -(float)lowH1 / verticalUvPixels;
					const float worldZHigh0 = worldBaseZ - (float)highH0, worldZLow0 = worldBaseZ - (float)lowH0;
					const float worldZHigh1 = worldBaseZ - (float)highH1, worldZLow1 = worldBaseZ - (float)lowH1;

					emit(x0, high0, u0, vHigh0, 0.0f, 0.0f, light0, tint0R, tint0G, tint0B, role, worldX0, worldY0, worldZHigh0);
					emit(x1, high1, u1, vHigh1, 0.0f, 0.0f, light1, tint1R, tint1G, tint1B, role, worldX1, worldY1, worldZHigh1);
					emit(x0, low0,  u0, vLow0,  0.0f, 0.0f, light0, tint0R, tint0G, tint0B, role, worldX0, worldY0, worldZLow0);
					emit(x1, high1, u1, vHigh1, 0.0f, 0.0f, light1, tint1R, tint1G, tint1B, role, worldX1, worldY1, worldZHigh1);
					emit(x1, low1,  u1, vLow1,  0.0f, 0.0f, light1, tint1R, tint1G, tint1B, role, worldX1, worldY1, worldZLow1);
					emit(x0, low0,  u0, vLow0,  0.0f, 0.0f, light0, tint0R, tint0G, tint0B, role, worldX0, worldY0, worldZLow0);
					verticalVertices += 6;
				};

				// Phase B: exterior surface generation. Top quads use their local semantic
				// profile directly. Shared edges are either continuous (no internal wall)
				// or exposed cuts (vertical substrate material).
				for (int y = 0; y < mapH; ++y)
				{
					for (int x = 0; x < mapW; ++x)
					{
						const size_t idx = cellIndex(x, y);
						if (!covered[idx]) continue;
						const BedrockCellGeometry &g = geometries[idx];
						Position sp;
						_camera->convertMapToScreen(Position(x, y, mapZ), &sp);
						sp += cameraPos;

						const float cornerLightTop = cornerSmoothLight(x, y) * cornerSunFactor(x, y);
						const float cornerLightRight = cornerSmoothLight(x + 1, y) * cornerSunFactor(x + 1, y);
						const float cornerLightBottom = cornerSmoothLight(x + 1, y + 1) * cornerSunFactor(x + 1, y + 1);
						const float cornerLightLeft = cornerSmoothLight(x, y + 1) * cornerSunFactor(x, y + 1);
						const float tintTopR = cornerSmoothTint(x, y, cellTintR), tintTopG = cornerSmoothTint(x, y, cellTintG), tintTopB = cornerSmoothTint(x, y, cellTintB);
						const float tintRightR = cornerSmoothTint(x + 1, y, cellTintR), tintRightG = cornerSmoothTint(x + 1, y, cellTintG), tintRightB = cornerSmoothTint(x + 1, y, cellTintB);
						const float tintBottomR = cornerSmoothTint(x + 1, y + 1, cellTintR), tintBottomG = cornerSmoothTint(x + 1, y + 1, cellTintG), tintBottomB = cornerSmoothTint(x + 1, y + 1, cellTintB);
						const float tintLeftR = cornerSmoothTint(x, y + 1, cellTintR), tintLeftG = cornerSmoothTint(x, y + 1, cellTintG), tintLeftB = cornerSmoothTint(x, y + 1, cellTintB);
						const float topH = sharedHeight(x, y, (float)g.top);
						const float rightH = sharedHeight(x + 1, y, (float)g.right);
						const float bottomH = sharedHeight(x + 1, y + 1, (float)g.bottom);
						const float leftH = sharedHeight(x, y + 1, (float)g.left);
						const float topX = (float)sp.x + 16.0f * mapScale;
						const float topY = (float)sp.y + 24.0f * mapScale + topH * mapScale;
						const float rightX = (float)sp.x + 32.0f * mapScale;
						const float rightY = (float)sp.y + 32.0f * mapScale + rightH * mapScale;
						const float bottomX = (float)sp.x + 16.0f * mapScale;
						const float bottomY = (float)sp.y + 40.0f * mapScale + bottomH * mapScale;
						const float leftX = (float)sp.x;
						const float leftY = (float)sp.y + 32.0f * mapScale + leftH * mapScale;
						const float worldBaseZ = (float)mapZ * Position::TileZ;
						const float worldX0 = (float)x * Position::TileXY, worldX1 = (float)(x + 1) * Position::TileXY;
						const float worldY0 = (float)y * Position::TileXY, worldY1 = (float)(y + 1) * Position::TileXY;
						HdSurfaceCursorQuad cursorQuad;
						cursorQuad.tile = Position(x, y, mapZ);
						cursorQuad.xy = {topX, topY, rightX, rightY,
							bottomX, bottomY, leftX, leftY};
						cursorQuad.depth = {worldX0 + worldY0 + worldBaseZ - topH,
							worldX1 + worldY0 + worldBaseZ - rightH,
							worldX1 + worldY1 + worldBaseZ - bottomH,
							worldX0 + worldY1 + worldBaseZ - leftH};
						_hdFogSurfaceQuads.push_back(cursorQuad);
						if (cellDiscovered[idx]) _hdSurfaceCursorQuads.push_back(cursorQuad);

						const float u0 = (float)x / uvPeriod, v0 = (float)y / uvPeriod;
						const float u1 = (float)(x + 1) / uvPeriod, v1 = (float)(y + 1) / uvPeriod;
						const float craterU0 = (float)x / (float)mapW;
						const float craterU1 = (float)(x + 1) / (float)mapW;
						const float craterV0 = ((float)mapZ * mapH + (float)y) / (float)(mapH * mapZCount);
						const float craterV1 = ((float)mapZ * mapH + (float)(y + 1)) / (float)(mapH * mapZCount);
						// materialRole 0/1/2 = exposed top/east/south.  +4 encodes only the
						// COVERED colour-grade context while preserving the geometric role.
						const float topGradeRole = gradeCovered[idx] ? 4.0f : 0.0f;
						// Shared-corner lighting is used identically by both triangles. There is no
						// longer a triangle-local brightness factor capable of drawing the Legacy
						// diagonal through an otherwise continuous BEDROCK surface.
						emit(topX, topY, u0, v0, craterU0, craterV0, cornerLightTop, tintTopR, tintTopG, tintTopB, topGradeRole, worldX0, worldY0, worldBaseZ - topH);
						emit(rightX, rightY, u1, v0, craterU1, craterV0, cornerLightRight, tintRightR, tintRightG, tintRightB, topGradeRole, worldX1, worldY0, worldBaseZ - rightH);
						emit(leftX, leftY, u0, v1, craterU0, craterV1, cornerLightLeft, tintLeftR, tintLeftG, tintLeftB, topGradeRole, worldX0, worldY1, worldBaseZ - leftH);
						emit(rightX, rightY, u1, v0, craterU1, craterV0, cornerLightRight, tintRightR, tintRightG, tintRightB, topGradeRole, worldX1, worldY0, worldBaseZ - rightH);
						emit(bottomX, bottomY, u1, v1, craterU1, craterV1, cornerLightBottom, tintBottomR, tintBottomG, tintBottomB, topGradeRole, worldX1, worldY1, worldBaseZ - bottomH);
						emit(leftX, leftY, u0, v1, craterU0, craterV1, cornerLightLeft, tintLeftR, tintLeftG, tintLeftB, topGradeRole, worldX0, worldY1, worldBaseZ - leftH);

						// East shared edge: current [right,bottom] vs east [top,left].
						if (x + 1 < mapW)
						{
							const size_t nidx = cellIndex(x + 1, y);
							if (covered[nidx])
							{
								const BedrockCellGeometry &n = geometries[nidx];
								const int d0 = g.right - n.top;
								const int d1 = g.bottom - n.left;
								if (d0 == 0 && d1 == 0) ++continuousEdges;
								else
								{
									++exposedEdges;
									maxExposedDelta = std::max(maxExposedDelta, std::max(std::abs(d0), std::abs(d1)));
									if (!sameSignOrZero(d0, d1)) ++crossingEdgeConflicts;
									const float faceLight0 = cornerSmoothLight(x + 1, y);
									const float faceLight1 = cornerSmoothLight(x + 1, y + 1);
									const float faceTint0R = cornerSmoothTint(x + 1, y, cellTintR), faceTint0G = cornerSmoothTint(x + 1, y, cellTintG), faceTint0B = cornerSmoothTint(x + 1, y, cellTintB);
									const float faceTint1R = cornerSmoothTint(x + 1, y + 1, cellTintR), faceTint1G = cornerSmoothTint(x + 1, y + 1, cellTintG), faceTint1B = cornerSmoothTint(x + 1, y + 1, cellTintB);
									const float faceGradeOffset = (gradeCovered[idx] && gradeCovered[nidx]) ? 4.0f : 0.0f;
									emitVerticalEdge(rightX, (float)sp.y + 32.0f * mapScale, g.right, n.top,
										bottomX, (float)sp.y + 40.0f * mapScale, g.bottom, n.left,
										(float)y / uvPeriod, (float)(y + 1) / uvPeriod, faceLight0, faceLight1,
										faceTint0R, faceTint0G, faceTint0B, faceTint1R, faceTint1G, faceTint1B, 1.0f + faceGradeOffset,
										worldX1, worldY0, worldX1, worldY1, worldBaseZ);
								}
							}
						}

						// South shared edge: current [left,bottom] vs south [top,right].
						if (y + 1 < mapH)
						{
							const size_t nidx = cellIndex(x, y + 1);
							if (covered[nidx])
							{
								const BedrockCellGeometry &n = geometries[nidx];
								const int d0 = g.left - n.top;
								const int d1 = g.bottom - n.right;
								if (d0 == 0 && d1 == 0) ++continuousEdges;
								else
								{
									++exposedEdges;
									maxExposedDelta = std::max(maxExposedDelta, std::max(std::abs(d0), std::abs(d1)));
									if (!sameSignOrZero(d0, d1)) ++crossingEdgeConflicts;
									const float faceLight0 = cornerSmoothLight(x, y + 1);
									const float faceLight1 = cornerSmoothLight(x + 1, y + 1);
									const float faceTint0R = cornerSmoothTint(x, y + 1, cellTintR), faceTint0G = cornerSmoothTint(x, y + 1, cellTintG), faceTint0B = cornerSmoothTint(x, y + 1, cellTintB);
									const float faceTint1R = cornerSmoothTint(x + 1, y + 1, cellTintR), faceTint1G = cornerSmoothTint(x + 1, y + 1, cellTintG), faceTint1B = cornerSmoothTint(x + 1, y + 1, cellTintB);
									const float faceGradeOffset = (gradeCovered[idx] && gradeCovered[nidx]) ? 4.0f : 0.0f;
									emitVerticalEdge(leftX, (float)sp.y + 32.0f * mapScale, g.left, n.top,
										bottomX, (float)sp.y + 40.0f * mapScale, g.bottom, n.right,
										(float)x / uvPeriod, (float)(x + 1) / uvPeriod, faceLight0, faceLight1,
										faceTint0R, faceTint0G, faceTint0B, faceTint1R, faceTint1G, faceTint1B, 2.0f + faceGradeOffset,
										worldX0, worldY1, worldX1, worldY1, worldBaseZ);
								}
							}
						}
					}
				}
			}

			if (rebuildBedrockGeometry)
			{
				// The CPU mesh is retained across presentation frames. Give each
				// content change a process-wide serial so a newly created Map cannot
				// accidentally reuse the previous Map's GPU vertex buffer.
				static unsigned long long nextGpuGeometryRevision = 0;
				_hdBedrockGeometryGpuRevision = ++nextGpuGeometryRevision;
				_hdBedrockGeometryRevision = _hdPhysicalRevision;
				_hdBedrockGeometryViewLevel = viewLevel;
				_hdBedrockGeometryMapW = mapW;
				_hdBedrockGeometryMapH = mapH;
			}
			// HD LOCAL LIGHTS V2: universal source list was built before BEDROCK and is reused below.
			perf.mapBedrockGeometryUs += hdPerfNowUs() - perfBedrockGeometryStart;

			// Small semantic field, uploaded each presentation frame. Terrain geometry
			// remains cached; only the current mission-knowledge samples change.
			const int fogLayers = _save->getMapSizeZ();
			std::vector<unsigned char> hdFogKnowledge((size_t)mapW * (size_t)mapH * (size_t)fogLayers, 0);
			for (int z = 0; z <= viewLevel && z < fogLayers; ++z)
				for (int y = 0; y < mapH; ++y)
					for (int x = 0; x < mapW; ++x)
					{
						Tile *tile = _save->getTile(Position(x, y, z));
						if (RealHdWorldPresentation::allows(tile, O_FLOOR,
							RealHdWorldPresentationClass::PersistentGeometry))
							hdFogKnowledge[((size_t)z * (size_t)mapH + (size_t)y) * (size_t)mapW + (size_t)x] = 255;
					}

			if (!vertices.empty())
			{
				HdGpuBedrock bedrock;
				bedrock.sandMicroreliefPermille = bedrockMaterial == BedrockMaterial::Sand ? 2600 : 0;
				bedrock.fogCoverage = hdFogKnowledge.data();
				bedrock.fogWidth = (unsigned)mapW;
				bedrock.fogHeight = (unsigned)(mapH * fogLayers);
				bedrock.fogLayerHeight = (unsigned)mapH;
				bedrock.fogSoftnessPermille = Options::hdFogEdgeSoftnessPermille;
				bedrock.assetKey = bedrockAssetPath.c_str();
				bedrock.rgba = bedrockImage->rgba.data();
				bedrock.imageWidth = bedrockImage->width;
				bedrock.imageHeight = bedrockImage->height;
				if (bedrockPbrReady)
				{
					bedrock.normalKey = bedrockNormalPath.c_str();
					bedrock.normalRgba = bedrockNormal->rgba.data();
					bedrock.normalWidth = bedrockNormal->width;
					bedrock.normalHeight = bedrockNormal->height;
					bedrock.roughnessKey = bedrockRoughnessPath.c_str();
					bedrock.roughnessRgba = bedrockRoughness->rgba.data();
					bedrock.roughnessWidth = bedrockRoughness->width;
					bedrock.roughnessHeight = bedrockRoughness->height;
					bedrock.aoKey = bedrockAoPath.c_str();
					bedrock.aoRgba = bedrockAo->rgba.data();
					bedrock.aoWidth = bedrockAo->width;
					bedrock.aoHeight = bedrockAo->height;
					bedrock.usePbrMaterial = true;
				}

				bedrock.verticalAssetKey = bedrockVerticalAssetPath.c_str();
				bedrock.verticalRgba = bedrockVertical->rgba.data();
				bedrock.verticalImageWidth = bedrockVertical->width;
				bedrock.verticalImageHeight = bedrockVertical->height;
				if (bedrockVerticalPbrReady)
				{
					bedrock.verticalNormalKey = bedrockVerticalNormalPath.c_str();
					bedrock.verticalNormalRgba = bedrockVerticalNormal->rgba.data();
					bedrock.verticalNormalWidth = bedrockVerticalNormal->width;
					bedrock.verticalNormalHeight = bedrockVerticalNormal->height;
					bedrock.verticalRoughnessKey = bedrockVerticalRoughnessPath.c_str();
					bedrock.verticalRoughnessRgba = bedrockVerticalRoughness->rgba.data();
					bedrock.verticalRoughnessWidth = bedrockVerticalRoughness->width;
					bedrock.verticalRoughnessHeight = bedrockVerticalRoughness->height;
					bedrock.verticalAoKey = bedrockVerticalAoPath.c_str();
					bedrock.verticalAoRgba = bedrockVerticalAo->rgba.data();
					bedrock.verticalAoWidth = bedrockVerticalAo->width;
					bedrock.verticalAoHeight = bedrockVerticalAo->height;
					bedrock.useVerticalPbrMaterial = true;
				}

				if (!_hdBedrockCraterField.empty())
				{
					bedrock.craterField = _hdBedrockCraterField.data();
					bedrock.craterFieldWidth = _hdBedrockCraterFieldW;
					bedrock.craterFieldHeight = _hdBedrockCraterFieldH;
					bedrock.craterFieldRevision = _hdBedrockCraterFieldUploadRevision;
				}
				if (!_hdBedrockWeaponField.empty())
				{
					bedrock.weaponField = _hdBedrockWeaponField.data();
					bedrock.weaponFieldWidth = _hdBedrockWeaponFieldW;
					bedrock.weaponFieldHeight = _hdBedrockWeaponFieldH;
					bedrock.weaponFieldRevision = _hdBedrockWeaponFieldUploadRevision;
				}

				bedrock.vertices = vertices.data();
				bedrock.vertexCount = (unsigned)vertices.size();
				bedrock.vertexRevision = _hdBedrockGeometryGpuRevision;
				bedrock.destX = px0; bedrock.destY = py0; bedrock.destW = px1 - px0; bedrock.destH = py1 - py0;
				bedrock.mapPhysicalOriginX = (double)screen->logicalToPhysicalX(logicalMapX);
				bedrock.mapPhysicalOriginY = (double)screen->logicalToPhysicalY(logicalMapY);
				bedrock.renderScaleX = sx; bedrock.renderScaleY = sy;
				bedrock.worldDepthMode = HdGpuWorldDepthMode::OpaqueWrite;
				bedrock.environmentProfile = hdBedrockEnvironmentProfileForMaterial(BedrockRenderPolicy::semanticKey(bedrockMaterial));
				bedrock.applyEnvironment = Options::hdEnvironmentGrade;
				bedrock.environmentGradeTopPermille = std::max(0, Options::hdBedrockLutTopPermille);
				bedrock.environmentGradeVerticalPermille = std::max(0, Options::hdBedrockLutVerticalPermille);
				bedrock.environmentGradeCoveredPermille = std::max(0, Options::hdBedrockLutCoveredPermille);
				bedrock.depthLuminancePermille = Options::hdDepthLuminance
					? hdDepthLuminancePermilleForDepth(_save->getDepth()) : 1000;
				// Caustics are driven by mission daylight/depth, not local lamps.
                const int waterDepth = _save->getDepth();
                HdCausticSettings::instance().context(waterDepth, _save->getGlobalShade());
                const float depthCaustic = waterDepth == 1 ? 1.0f : (waterDepth == 2 ? 0.65f : 0.0f);
                const float dayCaustic = std::max(0.0f, std::min(1.0f, (8.0f-_save->getGlobalShade())/8.0f));
                bedrock.causticStrength = depthCaustic * dayCaustic;
                static int lastCausticDepth=-1, lastCausticShade=-1, lastCausticZ=-1;
                if(lastCausticDepth!=waterDepth || lastCausticShade!=_save->getGlobalShade() || lastCausticZ!=viewLevel) {
                    Log(LOG_INFO) << "[REAL HD CAUSTIC INPUT] missionDepth=" << waterDepth << " cameraZ=" << viewLevel << " globalShade=" << _save->getGlobalShade() << " strength=" << bedrock.causticStrength;
                    lastCausticDepth=waterDepth; lastCausticShade=_save->getGlobalShade(); lastCausticZ=viewLevel;
                }

                // Independent wave rates: do not wrap to a common short period.
                bedrock.causticPhase = SDL_GetTicks()*0.00012;
                bedrockDrawn = gpu.drawBedrock(bedrock);

				static int loggedViewLevel = -1;
				if (bedrockDrawn && loggedViewLevel != viewLevel)
				{
					unsigned sandFloorParts = 0, semanticOwnedObjectParts = 0, localObjectParts = 0;
					for (int mapZ = 0; mapZ <= viewLevel; ++mapZ)
					{
						for (int cy = 0; cy < mapH; ++cy)
						{
							for (int cx = 0; cx < mapW; ++cx)
							{
								Tile *ct = _save->getTile(Position(cx, cy, mapZ));
								if (!ct) continue;
								if (BedrockRenderPolicy::ownsTilePart(bedrockMaterial, ct, O_FLOOR)) ++sandFloorParts;
								if (BedrockRenderPolicy::ownsTilePart(bedrockMaterial, ct, O_OBJECT)) ++semanticOwnedObjectParts;
								else if (ct->getMapData(O_OBJECT)) ++localObjectParts;

							}
						}
					}

					std::ostringstream perLayer;
					std::ostringstream hiddenPerLayer;
					for (int mapZ = 0; mapZ <= viewLevel; ++mapZ)
					{
						if (mapZ) { perLayer << ","; hiddenPerLayer << ","; }
						perLayer << mapZ << ":" << layerSurfaceTiles[(size_t)mapZ];
						hiddenPerLayer << mapZ << ":" << layerUndiscoveredTiles[(size_t)mapZ];
					}

					Log(LOG_INFO) << "[BEDROCK MATERIAL SETS + EXPOSED FACES V1] material=" << BedrockRenderPolicy::name(bedrockMaterial)
						<< " semantic=" << BedrockRenderPolicy::semanticKey(bedrockMaterial)
						<< " mission=" << _save->getMissionType()
						<< " map=" << mapW << "x" << mapH << "x" << _save->getMapSizeZ()
						<< " visibleLayers=0.." << viewLevel
						<< " layerSurfaceTiles=" << perLayer.str()
						<< " layerUndiscoveredBlack=" << hiddenPerLayer.str()
						<< " surfaceTiles=" << surfaceTiles
						<< " undiscoveredBlackTiles=" << undiscoveredBlackTiles
						<< " semanticExplicitTiles=" << explicitSemanticTiles
						<< " semanticDebrisTiles=" << semanticDebrisTiles
						<< " fallbackScalarTiles=" << fallbackScalarTiles
						<< " continuousEdges=" << continuousEdges
						<< " exposedEdges=" << exposedEdges
						<< " crossingEdgeConflicts=" << crossingEdgeConflicts
						<< " maxExposedDelta=" << maxExposedDelta
						<< " verticalVertices=" << verticalVertices
						<< " sandFloorParts=" << sandFloorParts
						<< " semanticOwnedObjectParts=" << semanticOwnedObjectParts
						<< " localObjectParts=" << localObjectParts
						<< " vertices=" << vertices.size()
						<< " geometrySource=owned-logical-MCD-only"
						<< " surfaceContinuity=shared-corner-v1"
						<< " sharedCornerConflicts=" << sharedCornerConflicts
						<< " hardBreakCorners=" << hardBreakCorners
						<< " maxSharedCornerConflict=" << maxSharedCornerConflict
						<< " surfaceSun=shared-corner-normal"
						<< " ownership=SAND-all+DEBRIS-MCD32-49"
						<< " topPbr=" << (bedrockPbrReady ? "base+normal+roughness+ao" : "base-only")
						<< " verticalPbr=" << (bedrockVerticalPbrReady ? "base+normal+roughness+ao" : "base-only")
						<< " materialGrade=semantic-v1-r3-legacy-push"
						<< " gradeTop=" << std::fixed << std::setprecision(3) << (Options::hdBedrockLutTopPermille / 1000.0)
						<< " gradeVertical=" << (Options::hdBedrockLutVerticalPermille / 1000.0)
						<< " gradeCovered=" << (Options::hdBedrockLutCoveredPermille / 1000.0)
						<< " depthLumaV1=" << (Options::hdDepthLuminance ? "ON" : "OFF")
						<< " lumaD1=" << (Options::hdBedrockLumaD1Permille / 1000.0)
						<< " lumaD3=" << (Options::hdBedrockLumaD3Permille / 1000.0)
						<< " missionDepth=" << _save->getDepth()
						<< " depthLumaFactor=" << std::fixed << std::setprecision(3)
						<< (bedrock.depthLuminancePermille / 1000.0);

					if (!semanticDebrisMcdIds.empty())
					{
						std::ostringstream ids;
						bool first = true;
						for (const auto &entry : semanticDebrisMcdIds)
						{
							if (!first) ids << ",";
							ids << entry.first << ":" << entry.second;
							first = false;
						}
						Log(LOG_INFO) << "[BEDROCK STRICT OWNERSHIP V2] DEBRIS_MCD=" << ids.str()
							<< " allowed=32..49 graphical-frame-routing=NONE";
					}
					loggedViewLevel = viewLevel;
				}
			}
		}
	}
	perf.mapBedrockOwnerUs += hdPerfNowUs() - perfBedrockOwnerStart;
	const uint64_t perfWorldReplayStart = hdPerfNowUs();
	// REAL HD WORLD COMPOSITOR V1. The old renderer made every authored HD
	// sprite sample _drawOrderBuffer, so a Legacy raster silhouette remained the
	// final authority over HD pixels. That dependency ends here. 2D compatibility
	// primitives compose in explicit GPU submission sequence; primitives with real
	// world geometry use the independent D3D11 geometric depth target. Only
	// CPU-rasterized compatibility pixels are replayed explicitly between GPU
	// submissions, and they never write geometric depth.
	_hdLegacyBridgeRanges = 0;
	_hdLegacyGpuCompatCommands = 0;
	// The native pixel bridge is permanently disabled below. Scanning the
	// logical map's draw-order pixels here cannot affect the GPU image.
	auto bridgeHasPixels = [&](unsigned first, unsigned last) -> bool
	{
		(void)first; (void)last;
		return false; // Native raster is never replayed in REAL HD.
	};
	auto replayLegacyBridge = [&](unsigned first, unsigned last) -> bool
	{
		if (!bridgeHasPixels(first, last)) return true;
		HdGpuMapLegacyOverlay bridge;
		bridge.destX = px0; bridge.destY = py0; bridge.destW = px1 - px0; bridge.destH = py1 - py0;
		bridge.mapPhysicalOriginX = (double)screen->logicalToPhysicalX(logicalMapX);
		bridge.mapPhysicalOriginY = (double)screen->logicalToPhysicalY(logicalMapY);
		bridge.renderScaleX = sx; bridge.renderScaleY = sy;
		bridge.sourceLogicalX = logicalMapX; bridge.sourceLogicalY = logicalMapY;
		bridge.minDrawOrder = first; bridge.maxDrawOrder = std::min(last, _drawSequence);
		++_hdLegacyBridgeRanges;
		return gpu.drawMapLegacyOverlay(bridge);
	};

	unsigned legacyGpuCommands = 0;
	unsigned bridgeCursor = 1;
	std::vector<HdGpuMapSprite> postVisibilitySprites;
	// Sort only indices: the persistent static prefix must remain intact across
	// camera redraws. Units can then change pose without rebuilding terrain.
	std::vector<size_t> sceneOrder;
	sceneOrder.reserve(_hdDrawCommands.size());
	for (size_t i = 0; i < _hdDrawCommands.size(); ++i) sceneOrder.push_back(i);
	std::stable_sort(sceneOrder.begin(), sceneOrder.end(), [&](size_t a, size_t b)
	{
		const auto &lhs = _hdDrawCommands[a], &rhs = _hdDrawCommands[b];
		if (lhs.postVisibility != rhs.postVisibility) return !lhs.postVisibility;
		const bool lhsWorld = lhs.worldAnchored || lhs.worldDynamic;
		const bool rhsWorld = rhs.worldAnchored || rhs.worldDynamic;
		if (lhsWorld != rhsWorld) return lhsWorld;
		if (!lhsWorld) return false;
		if (lhs.worldZ != rhs.worldZ) return lhs.worldZ < rhs.worldZ;
		const int lhsDiagonal = lhs.worldX + lhs.worldY;
		const int rhsDiagonal = rhs.worldX + rhs.worldY;
		if (lhsDiagonal != rhsDiagonal) return lhsDiagonal < rhsDiagonal;
		if (lhs.worldY != rhs.worldY) return lhs.worldY < rhs.worldY;
		if (lhs.worldX != rhs.worldX) return lhs.worldX < rhs.worldX;
		return lhs.worldLayer < rhs.worldLayer;
	});
    std::vector<bool> replacedUnitCommands(_hdDrawCommands.size(), false);
    for(size_t commandIndex=0;commandIndex<sceneOrder.size();++commandIndex)
    {
        const auto &cmd=_hdDrawCommands[sceneOrder[commandIndex]];
		if (replacedUnitCommands[commandIndex] && !cmd.unit3DKeepLayer) continue;
		// The persistent world inventory contains every terrain part. Camera
		// translation selects the nearby instances without re-reading map tiles.
		if (cmd.worldAnchored)
		{
			if (!_camera->getShowAllLayers() && cmd.worldZ > _camera->getViewLevel()) continue;
			if (_camera->getShowSingleLayer() && cmd.worldZ != _camera->getViewLevel()) continue;
			const Position view = _camera->getMapOffset();
			const int marginX = _spriteWidth * 4;
			const int marginY = _spriteHeight * 4;
			if (cmd.x + view.x < -marginX || cmd.y + view.y < -marginY ||
				cmd.x + view.x > getWidth() + marginX ||
				cmd.y + view.y > _visibleMapHeight + marginY) continue;
		}
		if (cmd.surfaceCursor)
		{
			if (cmd.drawOrder > bridgeCursor &&
				!replayLegacyBridge(bridgeCursor, cmd.drawOrder - 1u))
			{
				gpu.endMap(); return false;
			}
			bridgeCursor = std::max(bridgeCursor, cmd.drawOrder + 1u);
			for (const HdSurfaceCursorQuad &q : _hdSurfaceCursorQuads)
			{
				if (q.tile != cmd.cursorTile) continue;
				std::array<HdGpuCursorVertex, 4> lower, upper;
				const float physicalOriginX = (float)screen->logicalToPhysicalX(logicalMapX);
				const float physicalOriginY = (float)screen->logicalToPhysicalY(logicalMapY);
                const float rise = Position::TileZ * mapScale * (float)sy;
                const float cornerWorldSum[4] = {
                    float((q.tile.x + q.tile.y) * Position::TileXY),
                    float((q.tile.x + q.tile.y + 1) * Position::TileXY),
                    float((q.tile.x + q.tile.y + 2) * Position::TileXY),
                    float((q.tile.x + q.tile.y + 1) * Position::TileXY)};
				for (int i = 0; i < 4; ++i)
				{
					lower[i] = {physicalOriginX + q.xy[2*i] * (float)sx,
						physicalOriginY + q.xy[2*i+1] * (float)sy,
						q.depth[i] + 1.0f, cmd.cursorYellow && (_animFrame % 2) ? 0.30f : 1.0f};
                    // Recover each corner's terrain offset. Only the base
                    // follows that offset; the top is one horizontal Z plane.
                    const float flatDepth = cornerWorldSum[i] + q.tile.z * Position::TileZ;
                    const float terrainOffset = flatDepth - q.depth[i];
                    upper[i] = {lower[i].x,
                        lower[i].y - terrainOffset * mapScale * (float)sy - rise,
                        flatDepth + Position::TileZ + 1.0f, lower[i].alpha};
				}
				std::vector<HdGpuCursorVertex> lines;
				lines.reserve(12 * 6);
				auto edge = [&](const HdGpuCursorVertex &a, const HdGpuCursorVertex &b)
				{
					const float dx = b.x-a.x, dy = b.y-a.y;
					const float inv = 0.85f / std::sqrt(std::max(0.001f, dx*dx+dy*dy));
					const float ox = -dy*inv, oy = dx*inv;
					HdGpuCursorVertex a0{a.x+ox,a.y+oy,a.viewDepth,a.alpha};
					HdGpuCursorVertex a1{a.x-ox,a.y-oy,a.viewDepth,a.alpha};
					HdGpuCursorVertex b0{b.x+ox,b.y+oy,b.viewDepth,b.alpha};
					HdGpuCursorVertex b1{b.x-ox,b.y-oy,b.viewDepth,b.alpha};
					lines.insert(lines.end(), {a0,b0,a1,b0,b1,a1});
				};
				for (int i = 0; i < 4; ++i)
				{
					const int next = (i+1)&3;
					// 0=back, 1=right, 2=front, 3=left in the projected diamond.
					const bool frontEdge = i == 1 || i == 2;
					if (frontEdge == cmd.surfaceCursorFront)
					{
						edge(lower[i], lower[next]);
						if (!cmd.surfaceCursorGuide) edge(upper[i], upper[next]);
					}
                    if ((i != 0) == cmd.surfaceCursorFront)
                    {
                        if (!cmd.surfaceCursorGuide) edge(lower[i], upper[i]);
                        else
                        {
                            // Dashes end at the true ground height, never at
                            // the flat legacy tile plane inside the terrain.
                            const float length = std::abs(upper[i].y - lower[i].y);
                            auto point = [&](float t) {
                                return HdGpuCursorVertex{lower[i].x,
                                    lower[i].y + (upper[i].y-lower[i].y)*t,
                                    lower[i].viewDepth + (upper[i].viewDepth-lower[i].viewDepth)*t,
                                    lower[i].alpha};
                            };
                            for (float d = 0; d < length; d += 7.0f)
                                edge(point(d/length), point(std::min(d+2.0f,length)/length));
                        }
                    }
				}
				if (!gpu.drawMapSurfaceCursor(lines.data(), (unsigned)lines.size(),
					cmd.cursorYellow, cmd.surfaceCursorGuide)) { gpu.endMap(); return false; }
				break;
			}
			continue;
		}
        if(!cmd.unit3DModel.empty() && cmd.unit3DCommandCount && !_unit3DGpuFailed)
        {
            // Keep the original PNG group intact: it is replayed if 3D fails.
            HdGpuMapSprite unit;
            unit.destX=screen->logicalToPhysicalX(logicalMapX+cmd.unit3DX);
            unit.destY=screen->logicalToPhysicalY(logicalMapY+cmd.unit3DY);
            unit.destW=screen->logicalToPhysicalX(logicalMapX+cmd.unit3DX+32*mapScale)-unit.destX;
            unit.destH=screen->logicalToPhysicalY(logicalMapY+cmd.unit3DY+40*mapScale)-unit.destY;
            unit.baseRenderX=HdRenderSpace::legacyToRender(double(cmd.unit3DX));
            unit.baseRenderY=HdRenderSpace::legacyToRender(double(cmd.unit3DY));
            unit.renderToPhysicalX=renderToPhysicalX;unit.renderToPhysicalY=renderToPhysicalY;
            unit.imageWidth=512;unit.imageHeight=640;unit.colorMode=HdColorMode::Fixed;
            unit.drawOrder=cmd.drawOrder;unit.shade=cmd.shade;
            unit.lightTintR=cmd.lightTintR;unit.lightTintG=cmd.lightTintG;unit.lightTintB=cmd.lightTintB;
            unit.hasClipMask=true;unit.clipBegX=cmd.unit3DMask.beg_x;unit.clipBegY=cmd.unit3DMask.beg_y;
            unit.clipEndX=cmd.unit3DMask.end_x;unit.clipEndY=cmd.unit3DMask.end_y;
            if(cmd.drawOrder>bridgeCursor && !replayLegacyBridge(bridgeCursor,cmd.drawOrder-1u)) {gpu.endMap();return false;}
            bridgeCursor=std::max(bridgeCursor,cmd.drawOrder);
            if(gpu.drawMapUnit3D(unit,cmd.unit3DModel,cmd.unit3DPose))
            {
                hdMarkUnitReplacement(replacedUnitCommands,commandIndex,cmd.unit3DCommandCount);
                if (!cmd.unit3DKeepLayer) continue;
            }
            else
            {
                _unit3DGpuFailed=true;
                if(_unit3DReports.insert("gpu-failure").second)
                {
                    Log(LOG_WARNING)<<"[AQUANAUTE3D][FALLBACK] reason=3d-submit-failed selected=EXISTING_PNG_CHAIN retry=RESTART";
                }
            }
        }
		HdGpuMapSprite sprite;
		double baseRenderX = 0.0, baseRenderY = 0.0;
		int globalX0 = 0, globalY0 = 0, globalX1 = 0, globalY1 = 0;

		if (!cmd.generatedRgba.empty())
		{
			baseRenderX = HdRenderSpace::legacyToRender(double(cmd.x));
			baseRenderY = HdRenderSpace::legacyToRender(double(cmd.y));
			globalX0 = screen->logicalToPhysicalX(logicalMapX+cmd.x);
			globalY0 = screen->logicalToPhysicalY(logicalMapY+cmd.y);
			globalX1 = screen->logicalToPhysicalX(logicalMapX+cmd.x+2*mapScale);
			globalY1 = screen->logicalToPhysicalY(logicalMapY+cmd.y+2*mapScale);
			sprite.assetKey = cmd.assetPath.c_str(); sprite.rgba=cmd.generatedRgba.data();
			sprite.imageWidth=2; sprite.imageHeight=2; sprite.premultiplied=true;
		}
		else if (cmd.legacyRaw)
		{
			// A missing authored replacement may still use its indexed source
			// image as a D3D texture. No OXCE raster pixels/order are consumed.
			if (!cmd.legacyIndices || !cmd.legacyWidth || !cmd.legacyHeight) continue;
			const Position viewOffset = cmd.worldAnchored ? _camera->getMapOffset() : Position();
			baseRenderX = HdRenderSpace::legacyToRender((double)(cmd.x + viewOffset.x));
			baseRenderY = HdRenderSpace::legacyToRender((double)(cmd.y + viewOffset.y));
			const double renderDrawW = HdRenderSpace::legacyToRender((double)cmd.legacyWidth * mapScale);
			const double renderDrawH = HdRenderSpace::legacyToRender((double)cmd.legacyHeight * mapScale);
			globalX0 = screen->logicalToPhysicalX(logicalMapX + HdRenderSpace::renderToLegacy(baseRenderX));
			globalY0 = screen->logicalToPhysicalY(logicalMapY + HdRenderSpace::renderToLegacy(baseRenderY));
			globalX1 = screen->logicalToPhysicalX(logicalMapX + HdRenderSpace::renderToLegacy(baseRenderX + renderDrawW));
			globalY1 = screen->logicalToPhysicalY(logicalMapY + HdRenderSpace::renderToLegacy(baseRenderY + renderDrawH));
			if (globalX1 <= px0 || globalY1 <= py0 || globalX0 >= px1 || globalY0 >= py1) continue;
			sprite.assetKey = cmd.legacyKey.c_str();
			sprite.indices = cmd.legacyIndices;
			sprite.sourcePitch = cmd.legacyPitch;
			sprite.colorMode = HdColorMode::IndexedLegacy;
			sprite.legacyIndexed = true;
			sprite.imageWidth = cmd.legacyWidth;
			sprite.imageHeight = cmd.legacyHeight;
		}
		else
		{
			HdImage *image = _hdImageCache.get(cmd.assetPath);
			if (!image || !image->loaded || !image->width || !image->height || !image->hasVisiblePixels) continue;

			const int nativeScale = HdRenderSpace::resolveNativeScale(cmd.nativeScale, image->width, image->height);
			const double renderDrawW = HdRenderSpace::assetPixelsToRender((double)image->width * mapScale, nativeScale);
			const double renderDrawH = HdRenderSpace::assetPixelsToRender((double)image->height * mapScale, nativeScale);
			const double renderOffsetX = HdRenderSpace::assetPixelsToRender((double)cmd.offsetX * mapScale, nativeScale);
			const double renderOffsetY = HdRenderSpace::assetPixelsToRender((double)cmd.offsetY * mapScale, nativeScale);
			const Position viewOffset = cmd.worldAnchored ? _camera->getMapOffset() : Position();
			baseRenderX = HdRenderSpace::legacyToRender((double)(cmd.x + viewOffset.x)) + renderOffsetX;
			baseRenderY = HdRenderSpace::legacyToRender((double)(cmd.y + viewOffset.y)) + renderOffsetY;
			globalX0 = screen->logicalToPhysicalX(logicalMapX + HdRenderSpace::renderToLegacy(baseRenderX));
			globalY0 = screen->logicalToPhysicalY(logicalMapY + HdRenderSpace::renderToLegacy(baseRenderY));
			globalX1 = screen->logicalToPhysicalX(logicalMapX + HdRenderSpace::renderToLegacy(baseRenderX + renderDrawW));
			globalY1 = screen->logicalToPhysicalY(logicalMapY + HdRenderSpace::renderToLegacy(baseRenderY + renderDrawH));
			if (globalX1 <= px0 || globalY1 <= py0 || globalX0 >= px1 || globalY0 >= py1) continue;

			sprite.assetKey = cmd.assetPath.c_str();
			sprite.rgba = image->rgba.data();
			sprite.indices = image->indices.empty() ? nullptr : image->indices.data();
			sprite.colorMode = resolveHdColorMode(cmd.colorMode, image->paletteIndexed8);
			sprite.legacyIndexed = sprite.colorMode == HdColorMode::IndexedLegacy;
			if (sprite.colorMode == HdColorMode::Environment)
			{
				HdMaterialProfile profile = cmd.materialProfile;
				if (profile == HdMaterialProfile::Auto) profile = hdMaterialProfileForAssetPath(cmd.assetPath);
				const HdMaterialGradeParams material = hdMaterialGradeGet(profile, _save ? _save->getDepth() : 0);
				sprite.materialGradeEnabled = Options::hdMaterialGrade;
				sprite.materialTintR = hdMaterialGradeFloat(material.tintR);
				sprite.materialTintG = hdMaterialGradeFloat(material.tintG);
				sprite.materialTintB = hdMaterialGradeFloat(material.tintB);
				sprite.materialExposure = hdMaterialGradeFloat(material.exposure);
				sprite.materialContrast = hdMaterialGradeFloat(material.contrast);
				sprite.materialSaturation = hdMaterialGradeFloat(material.saturation);
			}
			sprite.imageWidth = image->width;
			sprite.imageHeight = image->height;
		}

		sprite.destX = globalX0;
		sprite.destY = globalY0;
		sprite.destW = std::max(1, globalX1 - globalX0);
		sprite.destH = std::max(1, globalY1 - globalY0);
		sprite.baseRenderX = baseRenderX;
		sprite.baseRenderY = baseRenderY;
		sprite.renderToPhysicalX = renderToPhysicalX;
		sprite.renderToPhysicalY = renderToPhysicalY;
		sprite.drawOrder = cmd.drawOrder;
		sprite.shade = cmd.shade;
		sprite.lightTintR = cmd.lightTintR;
		sprite.lightTintG = cmd.lightTintG;
		sprite.lightTintB = cmd.lightTintB;
        sprite.roofCaustic=cmd.roofCaustic;
        sprite.roofSunRows=cmd.roofSunRows;
        sprite.roofWorldX=(float)cmd.worldX;sprite.roofWorldY=(float)cmd.worldY;
        sprite.roofTime=(float)(SDL_GetTicks()*0.00012);

		sprite.legacyBaseColor = cmd.legacyBaseColor;
		sprite.rightHalfOnly = cmd.rightHalfOnly;
		sprite.hasClipMask = cmd.hasClipMask;
		if (cmd.hasClipMask)
		{
			sprite.clipBegX = cmd.clipMask.beg_x;
			sprite.clipBegY = cmd.clipMask.beg_y;
			sprite.clipEndX = cmd.clipMask.end_x;
			sprite.clipEndY = cmd.clipMask.end_y;
		}
		if (cmd.postVisibility)
		{
			postVisibilitySprites.push_back(sprite);
		}
		else
		{
			if (cmd.drawOrder > bridgeCursor && !replayLegacyBridge(bridgeCursor, cmd.drawOrder - 1u))
			{
				gpu.endMap();
				return false;
			}
			if (!gpu.drawMapSprite(sprite))
			{
				gpu.endMap();
				return false;
			}
			bridgeCursor = std::max(bridgeCursor, cmd.drawOrder + 1u);
		}
	}

	if (bridgeCursor <= _drawSequence && !replayLegacyBridge(bridgeCursor, _drawSequence))
	{
		gpu.endMap();
		return false;
	}

	_hdLegacyGpuCompatCommands = legacyGpuCommands;
	static bool loggedWorldCompositorV1 = false;
	if (!loggedWorldCompositorV1)
	{
		Log(LOG_INFO) << "[REAL HD WORLD COMPOSITOR V1][ACTIVE] ownership=D3D11"
			<< " authoredHdLegacyOrderSample=OFF bedrockLegacyOrderSample=OFF"
			<< " legacyCpuRaster=EXPLICIT_RANGE_BRIDGE geometryDepth=BEDROCK_WORLD_XYZ";
		loggedWorldCompositorV1 = true;
	}

	perf.mapWorldReplayUs += hdPerfNowUs() - perfWorldReplayStart;
	const uint64_t perfPostFxStart = hdPerfNowUs();
	// HD LOCAL LIGHTS V2 — universal post-light pass. Because this runs after all
	// Battlescape world compositing, it affects BEDROCK, vertical faces, authored HD
	// PNGs, Legacy indexed terrain/objects/units and future map assets automatically.
	// The visibility mask below is applied afterwards, so local lights cannot reveal
	// undiscovered gameplay information.
	if (!hdLocalLights.empty() && !gpu.drawMapLocalLights(hdLocalLights.data(), (unsigned)hdLocalLights.size()))
	{
		gpu.endMap();
		return false;
	}
	if (!hdSmokeBlobs.empty() && !gpu.drawMapSmokeVolume(hdSmokeBlobs.data(), (unsigned)hdSmokeBlobs.size()))
	{
		gpu.endMap();
		return false;
	}
	if (!hdSedimentBlobs.empty() && !gpu.drawMapSmokeVolume(hdSedimentBlobs.data(), (unsigned)hdSedimentBlobs.size()))
	{
		gpu.endMap();
		return false;
	}

	// REAL HD FOV DELUXE PREMIUM JEAN-MICHEL EDITION V1.
	//
	// V4's mono-Z inverse-projected screen mask is intentionally gone. The world is
	// now visibility-owned at the source: every floor/object/wall command is accepted
	// or consumed from its real Tile + TilePart + Z before it reaches the GPU, and
	// BEDROCK already emits only discovered source cells using its real raised geometry.
	// This makes multi-level composition native: Z0..viewLevel can coexist without a
	// current-Z black sheet slicing through roofs, mezzanines or dunes that rise across
	// logical height bands. Undiscovered space stays black because the REAL-HD tactical
	// surface is cleared opaque black before world rendering.
	//
	// REAL HD mission knowledge is published by the native physical-scene FOV.
	// Presentation still consumes the existing discovery/visibility storage.
	// Cursor/UI sprites flagged postVisibility are still replayed last.
	if (Options::hdGraphics)
	{
		static bool loggedRealHdFovV1 = false;
		if (!loggedRealHdFovV1)
		{
			Log(LOG_INFO) << "[REAL HD FOV R4][ACTIVE] presentation=SOURCE_OWNED_MULTI_Z"
				<< " terrainFog=WORLD_COORDINATE_BILINEAR screenFog=CONTINUOUS_FIELD"
				<< " legacyShade16=BYPASSED bedrockRaisedGeometry=NATIVE cursorOverlay=POST_WORLD";
			loggedRealHdFovV1 = true;
		}
	}

	// Rasterize a continuous HD disclosure field over the complete physical ground.
	// Mission knowledge supplies samples at tile centres, but does not pre-cut the
	// terrain mesh at tile edges. Gameplay and dynamic entities retain their own
	// knowledge gates. The field can therefore disclose part of a ground tile.
	if (!_hdFogSurfaceQuads.empty())
	{
		std::vector<HdGpuVisibilityVertex> visionSurface;
		visionSurface.reserve(_hdFogSurfaceQuads.size() * 12u);
		const float originX = (float)screen->logicalToPhysicalX(logicalMapX);
		const float originY = (float)screen->logicalToPhysicalY(logicalMapY);
		const int fogW = _save->getMapSizeX(), fogH = _save->getMapSizeY();
		auto knownGround = [&](int x, int y, int z) -> float
		{
			if (x < 0 || y < 0 || x >= fogW || y >= fogH) return 0.0f;
			Tile *tile = _save->getTile(Position(x, y, z));
			return RealHdWorldPresentation::allows(tile, O_FLOOR,
				RealHdWorldPresentationClass::PersistentGeometry) ? 1.0f : 0.0f;
		};
		for (const HdSurfaceCursorQuad &q : _hdFogSurfaceQuads)
		{
			const int x = q.tile.x, y = q.tile.y, z = q.tile.z;
			HdGpuVisibilityVertex v[5];
			const int cornerX[4] = {x, x + 1, x + 1, x};
			const int cornerY[4] = {y, y, y + 1, y + 1};
			for (int i = 0; i < 4; ++i)
			{
				v[i].x = originX + q.xy[2*i] * (float)sx;
				v[i].y = originY + q.xy[2*i+1] * (float)sy;
				const int cx = cornerX[i], cy = cornerY[i];
				v[i].alpha = 0.25f * (knownGround(cx - 1, cy - 1, z) +
					knownGround(cx, cy - 1, z) + knownGround(cx - 1, cy, z) +
					knownGround(cx, cy, z));
			}
			v[4].x = 0.25f * (v[0].x + v[1].x + v[2].x + v[3].x);
			v[4].y = 0.25f * (v[0].y + v[1].y + v[2].y + v[3].y);
			v[4].alpha = knownGround(x, y, z);
			for (int i = 0; i < 4; ++i)
				visionSurface.insert(visionSurface.end(), {v[4], v[i], v[(i + 1) & 3]});
		}
		if (!gpu.applyMapVisionFeather(visionSurface.data(),
			(unsigned)visionSurface.size(), Options::hdFogEdgeSoftnessPermille))
		{
			gpu.endMap();
			return false;
		}
	}

	// REAL HD FOV V1 post-world replay. Tactical cursor/UI remains fully readable over
	// black. Boundary walls are no longer delayed: because there is no global blackout
	// mask, discovered walls participate normally in lighting/smoke/world composition.
	for (const auto &sprite : postVisibilitySprites)
	{
		if (!gpu.drawMapSprite(sprite))
		{
			gpu.endMap();
			return false;
		}
	}

	gpu.endMap();
	perf.mapPostFxUs += hdPerfNowUs() - perfPostFxStart;

	perf.mapCommands = (unsigned)_hdDrawCommands.size();
	perf.mapLegacyGpuCommands = legacyGpuCommands;
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

	// The logical Map already contains the complete hidden-movement/end-turn
	// replacement screen.  Do not composite any tactical-world HD layer on top.
	if (_hdPhysicalMapSuppressed)
	{
		const int x = screen->logicalToPhysicalX(getDisplayX()+_message->getDisplayX());
		const int y = screen->logicalToPhysicalY(getDisplayY()+_message->getDisplayY());
		const int w = screen->logicalToPhysicalX(getDisplayX()+_message->getDisplayX()+_message->getWidth())-x;
		const int h = screen->logicalToPhysicalY(getDisplayY()+_message->getDisplayY()+_message->getHeight())-y;
		screen->tryBlitHdSurfaceAt(_message, destination, x, y, w, h);
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
			if (_hdExplosionFlash)
			{
				static const unsigned char white[4] = {255,255,255,255};
				HdGpuPresentationSprite flash;
				flash.assetKey="REAL_HD_EXPLOSION_FLASH"; flash.rgba=white; flash.imageWidth=flash.imageHeight=1;
				flash.destX=screen->logicalToPhysicalX(getDisplayX()); flash.destY=screen->logicalToPhysicalY(getDisplayY());
				flash.destW=screen->logicalToPhysicalX(getDisplayX()+getWidth())-flash.destX;
				flash.destH=screen->logicalToPhysicalY(getDisplayY()+_visibleMapHeight)-flash.destY;
				flash.opacity=96; HdGpuBackend::instance().drawPresentationSprite(flash);
				_hdExplosionFlash=false;
			}
			const int scale = std::max(1, _spriteWidth / 32);
			auto label = [&](Surface *text, int x, int y)
			{
				const int px = screen->logicalToPhysicalX(getDisplayX()+x);
				const int py = screen->logicalToPhysicalY(getDisplayY()+y);
				const int pw = screen->logicalToPhysicalX(getDisplayX()+x+text->getWidth()*scale)-px;
				const int ph = screen->logicalToPhysicalY(getDisplayY()+y+text->getHeight()*scale)-py;
				screen->tryBlitHdSurfaceAt(text, destination, px, py, pw, ph);
			};
			NumberText number(32,8);
			number.setPalette(getPalette()); number.setBordered(true);
			for (const auto &entry : _hdTacticalNumbers)
			{
				number.setValue(entry.value); number.setColor(entry.color);
				label(&number, entry.x, entry.y);
			}
			if (_hdCursorInfoVisible)
			{
				Position cursor; _camera->convertMapToScreen(Position(_selectorX,_selectorY,_camera->getViewLevel()), &cursor);
				cursor += _camera->getMapOffset();
				label(_txtAccuracy, cursor.x, cursor.y);
			}
			perf.mapPhysicalUs = hdPerfNowUs() - perfPhysicalStart;
			return;
		}
		throw Exception("REAL HD world pass failed: native map display is forbidden");
		hdTraceRoute("world-compositor", "map", "HD_SOFTWARE_WITH_COMPATIBILITY",
			"GPU world pass failed; software PNG compositor and explicit native bridge retained");
		HdGpuBackend::instance().abortFrame();
	}

    if(!_unit3DReports.count("software-compositor"))
    {
        for(const auto &cmd:_hdDrawCommands) if(!cmd.unit3DModel.empty())
        {
            _unit3DReports.insert("software-compositor");
            Log(LOG_WARNING)<<"[AQUANAUTE3D][FALLBACK] reason=GPU-world-unavailable selected=EXISTING_PNG_CHAIN";
            break;
        }
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
		if (cmd.legacyRaw)
		{
			hashBytes(cmd.legacyKey.data(), cmd.legacyKey.size());
			hashUnsigned(cmd.legacyWidth); hashUnsigned(cmd.legacyHeight); hashUnsigned(cmd.legacyPitch);
			hashInt(cmd.legacyBaseColor);
		}
		else
		{
			hashBytes(cmd.assetPath.data(), cmd.assetPath.size());
		}
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
	if (_save)
	{
		const unsigned long long craterRevision = _save->getBedrockCraterRevision();
		hashBytes(&craterRevision, sizeof(craterRevision));
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
		
			for (const auto &cmd : _hdDrawCommands)
			{
				const bool rawLegacy = cmd.legacyRaw;
				HdImage *image = nullptr;
				unsigned srcW = 0, srcH = 0;
				if (rawLegacy)
				{
					if (!cmd.legacyIndices || !cmd.legacyWidth || !cmd.legacyHeight) continue;
					srcW = cmd.legacyWidth;
					srcH = cmd.legacyHeight;
				}
				else
				{
					image = _hdImageCache.get(cmd.assetPath);
					if (!image || !image->loaded || !image->width || !image->height || !image->hasVisiblePixels) continue;
					srcW = image->width;
					srcH = image->height;
				}

				double renderDrawW, renderDrawH, baseRenderX, baseRenderY;
				if (rawLegacy)
				{
					renderDrawW = HdRenderSpace::legacyToRender((double)srcW);
					renderDrawH = HdRenderSpace::legacyToRender((double)srcH);
					baseRenderX = HdRenderSpace::legacyToRender((double)cmd.x);
					baseRenderY = HdRenderSpace::legacyToRender((double)cmd.y);
				}
				else
				{
					const int nativeScale = HdRenderSpace::resolveNativeScale(cmd.nativeScale, srcW, srcH);
					renderDrawW = HdRenderSpace::assetPixelsToRender((double)srcW * mapScale, nativeScale);
					renderDrawH = HdRenderSpace::assetPixelsToRender((double)srcH * mapScale, nativeScale);
					const double renderOffsetX = HdRenderSpace::assetPixelsToRender((double)cmd.offsetX * mapScale, nativeScale);
					const double renderOffsetY = HdRenderSpace::assetPixelsToRender((double)cmd.offsetY * mapScale, nativeScale);
					baseRenderX = HdRenderSpace::legacyToRender((double)cmd.x) + renderOffsetX;
					baseRenderY = HdRenderSpace::legacyToRender((double)cmd.y) + renderOffsetY;
				}

				const int globalX0 = screen->logicalToPhysicalX(logicalMapX + HdRenderSpace::renderToLegacy(baseRenderX));
				const int globalY0 = screen->logicalToPhysicalY(logicalMapY + HdRenderSpace::renderToLegacy(baseRenderY));
				const int globalX1 = screen->logicalToPhysicalX(logicalMapX + HdRenderSpace::renderToLegacy(baseRenderX + renderDrawW));
				const int globalY1 = screen->logicalToPhysicalY(logicalMapY + HdRenderSpace::renderToLegacy(baseRenderY + renderDrawH));
				const int drawW = std::max(1, globalX1 - globalX0);
				const int drawH = std::max(1, globalY1 - globalY0);
				const int startX = cmd.rightHalfOnly ? drawW / 2 : 0;
				const double legacyLight = (16.0 - std::max(0, std::min(16, cmd.shade))) / 16.0;
				const double lightFactor = legacyLight;

				xSource.resize((size_t)drawW);
				xMap.resize((size_t)drawW);
				for (int xx = 0; xx < drawW; ++xx)
				{
					xSource[(size_t)xx] = std::min(srcW - 1,
						(unsigned)((unsigned long long)xx * srcW / (unsigned)drawW));
					const double renderX = baseRenderX + (double)xx / renderToPhysicalX;
					xMap[(size_t)xx] = (int)std::floor(HdRenderSpace::renderToLegacy(renderX));
				}

				for (int yy = 0; yy < drawH; ++yy)
				{
					const int gy = globalY0 + yy;
					const int cy = gy - py0;
					if (cy < 0 || cy >= cacheH) continue;

					const unsigned iy = std::min(srcH - 1,
						(unsigned)((unsigned long long)yy * srcH / (unsigned)drawH));
					unsigned sourceMin = 0, sourceMax = srcW;
					if (!rawLegacy)
					{
						if (iy >= image->alphaRowMinX.size() || iy >= image->alphaRowMaxX.size()) continue;
						sourceMin = image->alphaRowMinX[iy];
						sourceMax = image->alphaRowMaxX[iy];
						if (sourceMin >= sourceMax) continue;
					}

					const double renderY = baseRenderY + (double)yy / renderToPhysicalY;
					const int mapY = (int)std::floor(HdRenderSpace::renderToLegacy(renderY));
					if (mapY < 0 || mapY >= getHeight()) continue;
					const size_t orderRow = (size_t)mapY * getWidth();

					const int spanStart = (int)(((unsigned long long)sourceMin * drawW + srcW - 1) / srcW);
					const int spanEnd = (int)(((unsigned long long)sourceMax * drawW + srcW - 1) / srcW);
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
						Uint8 sa = 255, sr = 0, sg = 0, sb = 0;
						if (rawLegacy)
						{
							const Uint8 srcIndex = cmd.legacyIndices[(size_t)iy * cmd.legacyPitch + ix];
							if (!srcIndex) continue;
							Uint8 finalIndex;
							if (cmd.legacyBaseColor)
							{
								const int newShade = (srcIndex & 0x0F) + std::max(0, std::min(16, cmd.shade));
								finalIndex = (newShade & 0xF0) ? 0x0F : (Uint8)(((cmd.legacyBaseColor - 1) << 4) | (newShade & 0x0F));
							}
							else
							{
								const Uint8 shaded = (Uint8)(srcIndex + std::max(0, std::min(16, cmd.shade)));
								finalIndex = ((shaded ^ srcIndex) & 0xF0) ? 0x0F : shaded;
							}
							const SDL_Color &pc = activePalette[finalIndex];
							sr = pc.r; sg = pc.g; sb = pc.b;
						}
						else
						{
							const size_t pixelIndex = (size_t)iy * image->width + ix;
							const HdColorMode colorMode = resolveHdColorMode(cmd.colorMode, image->paletteIndexed8);
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
								if (colorMode == HdColorMode::Environment && Options::hdMaterialGrade)
					{
						HdMaterialProfile profile = cmd.materialProfile;
						if (profile == HdMaterialProfile::Auto) profile = hdMaterialProfileForAssetPath(cmd.assetPath);
						hdApplyMaterialGrade(sr, sg, sb, hdMaterialGradeGet(profile, _save ? _save->getDepth() : 0));
					}
								sr = (Uint8)std::lround((double)sr * lightFactor);
								sg = (Uint8)std::lround((double)sg * lightFactor);
								sb = (Uint8)std::lround((double)sb * lightFactor);
							}
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
	// REAL HD LIGHTING V2: helmet light may make a hostile visually readable in
	// already-known terrain without changing OXCE detection/HUD state. The field
	// is LOS-blocked and only extends to the configured +/-90 degree outer halo.
	const bool helmetVisualContact = Options::hdGraphics && unitTile->isDiscovered(O_FLOOR) &&
		realHdHelmetLightAt(unitTile->getPosition()) > 0.01f;
	if (!(bu->getVisible() || flareVisualContact || helmetVisualContact || _save->getDebugMode()))
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

	if (moving && !_hdProducingDynamic)
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
	if (_hdProducingDynamic)
	{
		// Legacy slices stationary units too when their head crosses an upper
		// tile, then draws the missing portion in another traversal pass. The
		// direct producer emits the complete unit once, at every camera level.
		// Keep viewport clipping; scene composition owns terrain occlusion.
		mask = GraphSubset(getWidth(), _visibleMapHeight);
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
	auto markLastUnitGrade = [&]()
	{
		if (_hdDrawCommands.empty()) return;
		_hdDrawCommands.back().materialProfile = HdMaterialProfile::Unit;
		float tr = 1.0f, tg = 1.0f, tb = 1.0f;
		realHdLightingTintAt(unitTile, tr, tg, tb);
		_hdDrawCommands.back().lightTintR = tr;
		_hdDrawCommands.back().lightTintG = tg;
		_hdDrawCommands.back().lightTintB = tb;
	};
    const Armor *partsArmor = bu->getArmor();
    const bool partsMode = Options::hdGraphics && partsArmor &&
        !partsArmor->getHdUnitPartsRoot().empty() && partsArmor->getHdUnitPartsScale() > 0;
    if (partsMode)
    {
        const size_t firstUnitCommand = _hdDrawCommands.size();
        bool unit3DSafe = true;
        const bool wants3D = !partsArmor->getHdUnit3DModel().empty();
        if(wants3D && _unit3DReports.insert("requested:"+partsArmor->getHdUnit3DModel()).second)
        {
            Log(LOG_INFO)<<"[AQUANAUTE3D][REQUEST] asset="<<partsArmor->getHdUnit3DModel()<<" logicalHeight="<<partsArmor->getHdUnit3DHeight();
        }
        const auto status = bu->getStatus();
        const bool supported3D = (status==STATUS_STANDING || status==STATUS_WALKING ||
            status==STATUS_TURNING || status==STATUS_AIMING) && !bu->getFire() && partsArmor->getSize()==1;
        if(wants3D && !supported3D && _unit3DReports.insert("state:"+std::to_string(status)+":fire="+std::to_string(bu->getFire()>0)).second)
        {
            Log(LOG_INFO)<<"[AQUANAUTE3D][FALLBACK] reason=unsupported-state status="<<status<<" selected=EXISTING_PNG_CHAIN";
        }
        unitSprite.setLayerRenderer([&](const UnitSprite::RenderLayer &layer,
            const std::function<void()> &legacyDraw) {
            const unsigned layerOrder = nextDrawSequence();
            std::string asset;
            int nativeScale = 16;
            // Resolve each body piece independently. A sparse artistic mod must
            // fall back to the complete PNG base, not to a PCK raster draw.
            if (layer.body && layer.frame >= 0 && layer.burn == 0)
            {
                const HdPngAsset resolved = hdResolveUnitPartPng(
                    [&](const std::string &path) { return _hdImageCache.usable(path); },
                    partsArmor->getHdUnitPartsRoot(), partsArmor->getHdUnitPartsScale(),
                    layer.dataset, layer.frame);
                asset = resolved.path;
                nativeScale = resolved.nativeScale;
            }
            if (!asset.empty() && _hdImageCache.usable(asset))
            {
                queueHdAsset(asset, nativeScale, 0, 0,
                    layer.x, layer.y, layer.shade, false, layerOrder, &layer.mask);
                markLastUnitGrade();
                return;
            }
            if (!layer.body && layer.frame >= 0)
            {
                const size_t firstCommand = _hdDrawCommands.size();
                const std::string key = "unit:" + std::to_string(bu->getId()) + ":" + layer.dataset;
                if (queueHdSurfaceSetOrConvention(layer.dataset, layer.frame, key,
                    layer.x, layer.y, layer.shade, false, layerOrder, layer.hasMask ? &layer.mask : nullptr))
                {
                    applyRealHdLightingTintToCommands(firstCommand, unitTile);
                    for (size_t i = firstCommand; i < _hdDrawCommands.size(); ++i)
                    {
                        _hdDrawCommands[i].legacyBaseColor = layer.baseColor;
                        _hdDrawCommands[i].unit3DKeepLayer = true;
                    }
                    return;
                }
            }
			if (_hdProducingDynamic && layer.source)
			{
				queueLegacyIndexedAsset(SurfaceRaw<const Uint8>(layer.source),
					layer.x, layer.y, layer.shade, false, layerOrder,
					layer.baseColor, layer.hasMask ? &layer.mask : nullptr);
				markLastUnitGrade();
				unit3DSafe = false;
				return;
			}
			unit3DSafe = false; // Native CPU pixels cannot be removed transactionally.
            // This migration patch does not claim sovereignty. Unsupported
            // burn/scripts/missing assets remain visible in a per-layer report.
            const std::string fallbackKey = layer.dataset + ":" + std::to_string(layer.frame)
                + ":burn=" + std::to_string(layer.burn);
            if (_hdPngLayerFallbacks.insert(fallbackKey).second)
                Log(LOG_WARNING) << "[PNG TRANSITION P1][LEGACY UNIT FALLBACK] unit=" << bu->getId()
                    << " layer=" << fallbackKey << " strictHDContract=NOT_SATISFIED";
            const std::vector<Uint8> beforeLayer = snapshotArea(layer.mask);
            hdTraceRoute("unit", std::to_string(bu->getId()) + ":" + fallbackKey,
                "LEGACY_NATIVE", "HD piece missing or native burn/script still required", true);
            legacyDraw();
            markChangedDrawOrder(beforeLayer, layer.mask, layerOrder);
        });
        unitSprite.draw(bu, part, tileScreenPosition.x + offsets.ScreenOffset.x,
            tileScreenPosition.y + offsets.ScreenOffset.y, shade, mask, _isAltPressed && !_isCtrlPressed);
        unitSprite.setLayerRenderer(UnitSprite::LayerRenderer());
        if(wants3D && supported3D && unit3DSafe && firstUnitCommand<_hdDrawCommands.size())
        {
            const Uint32 now=SDL_GetTicks();
            auto found=_unit3DStates.emplace(bu->getId(),Unit3DAnimationState{bu->isKneeled(),now-10000u});
            auto &state=found.first->second;
            if(state.kneeling!=bu->isKneeled()) { state.kneeling=bu->isKneeled(); state.changed=now; }
            auto &command=_hdDrawCommands[firstUnitCommand];
            command.unit3DModel=partsArmor->getHdUnit3DModel();
            command.unit3DCommandCount=_hdDrawCommands.size()-firstUnitCommand;
            command.unit3DX=tileScreenPosition.x+offsets.ScreenOffset.x;
            command.unit3DY=tileScreenPosition.y+offsets.ScreenOffset.y;
            command.unit3DMask=mask;
            auto &pose=command.unit3DPose;
            pose.walking=status==STATUS_WALKING;
            pose.kneeling=bu->isKneeled();
            pose.walkPhase=float(offsets.NormalizedMovePhase)/16.f;
            pose.direction=bu->getDirection();
            pose.seconds=float(now%600000u)/1000.f;
            pose.transitionSeconds=float(Uint32(now-state.changed))/1000.f;
            pose.height=float(partsArmor->getHdUnit3DHeight());
        }
        else if(wants3D && supported3D && _unit3DReports.insert("native-unit-layer").second)
        {
            Log(LOG_WARNING)<<"[AQUANAUTE3D][FALLBACK] reason=native-or-empty-unit-layer selected=EXISTING_PNG_CHAIN";
        }
        return;
    }
	if (usesFullBodySprite(bu))
	{
		const Armor *armor = bu->getArmor();
		std::string combined = findHdUnitCombinedAsset(bu);
		std::string body = combined.empty() ? findHdUnitBodyAsset(bu) : combined;
		if (body.empty() && !armor->getFullBodySprite().empty() && _hdImageCache.usable(armor->getFullBodySprite()))
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
			markLastUnitGrade();

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
				if (!armL.empty())
				{
					queueHdAsset(armL, armor->getFullBodySpriteScale(), armor->getFullBodySpriteOffsetX(), armor->getFullBodySpriteOffsetY(), drawX, drawY, shade, false, order, &mask);
					markLastUnitGrade();
				}
				if (!armR.empty())
				{
					queueHdAsset(armR, armor->getFullBodySpriteScale(), armor->getFullBodySpriteOffsetX(), armor->getFullBodySpriteOffsetY(), drawX, drawY, shade, false, order, &mask);
					markLastUnitGrade();
				}
				if (!weaponL.empty())
				{
					queueHdAsset(weaponL, armor->getFullBodySpriteScale(), armor->getFullBodySpriteOffsetX(), armor->getFullBodySpriteOffsetY(), drawX, drawY, shade, false, order, &mask);
					markLastUnitGrade();
				}
				if (!weaponR.empty())
				{
					queueHdAsset(weaponR, armor->getFullBodySpriteScale(), armor->getFullBodySpriteOffsetX(), armor->getFullBodySpriteOffsetY(), drawX, drawY, shade, false, order, &mask);
					markLastUnitGrade();
				}
			}
			return;
		}
	}

	const unsigned order = nextDrawSequence();
	if (_hdProducingDynamic)
	{
		unitSprite.setLayerRenderer([&](const UnitSprite::RenderLayer &layer,
			const std::function<void()> &) {
			const size_t begin = _hdDrawCommands.size();
			const std::string key = "unit:" + std::to_string(bu->getId()) + ":" + layer.dataset;
			if (!queueHdSurfaceSetOrConvention(layer.dataset, layer.frame, key,
				layer.x, layer.y, layer.shade, false, order,
				layer.hasMask ? &layer.mask : nullptr) && layer.source)
				queueLegacyIndexedAsset(SurfaceRaw<const Uint8>(layer.source),
					layer.x, layer.y, layer.shade, false, order,
					layer.baseColor, layer.hasMask ? &layer.mask : nullptr);
			applyRealHdLightingTintToCommands(begin, unitTile);
			for (size_t i = begin; i < _hdDrawCommands.size(); ++i)
				_hdDrawCommands[i].materialProfile = HdMaterialProfile::Unit;
		});
		unitSprite.draw(bu, part, tileScreenPosition.x + offsets.ScreenOffset.x,
			tileScreenPosition.y + offsets.ScreenOffset.y, shade, mask,
			_isAltPressed && !_isCtrlPressed);
		unitSprite.setLayerRenderer(UnitSprite::LayerRenderer());
		return;
	}
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
				// HD PROJECTILE CAMERA SMOOTH FOLLOW V1. Legacy either waits for the projectile
				// to leave the viewport and jumps by a complete screen, or (old smooth mode) snaps
				// the projectile straight to center. In HD, engage before the edge and ease the
				// camera toward the projectile in bounded pixel steps. Projectile speed/trajectory
				// are untouched; this is presentation-only camera motion.
				if (_launch)
				{
					_launch = false;
					if ((bulletPositionScreen.x < 1 || bulletPositionScreen.x > surface->getWidth() - 1 ||
						bulletPositionScreen.y < 1 || bulletPositionScreen.y > _visibleMapHeight - 1))
					{
						// Only the exceptional case where the shot is already off-screen at creation may
						// recenter immediately. Normal visible shots never use this jump.
						_camera->centerOnPosition(Position(bulletLowX, bulletLowY, bulletHighZ), false);
						_camera->convertVoxelToScreen(_projectile->getPosition(), &bulletPositionScreen);
					}
				}

				const int safeMarginX = std::max(_spriteWidth * 2, surface->getWidth() / 5);
				const int safeMarginY = std::max(_spriteHeight * 2, _visibleMapHeight / 5);
				if (!_smoothingEngaged &&
					(bulletPositionScreen.x < safeMarginX || bulletPositionScreen.x > surface->getWidth() - safeMarginX ||
					 bulletPositionScreen.y < safeMarginY || bulletPositionScreen.y > _visibleMapHeight - safeMarginY))
				{
					_smoothingEngaged = true;
				}

				if (_smoothingEngaged)
				{
					const int errorX = surface->getWidth() / 2 - bulletPositionScreen.x;
					const int errorY = _visibleMapHeight / 2 - bulletPositionScreen.y;
					const int maxStepX = std::max(2, surface->getWidth() / 18);
					const int maxStepY = std::max(2, _visibleMapHeight / 18);
					auto easedStep = [](int error, int maxStep) -> int
					{
						if (std::abs(error) <= 2) return 0;
						int step = (int)std::lround((double)error * 0.28);
						if (step == 0) step = error > 0 ? 1 : -1;
						return std::max(-maxStep, std::min(maxStep, step));
					};
					const int stepX = easedStep(errorX, maxStepX);
					const int stepY = easedStep(errorY, maxStepY);
					if (stepX || stepY)
					{
						_camera->jumpXY(stepX, stepY);
						_camera->convertVoxelToScreen(_projectile->getPosition(), &bulletPositionScreen);
					}
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
	const BedrockMaterial bedrockMaterial = BedrockRenderPolicy::resolve(_save);
	// BEDROCK ownership is semantic, not conditional on whether the GPU asset happened to load.
	// When the map declares BEDROCK_SAND, individual SAND sprites have no graphical authority.
	const bool bedrockGraphicsActive = bedrockMaterial != BedrockMaterial::None;
	Position surfaceCursorTile(-1, -1, -1);
	if (Options::hdGraphics && bedrockGraphicsActive && _cursorType == CT_NORMAL &&
		_cursorSize == 1 && !_save->getBattleState()->getMouseOverIcons())
	{
		for (int z = _camera->getViewLevel(); z >= 0; --z)
		{
			Tile *candidate = _save->getTile(Position(_selectorX, _selectorY, z));
			if (!candidate || !candidate->isDiscovered(O_FLOOR) ||
				!BedrockRenderPolicy::hasSurface(bedrockMaterial, candidate)) continue;
			if (z < _camera->getViewLevel() &&
				!hdCanPresentLowerCursor(_save, _selectorX, _selectorY,
					_camera->getViewLevel(), z)) continue;
			surfaceCursorTile = Position(_selectorX, _selectorY, z);
			break;
		}
	}
	const bool surfaceCursorActive = surfaceCursorTile.z >= 0;
	auto bedrockOwns = [&](Tile *t, TilePart part) -> bool
	{
		return bedrockGraphicsActive && BedrockRenderPolicy::ownsTilePart(bedrockMaterial, t, part);
	};
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

					// BEDROCK transfers graphical ownership of its source dataset away
					// from individual tile sprites. OXCE still owns all tile/MCD logic.
					if (!bedrockOwns(tile, O_FLOOR))
					{
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
								if (!queueLegacyIfGpuReady(tmpSurface, hx, hy, floorShade, false, hdOrder, _nvColor, nullptr, tile, O_FLOOR))
								{
									Surface::blitRaw(surface, tmpSurface, hx, hy, floorShade, false, _nvColor);
									markSourceDrawOrder(tmpSurface, hx, hy, false, hdOrder);
									++getHdPerfStats().current.mapCpuLegacyBlits;
								}
							}
						}
						}
					}

					auto* unit = tile->getUnit();
					if (surfaceCursorActive && mapPosition == surfaceCursorTile)
					{
						HdDrawCommand cursor;
						cursor.surfaceCursor = true;
						cursor.cursorTile = surfaceCursorTile;
						cursor.cursorYellow = unit && (unit->getVisible() || _save->getDebugMode());
						cursor.drawOrder = nextDrawSequence();
						_hdDrawCommands.push_back(std::move(cursor));
					}

					// Draw cursor back. TAKEOVER V3 treats CURSOR.PCK as a tactical UI overlay:
					// it is replayed after the black visibility mask so targeting remains usable
					// without allowing hidden world geometry to pass through.
					const bool hdTakeoverHiddenTile =
						HdGpuBackend::instance().directWorldReady() &&
						!tile->isDiscovered(O_FLOOR);
					if (!surfaceCursorActive && _cursorType != CT_NONE && _selectorX > itX - _cursorSize && _selectorY > itY - _cursorSize && _selectorX < itX+1 && _selectorY < itY+1 && !_save->getBattleState()->getMouseOverIcons())
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
							drawHdSurfaceSetOrLegacy(surface, "CURSOR.PCK", frameNumber, tmpSurface, screenPosition.x, screenPosition.y, 0, false, 0, true);
						}
						else if (_camera->getViewLevel() > itZ)
						{
							// OXCE CURSOR PRESENTATION GATE V4:
							// One shared authority owns both halves of the translucent lower cursor.
							// It follows Real HD discovery/presentation, not coarse visibleTiles.
							if (hdCanPresentLowerCursor(_save, itX, itY, _camera->getViewLevel(), itZ))
							{
								frameNumber = 2; // translucent lower-level box
								tmpSurface = _game->getMod()->getSurfaceSet("CURSOR.PCK")->getFrame(frameNumber);
								drawHdSurfaceSetOrLegacy(surface, "CURSOR.PCK", frameNumber, tmpSurface, screenPosition.x, screenPosition.y, 0, false, 0, true);
							}
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
						if (!bedrockOwns(tile, O_WESTWALL) && (tmpSurface || findHdTerrainVisualRule(tile, O_WESTWALL)))
						{
							const int wallShade = tile->getObstacle(O_WESTWALL) ? obstacleShade : getWallShade(O_WESTWALL, tile);
							const int hx = screenPosition.x;
							const int hy = screenPosition.y - tile->getYOffset(O_WESTWALL) * mapGraphicsScale;
							const unsigned hdOrder = nextDrawSequence();
							if (!queueHdTerrainOrConvention(tile, O_WESTWALL, hx, hy, wallShade, false, hdOrder))
							{
								if (tmpSurface)
								{
									if (!queueLegacyIfGpuReady(tmpSurface, hx, hy, wallShade, false, hdOrder, _nvColor, nullptr, tile, O_WESTWALL))
									{
										Surface::blitRaw(surface, tmpSurface, hx, hy, wallShade, false, _nvColor);
										markSourceDrawOrder(tmpSurface, hx, hy, false, hdOrder);
										++getHdPerfStats().current.mapCpuLegacyBlits;
									}
								}
							}
						}
						// Draw north wall
						tmpSurface = tile->getSprite(O_NORTHWALL);
						if (!bedrockOwns(tile, O_NORTHWALL) && (tmpSurface || findHdTerrainVisualRule(tile, O_NORTHWALL)))
						{
							const bool halfWall = !bedrockOwns(tile, O_WESTWALL) && bool(tile->getSprite(O_WESTWALL));
							const int wallShade = tile->getObstacle(O_NORTHWALL) ? obstacleShade : getWallShade(O_NORTHWALL, tile);
							const int hx = screenPosition.x;
							const int hy = screenPosition.y - tile->getYOffset(O_NORTHWALL) * mapGraphicsScale;
							const unsigned hdOrder = nextDrawSequence();
							if (!queueHdTerrainOrConvention(tile, O_NORTHWALL, hx, hy, wallShade, halfWall, hdOrder))
							{
								if (tmpSurface)
								{
									if (!queueLegacyIfGpuReady(tmpSurface, hx, hy, wallShade, halfWall, hdOrder, _nvColor, nullptr, tile, O_NORTHWALL))
									{
										Surface::blitRaw(surface, tmpSurface, hx, hy, wallShade, halfWall, _nvColor);
										markSourceDrawOrder(tmpSurface, hx, hy, halfWall, hdOrder);
										++getHdPerfStats().current.mapCpuLegacyBlits;
									}
								}
							}
						}
						// Draw object behind the unit when the MCD requests it.
						tmpSurface = tile->getSprite(O_OBJECT);
						if (!bedrockOwns(tile, O_OBJECT) && (tmpSurface || findHdTerrainVisualRule(tile, O_OBJECT)) && tile->isBackTileObject(O_OBJECT))
						{
							const int objectShade = tile->getObstacle(O_OBJECT) ? obstacleShade : tileShade;
							const int hx = screenPosition.x;
							const int hy = screenPosition.y - tile->getYOffset(O_OBJECT) * mapGraphicsScale;
							const unsigned hdOrder = nextDrawSequence();
							if (!queueHdTerrainOrConvention(tile, O_OBJECT, hx, hy, objectShade, false, hdOrder))
							{
								if (tmpSurface)
								{
									if (!queueLegacyIfGpuReady(tmpSurface, hx, hy, objectShade, false, hdOrder, _nvColor, nullptr, tile, O_OBJECT))
									{
										Surface::blitRaw(surface, tmpSurface, hx, hy, objectShade, false, _nvColor);
										markSourceDrawOrder(tmpSurface, hx, hy, false, hdOrder);
										++getHdPerfStats().current.mapCpuLegacyBlits;
									}
								}
							}
						}
						// draw an item/corpse on top of the floor (if any). FLOOROB.PCK
						// frames can also be replaced by native-resolution RGBA files (x4/x8/x16).
						BattleItem* item = tile->getTopItem();
						if (item && !hdTakeoverHiddenTile)
						{
							GraphSubset itemTrackArea(_spriteWidth * 2, _spriteHeight * 2);
							itemTrackArea = itemTrackArea.offset(screenPosition.x - _spriteWidth / 2, screenPosition.y - _spriteHeight / 2);
							const int itemFrame = item->getFloorSpriteIndex(_save, _animFrame, tileShade);
							const int itemX = screenPosition.x;
							const int itemY = screenPosition.y + tile->getTerrainLevel() * mapGraphicsScale;
							const unsigned itemOrder = nextDrawSequence();
							const size_t itemTintBegin = _hdDrawCommands.size();
							std::ostringstream itemKey;
							itemKey << "groundItem:" << item->getId();
							if (!queueHdSurfaceSetOrConvention("FLOOROB.PCK", itemFrame, itemKey.str(), itemX, itemY, tileShade, false, itemOrder))
							{
								auto before = snapshotArea(itemTrackArea);
								itemSprite.draw(item, itemX, itemY, tileShade);
								markChangedDrawOrder(before, itemTrackArea, itemOrder);
							}
							else
							{
								applyRealHdLightingTintToCommands(itemTintBegin, tile);
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
									const size_t itemTintBegin = _hdDrawCommands.size();
									std::ostringstream itemKey;
									itemKey << "thrownItem:" << item->getId();
									if (!queueHdSurfaceSetOrConvention("FLOOROB.PCK", itemFrame, itemKey.str(), ix, iy, tileShade, false, itemOrder))
									{
										GraphSubset area(_spriteWidth * 2, _spriteHeight * 2); area = area.offset(ix - _spriteWidth / 2, iy - _spriteHeight / 2);
										auto before = snapshotArea(area);
										itemSprite.draw(item, ix, iy, tileShade);
										markChangedDrawOrder(before, area, itemOrder);
									}
									else
									{
										applyRealHdLightingTintToCommands(itemTintBegin, tile);
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

					// Draw smoke/fire. Pure smoke may be transferred to the physical HD pseudo-volume
					// pass, but fire keeps the Legacy animation path. If D3D11 is unavailable, Legacy
					// remains the automatic fallback.
					const bool hdSmokeOwns = Options::hdSmokeVolumeEnabled && Options::hdSmokeReplaceLegacy && HdGpuBackend::instance().directWorldReady() && tile->getSmoke() && !tile->getFire();
					if (tile->getSmoke() && tile->isDiscovered(O_FLOOR) && !hdSmokeOwns)
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
						if (!bedrockOwns(tile, O_OBJECT) && (tmpSurface || findHdTerrainVisualRule(tile, O_OBJECT)) && !tile->isBackTileObject(O_OBJECT))
						{
							const int objectShade = tile->getObstacle(O_OBJECT) ? obstacleShade : tileShade;
							const int hx = screenPosition.x;
							const int hy = screenPosition.y - tile->getYOffset(O_OBJECT) * mapGraphicsScale;
							const unsigned hdOrder = nextDrawSequence();
							if (!queueHdTerrainOrConvention(tile, O_OBJECT, hx, hy, objectShade, false, hdOrder))
							{
								if (tmpSurface)
								{
									if (!queueLegacyIfGpuReady(tmpSurface, hx, hy, objectShade, false, hdOrder, _nvColor, nullptr, tile, O_OBJECT))
									{
										Surface::blitRaw(surface, tmpSurface, hx, hy, objectShade, false, _nvColor);
										markSourceDrawOrder(tmpSurface, hx, hy, false, hdOrder);
										++getHdPerfStats().current.mapCpuLegacyBlits;
									}
								}
							}
						}
					}
					// Draw cursor front. Like the back half, TAKEOVER V3 replays CURSOR.PCK
					// after the visibility mask; the cursor may cross black space, but world
					// geometry underneath remains independently culled by OXCE discovery.
					if (!surfaceCursorActive && _cursorType != CT_NONE && _selectorX > itX - _cursorSize && _selectorY > itY - _cursorSize && _selectorX < itX+1 && _selectorY < itY+1 && !_save->getBattleState()->getMouseOverIcons())
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
							drawHdSurfaceSetOrLegacy(surface, "CURSOR.PCK", frameNumber, tmpSurface, screenPosition.x, screenPosition.y, 0, false, 0, true);

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
							// Same V4 presentation gate as frame 2.  Keeping both cursor halves behind
							// one helper prevents one half from leaking geometry independently.
							if (hdCanPresentLowerCursor(_save, itX, itY, _camera->getViewLevel(), itZ))
							{
								frameNumber = 5; // translucent lower-level box
								tmpSurface = _game->getMod()->getSurfaceSet("CURSOR.PCK")->getFrame(frameNumber);
								drawHdSurfaceSetOrLegacy(surface, "CURSOR.PCK", frameNumber, tmpSurface, screenPosition.x, screenPosition.y, 0, false, 0, true);
							}
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
								drawHdSurfaceSetOrLegacy(surface, "CURSOR.PCK", cursorFrame, tmpSurface, screenPosition.x, screenPosition.y, 0, false, 0, true);
							}
						}
					}

					// Draw waypoints if any on this tile
					int waypid = 1;
					int waypXOff = 2 * mapGraphicsScale;
					int waypYOff = 2 * mapGraphicsScale;

					for (const auto& waypoint : _waypoints)
					{
						if (waypoint == mapPosition && !hdTakeoverHiddenTile)
						{
							if (waypXOff == 2 * mapGraphicsScale && waypYOff == 2 * mapGraphicsScale)
							{
								tmpSurface = _game->getMod()->getSurfaceSet("CURSOR.PCK")->getFrame(7);
								drawHdSurfaceSetOrLegacy(surface, "CURSOR.PCK", 7, tmpSurface, screenPosition.x, screenPosition.y, 0, false, 0, true);
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
					trackedBlitNShade(_arrow,
						surface,
						screenPosition.x + offset.x + (_spriteWidth / 2) - (_arrow->getWidth() / 2),
						screenPosition.y + offset.y - _arrow->getHeight() + getArrowBobForFrame(_animFrame) * mapGraphicsScale,
						0);
				}
				else if (customMarker)
				{
					trackedBlitNShade(_arrow,
						surface,
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
				trackedBlitNShade(_arrow,
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
 * REAL HD LIGHTING V2.
 *
 * OXCE supplies gameplay light facts. REAL HD owns physical sight and lighting
 * geometry in the active HD mission. Presentation deliberately
 * ignores LL_UNITS: the historical personal-light layer is a radial 360-degree
 * field and is not the physical light emitted by an Aquanaut helmet.  Instead
 * we build a directional, LOS-blocked field from player units and keep it
 * strictly presentation-only.
 */
void Map::rebuildRealHdHelmetLightField()
{
	if (_realHdHelmetLightFieldRevision == _realHdLightingRevision) return;
	_realHdHelmetLightFieldRevision = _realHdLightingRevision;
	auto &perf = getHdPerfStats().current;
	const uint64_t perfHelmetStart = hdPerfNowUs();
	++perf.mapHelmetLightRebuilds;
	auto finishHelmetPerf = [&]()
	{
		perf.mapHelmetLightBuildUs += hdPerfNowUs() - perfHelmetStart;
	};
	_realHdHelmetLightField.clear();
	_realHdHelmetLightFieldR.clear();
	_realHdHelmetLightFieldG.clear();
	_realHdHelmetLightFieldB.clear();
	if (!Options::hdGraphics || !_save || !_save->getTogglePersonalLight())
	{
		finishHelmetPerf();
		return;
	}

	const int mapW = _save->getMapSizeX();
	const int mapH = _save->getMapSizeY();
	const int mapZ = _save->getMapSizeZ();
	if (mapW <= 0 || mapH <= 0 || mapZ <= 0)
	{
		finishHelmetPerf();
		return;
	}
	const size_t fieldSize = (size_t)mapW * (size_t)mapH * (size_t)mapZ;
	perf.mapHelmetLightFieldCells += fieldSize;
	_realHdHelmetLightField.assign(fieldSize, 0.0f);
	_realHdHelmetLightFieldR.assign(fieldSize, 0.0f);
	_realHdHelmetLightFieldG.assign(fieldSize, 0.0f);
	_realHdHelmetLightFieldB.assign(fieldSize, 0.0f);

	TileEngine *tileEngine = _save->getTileEngine();
	if (!tileEngine)
	{
		finishHelmetPerf();
		return;
	}
	const RealHdLightingAuthority::HelmetLightTuning tuning = RealHdLightingAuthority::helmetLightTuning();

	auto fieldIndex = [=](int x, int y, int z) -> size_t
	{
		return ((size_t)z * (size_t)mapH + (size_t)y) * (size_t)mapW + (size_t)x;
	};

	for (BattleUnit *unit : *_save->getUnits())
	{
		if (!unit || unit->isOut() || unit->getFaction() != FACTION_PLAYER || !unit->getArmor()) continue;
		const int authoredPersonalLight = std::max(0, unit->getArmor()->getPersonalLightFriend());
		if (authoredPersonalLight <= 0) continue;
		const float equipmentScale = std::max(0.15f, std::min(2.0f, authoredPersonalLight / 15.0f));
		const float range = tuning.rangeTiles * equipmentScale;
		if (range <= 0.01f || tuning.intensity <= 0.0f) continue;

		const Position source = unit->getPosition();
		const int r = (int)std::ceil(range);
		for (int z = std::max(0, source.z - 1); z <= std::min(mapZ - 1, source.z + 1); ++z)
		for (int y = std::max(0, source.y - r); y <= std::min(mapH - 1, source.y + r); ++y)
		for (int x = std::max(0, source.x - r); x <= std::min(mapW - 1, source.x + r); ++x)
		{
			const Position target(x, y, z);
			++perf.mapHelmetLightCandidateSamples;
			const RealHdLightingAuthority::HelmetLightSample sample =
				RealHdLightingAuthority::helmetLightSampleAt(_save, tileEngine, unit, target);
			if (sample.intensity <= 0.001f) continue;
			const size_t fi = fieldIndex(x, y, z);
			// Presentation envelope: overlapping helmets extend coverage, not exposure.
			// Keep scalar and RGB bounded by individual samples; perception stays independent.
			_realHdHelmetLightField[fi] = std::max(_realHdHelmetLightField[fi], sample.intensity);
			_realHdHelmetLightFieldR[fi] = std::max(_realHdHelmetLightFieldR[fi], sample.r);
			_realHdHelmetLightFieldG[fi] = std::max(_realHdHelmetLightFieldG[fi], sample.g);
			_realHdHelmetLightFieldB[fi] = std::max(_realHdHelmetLightFieldB[fi], sample.b);
		}
	}

	finishHelmetPerf();

	static bool logged = false;
	if (!logged)
	{
		Log(LOG_INFO) << "[REAL HD HELMET LIGHT ARMOR V2][ACTIVE] base=OXCE_SEMANTIC_LIGHT_WITHOUT_LL_UNITS"
			<< " helmet=DIRECTIONAL_LOS_BLOCKED_SHARED_AUTHORITY overlapPresentation=MAX_ENVELOPE_P2ZH bodyDirection=YES headBone=NEXT_STAGE"
			<< " baseCoreHalfDeg=" << tuning.coreHalfDeg << " baseOuterHalfDeg=" << tuning.outerHalfDeg
			<< " armorCore=FOV_DERIVED magneticLateralTint=YES"
			<< " rangeTiles=" << tuning.rangeTiles << " intensity=" << tuning.intensity
			<< " migratedV2Defaults=" << (tuning.migratedV2Defaults ? 1 : 0)
			<< " angularFalloff=SMOOTHSTEP_SQUARED radialFalloff=POW_0_75"
			<< " perceptionAuthority=SHARED detectionLight=AMBIENT_FIRE_ITEMS_PLUS_HELMET"
			<< " legacyLLUnitsDetection=DISABLED_FOR_REAL_HD_PLAYER_ONLY"
			<< " lutCoupling=NONE rasterLegacyAuthority=NONE";
		logged = true;
	}
}

float Map::realHdHelmetLightAt(const Position &position)
{
	rebuildRealHdHelmetLightField();
	if (!_save || _realHdHelmetLightField.empty()) return 0.0f;
	const int mapW = _save->getMapSizeX(), mapH = _save->getMapSizeY(), mapZ = _save->getMapSizeZ();
	if (position.x < 0 || position.y < 0 || position.z < 0 || position.x >= mapW || position.y >= mapH || position.z >= mapZ) return 0.0f;
	const size_t index = ((size_t)position.z * (size_t)mapH + (size_t)position.y) * (size_t)mapW + (size_t)position.x;
	return index < _realHdHelmetLightField.size() ? _realHdHelmetLightField[index] : 0.0f;
}

void Map::realHdHelmetLightColorAt(const Position &position, float &r, float &g, float &b)
{
	rebuildRealHdHelmetLightField();
	r = g = b = 0.0f;
	if (!_save || _realHdHelmetLightField.empty()) return;
	const int mapW = _save->getMapSizeX(), mapH = _save->getMapSizeY(), mapZ = _save->getMapSizeZ();
	if (position.x < 0 || position.y < 0 || position.z < 0 || position.x >= mapW || position.y >= mapH || position.z >= mapZ) return;
	const size_t index = ((size_t)position.z * (size_t)mapH + (size_t)position.y) * (size_t)mapW + (size_t)position.x;
	if (index >= _realHdHelmetLightField.size()) return;
	r = _realHdHelmetLightFieldR[index];
	g = _realHdHelmetLightFieldG[index];
	b = _realHdHelmetLightFieldB[index];
}

float Map::realHdLightingAt(Tile *tile)
{
	if (!tile || !_save) return 0.0f;

	// Global mission darkness remains the common floor for every REAL HD world
	// provider. Dynamic OXCE light layers are semantic inputs, but LL_UNITS is
	// intentionally excluded because the helmet field above replaces it visually.
	const float ambientScale = std::max(0.0f, Options::hdAmbientLightPermille / 1000.0f);
	const float missionAmbient = std::max(0.0f, std::min(1.0f,
		std::max(0.10f, std::min(1.0f, (16.0f - (float)_save->getGlobalShade()) / 16.0f)) * ambientScale));
	const int semanticLightLevel = std::max(tile->getLight(LL_AMBIENT),
		std::max(tile->getLight(LL_FIRE), tile->getLight(LL_ITEMS)));
	const float semanticLight = std::max(0.0f, std::min(1.0f, semanticLightLevel / 15.0f));
	const float depthLuma = Options::hdDepthLuminance ? std::max(0.0f, hdDepthLuminancePermilleForDepth(_save->getDepth()) / 1000.0f) : 1.0f;
	const float base = std::max(missionAmbient, semanticLight) * depthLuma;
	return std::max(0.0f, std::min(1.0f, base + realHdHelmetLightAt(tile->getPosition())));
}

void Map::realHdLightingTintAt(const Tile *tile, float &r, float &g, float &b)
{
	r = g = b = 1.0f;
	if (!tile || !_save || !Options::hdGraphics) return;

	const float ambientScale = std::max(0.0f, Options::hdAmbientLightPermille / 1000.0f);
	const float missionAmbient = std::max(0.0f, std::min(1.0f,
		std::max(0.10f, std::min(1.0f, (16.0f - (float)_save->getGlobalShade()) / 16.0f)) * ambientScale));
	const int semanticLightLevel = std::max(tile->getLight(LL_AMBIENT),
		std::max(tile->getLight(LL_FIRE), tile->getLight(LL_ITEMS)));
	const float semanticLight = std::max(0.0f, std::min(1.0f, semanticLightLevel / 15.0f));
	const float depthLuma = Options::hdDepthLuminance ? std::max(0.0f, hdDepthLuminancePermilleForDepth(_save->getDepth()) / 1000.0f) : 1.0f;
	const float base = std::max(missionAmbient, semanticLight) * depthLuma;
	const float helmet = realHdHelmetLightAt(tile->getPosition());
	if (helmet <= 0.001f) return;

	float hr = 0.0f, hg = 0.0f, hb = 0.0f;
	realHdHelmetLightColorAt(tile->getPosition(), hr, hg, hb);
	const float denom = std::max(0.001f, base + helmet);
	r = std::max(0.75f, std::min(1.25f, (base + hr) / denom));
	g = std::max(0.75f, std::min(1.25f, (base + hg) / denom));
	b = std::max(0.75f, std::min(1.25f, (base + hb) / denom));
}

void Map::applyRealHdLightingTintToCommands(size_t begin, const Tile *tile)
{
	if (!Options::hdGraphics || !tile || begin >= _hdDrawCommands.size()) return;
	float r = 1.0f, g = 1.0f, b = 1.0f;
	realHdLightingTintAt(tile, r, g, b);
	for (size_t i = begin; i < _hdDrawCommands.size(); ++i)
	{
		_hdDrawCommands[i].lightTintR = r;
		_hdDrawCommands[i].lightTintG = g;
		_hdDrawCommands[i].lightTintB = b;
	}
}

int Map::realHdShade(Tile *tile)
{
	const float light = realHdLightingAt(tile);
	return std::max(0, std::min(16, (int)std::lround((1.0f - light) * 16.0f)));
}

/**
 * Handles fade-in and fade-out shade modification
 * @param original tile/item/unit shade
 */

int Map::reShade(Tile *tile)
{
	if (!tile) return 16;
	const int baseShade = Options::hdGraphics ? realHdShade(tile) : tile->getShade();

	// Preserve OXCE debug/night-vision presentation behaviour, but apply it to
	// the REAL HD light value rather than reintroducing Tile::getShade()/LL_UNITS.
	if (_debugVisionMode > 0)
	{
		if (_debugVisionMode == 1) return baseShade / 2;
		return 0;
	}
	if (_nvColor == 0) return baseShade;
	if (baseShade <= NIGHT_VISION_SHADE) return baseShade;

	for (const auto* bu : *_save->getUnits())
	{
		if (bu->getFaction() == FACTION_PLAYER && !bu->isOut() &&
			Position::distance2dSq(tile->getPosition(), bu->getPosition()) <= bu->getMaxViewDistanceAtDarkSquared())
		{
			return baseShade > _fadeShade ? _fadeShade : baseShade;
		}
	}
	return std::min(+NIGHT_VISION_MAX_SHADE, baseShade);
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

std::string Map::getHdFovAuditTrace() const
{
	if (!_save || !_camera || !Options::hdFovAuditProbeEnabled) return std::string();
	std::ostringstream out;
	const int mapZ = std::max(0, _save->getMapSizeZ());
	const int viewZ = std::max(0, std::min(mapZ - 1, _camera->getViewLevel()));
	const int x = _selectorX, y = _selectorY;
	BattleUnit *unit = _save->getSelectedUnit();
	out << "FOVPROBE xy=" << x << "," << y << " viewZ=" << viewZ;
	if (unit)
		out << " unit=" << unit->getPosition().x << "," << unit->getPosition().y << "," << unit->getPosition().z << " dir=" << unit->getDirection();
	else
		out << " unit=NONE";
	out << "  D=discovered V=visibleNow T=voxelTargetable R=realHdSurfaceLOS P=part O=open-column\n";

	Position origin;
	TileEngine *te = _save->getTileEngine();
	if (unit && te) origin = te->getSightOriginVoxel(unit);
	const TilePart parts[4] = {O_FLOOR, O_WESTWALL, O_NORTHWALL, O_OBJECT};
	const char *names[4] = {"F", "W", "N", "O"};
	for (int z = 0; z <= viewZ; ++z)
	{
		Tile *tile = _save->getTile(Position(x, y, z));
		if (!tile)
		{
			out << " z" << z << " <no tile>\n";
			continue;
		}
		out << " z" << z << " O" << (tile->hasNoFloor(_save) ? 1 : 0);
		const bool visibleNow = hdTileVisibleNowToPlayer(_save, tile);
		for (int i = 0; i < 4; ++i)
		{
			MapData *md = tile->getMapData(parts[i]);
			const bool bedrockFloorCandidate = parts[i] == O_FLOOR && BedrockRenderPolicy::resolve(_save) != BedrockMaterial::None && BedrockRenderPolicy::hasSurface(BedrockRenderPolicy::resolve(_save), tile);
			const bool present = md != nullptr || bedrockFloorCandidate;
			const bool discovered = tile->isDiscovered(parts[i]);
			bool targetable = false;
			if (present && unit && te)
			{
				Position hit;
				targetable = te->canTargetTile(&origin, tile, parts[i], &hit, unit, false);
			}
			const RealHdVisibilityAuditResult realHdLos = (unit && parts[i] == O_FLOOR)
				? RealHdVisibilitySolver::auditSurfaceLos(_save, unit, tile, parts[i])
				: RealHdVisibilityAuditResult();
			out << " " << names[i] << "[P" << (present ? 1 : 0) << "D" << (discovered ? 1 : 0) << "V" << (visibleNow ? 1 : 0) << "T" << (targetable ? 1 : 0);
			if (realHdLos.supported) out << "R" << (realHdLos.visible ? 1 : 0);
			else out << "R-";
			out << "]";
		}
		out << "\n";
	}

	const BedrockMaterial material = BedrockRenderPolicy::resolve(_save);
	if (material != BedrockMaterial::None)
	{
		for (int z = 0; z <= viewZ; ++z)
		{
			Tile *tile = _save->getTile(Position(x, y, z));
			if (!tile || !BedrockRenderPolicy::hasSurface(material, tile)) continue;
			const BedrockCellGeometry g = BedrockRenderPolicy::cellGeometry(material, tile);
			const int minH = std::min(std::min(g.top, g.right), std::min(g.bottom, g.left));
			const int maxH = std::max(std::max(g.top, g.right), std::max(g.bottom, g.left));
			out << " BR z" << z << " MCD=" << g.sourceMcdId
				<< " h(T/R/B/L)=" << g.top << "/" << g.right << "/" << g.bottom << "/" << g.left
				<< " span=" << minH << ".." << maxH
				<< " explicit=" << (g.explicitGeometry ? 1 : 0) << "\n";
		}
	}
	return out.str();
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
	if (projectile && _smoothCamera)
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
	if (hidden)
		_visibleMapHeight = getHeight();
	else if (_hudVisibleMapHeightOverride > 0)
		_visibleMapHeight = std::min(getHeight(), _hudVisibleMapHeightOverride);
	else
		_visibleMapHeight = getHeight() - _iconHeight;
	if (_camera) _camera->setVisibleMapHeight(_visibleMapHeight);
	if (_message)
	{
		_message->setHeight((_visibleMapHeight < 200) ? _visibleMapHeight : 200);
		_message->setY((_visibleMapHeight - _message->getHeight()) / 2);
	}
}

void Map::setHudVisibleMapHeightOverride(int height)
{
	_hudVisibleMapHeightOverride = height > 0 ? height : -1;
	_visibleMapHeight = _hudVisibleMapHeightOverride > 0
		? std::min(getHeight(), _hudVisibleMapHeightOverride)
		: getHeight() - _iconHeight;
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
	_visibleMapHeight = _hudVisibleMapHeightOverride > 0
		? std::min(height, _hudVisibleMapHeightOverride)
		: height - _iconHeight;
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
