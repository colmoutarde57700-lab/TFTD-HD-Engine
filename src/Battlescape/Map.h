#pragma once
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
#include "../Engine/InteractiveSurface.h"
#include "../Engine/Options.h"
#include "../Engine/Collections.h"
#include "../Engine/HdImage.h"
#include "RemasterWorldState.h"
#include "../Engine/HdColorTransform.h"
#include "../Engine/HdMaterialGrade.h"
#include "../Engine/HdGpuBackend.h"
#include "../Mod/MapData.h"
#include "Position.h"
#include "Particle.h"
#include <vector>
#include <array>
#include <map>
#include <set>
#include <cstdint>

namespace OpenXcom
{

class SavedBattleGame;
class Surface;
class SurfaceSet;
class BattleUnit;
class Projectile;
class Explosion;
class BattlescapeMessage;
class Camera;
class Timer;
class Screen;
class Text;
class Tile;
class UnitSprite;
struct HdVisualRule;

enum CursorType { CT_NONE, CT_NORMAL, CT_AIM, CT_PSI, CT_WAYPOINT, CT_THROW };
enum TilePart : int;

/**
 * Helper class that returns all important data about the unit movement
 */
struct UnitWalkingOffset
{
	Position ScreenOffset;
	int NormalizedMovePhase;
	int TerrainLevelOffset;
};

/**
 * Interactive map of the battlescape.
 */
class Map : public InteractiveSurface
{
public:
	enum HdRedrawReason
	{
		HDR_EXTERNAL = 0,
		HDR_CAMERA,
		HDR_SELECTOR,
		HDR_ANIMATION,
		HDR_STATE_QUEUE,
		HDR_ATTACK,
		HDR_RESIZE,
		HDR_COUNT
	};

private:
	static const int SCROLL_INTERVAL = 15;
	static const int FADE_INTERVAL = 23;
	static const int NIGHT_VISION_SHADE = 4;
	static const int NIGHT_VISION_MAX_SHADE = 8;
	static const int BULLET_SPRITES = 35;
	Timer *_scrollMouseTimer, *_scrollKeyTimer, *_obstacleTimer;
	Timer *_fadeTimer;
	int _fadeShade;
	bool _nightVisionOn;
	int _debugVisionMode;
	int _nvColor;
	Game *_game;
	SavedBattleGame *_save;
	bool _isTFTD;
	Surface *_arrow;
	Surface *_stunIndicator, *_woundIndicator, *_burnIndicator, *_shockIndicator;
	bool _anyIndicator, _isAltPressed, _isCtrlPressed;
	int _spriteWidth, _spriteHeight;
	int _selectorX, _selectorY;
	int _mouseX, _mouseY;
	CursorType _cursorType;
	int _cursorSize;
	int _cacheActiveWeaponUfopediaArticleUnlocked; // -1 = unknown, 0 = locked, 1 = unlocked
	bool _cacheIsCtrlPressed;
	Position _cacheCursorPosition;
	int _cacheHasLOS; // -1 = unknown, 0 = no LOS, 1 = has LOS
	int _animFrame;
	Projectile *_projectile;
	bool _followProjectile;
	bool _projectileInFOV;
	std::list<Explosion *> _explosions;
	std::vector<std::vector<Particle>> _vaporParticlesInit;
	std::vector<std::vector<Particle>> _vaporParticles;
	bool _explosionInFOV, _launch;
	BattlescapeMessage *_message;
	Camera *_camera;
	int _visibleMapHeight;
	// Optional world-logical viewport height ending at the independently presented HUD top.
	int _hudVisibleMapHeightOverride;
	std::vector<Position> _waypoints;
	bool _unitDying, _smoothCamera, _smoothingEngaged, _flashScreen;
	int _bgColor;
	bool _previewSettingArrows, _previewSettingTu, _previewSettingEnergy;
	Text *_txtAccuracy;
	SurfaceSet *_projectileSet;

	// Real HD world submission layer. OXCE supplies semantic state and 2D
	// compatibility sprites, while D3D11 owns physical composition. The Legacy
	// raster/order buffer is retained only for CPU fallback pixels that have not
	// yet migrated to a Real HD semantic/geometry provider.
    unsigned long long _hdRoofGeometryRevision = ~0ull;
    std::map<int,std::array<unsigned,16>> _hdRoofSunCache;
	struct HdDrawCommand
	{
        bool roofCaustic = false;
        std::array<unsigned,16> roofSunRows{};
		bool surfaceCursor = false;
		bool surfaceCursorFront = false;
		bool surfaceCursorGuide = false;
		bool unit3DKeepLayer = false;
		std::vector<Uint8> generatedRgba;
		Position cursorTile;
		bool cursorYellow = false;
        // Optional real-geometry replacement of this complete PNG unit group.
        std::string unit3DModel;
        HdUnit3DPose unit3DPose;
        size_t unit3DCommandCount = 0;
        int unit3DX = 0, unit3DY = 0;
        GraphSubset unit3DMask;
		std::string assetPath;
		// WORLD_GPU_V1: a Legacy indexed frame can use the exact same map command
		// stream as an authored HD asset. The source remains Legacy; only rasterization
		// moves from Surface::blitRaw to D3D11.
		bool legacyRaw = false;
		std::string legacyKey;
		const Uint8 *legacyIndices = nullptr;
		unsigned legacyWidth = 0;
		unsigned legacyHeight = 0;
		unsigned legacyPitch = 0;
		int legacyBaseColor = 0;
		int nativeScale = 0;
		std::string colorMode = "auto";
		HdMaterialProfile materialProfile = HdMaterialProfile::Auto;
		int offsetX = 0;
		int offsetY = 0;
		int x = 0;
		int y = 0;
		// Static REAL HD scene positions are projected from world space with a
		// zero camera offset. The view translation is applied at submission.
		bool worldAnchored = false;
		int worldZ = 0;
		int worldX = 0, worldY = 0;
		int worldLayer = 0;
		bool worldDynamic = false;
		int shade = 0;
		// Presentation-only colour response of the physical REAL HD helmet light.
		// 1,1,1 is neutral; it never changes gameplay detection.
		float lightTintR = 1.0f;
		float lightTintG = 1.0f;
		float lightTintB = 1.0f;
		bool rightHalfOnly = false;
		unsigned drawOrder = 0;
		GraphSubset clipMask;
		bool hasClipMask = false;
		// V3: replay player UI / independently-discovered boundary walls after
		// the final black visibility mask.
		bool postVisibility = false;
	};
    struct Unit3DAnimationState { bool kneeling; Uint32 changed; };
    std::map<int, Unit3DAnimationState> _unit3DStates;
    std::set<std::string> _unit3DReports;
    bool _unit3DGpuFailed = false;
	HdImageCache _hdImageCache;
	HdEnvironmentTransform _hdEnvironmentTransform;
	// BEDROCK impact fields. Blast RGB stays separate from weapon triplet RGB so the shader can
	// read authored weapon CORE/RIM/HALO masks directly instead of inferring them from one legacy mask.
	std::vector<unsigned char> _hdBedrockCraterField;
	unsigned _hdBedrockCraterFieldW;
	unsigned _hdBedrockCraterFieldH;
	unsigned long long _hdBedrockCraterFieldSourceRevision;
	unsigned long long _hdBedrockCraterFieldUploadRevision;
	std::vector<unsigned char> _hdBedrockWeaponField;
	unsigned _hdBedrockWeaponFieldW;
	unsigned _hdBedrockWeaponFieldH;
	unsigned long long _hdBedrockWeaponFieldUploadRevision;

	// RESOURCE_RESOLVER_CACHE_V1: resolution from OXCE semantic resources to
	// authored HD overrides is stable for the lifetime of a tactical Map.  The
	// old path rebuilt candidate strings, queried the VFS and rescanned every
	// HdVisual rule for every visible tile on every redraw.  Cache both positive
	// and negative answers; gameplay/animation still selects the frame first.
	mutable std::map<std::string, std::string> _hdTerrainAssetResolveCache;
	mutable std::map<std::string, const HdVisualRule*> _hdTerrainRuleResolveCache;
	mutable std::map<std::string, std::string> _hdSurfaceSetAssetResolveCache;
	mutable std::map<std::string, const HdVisualRule*> _hdSurfaceSetRuleResolveCache;

	RemasterWorldState _remasterWorldState;
	RemasterStaticScene _remasterStaticScene;
	bool _hdStaticSceneCommandsReady = false;
	size_t _hdStaticCommandCount = 0;
	bool _hdProducingDynamic = false;
	unsigned long long _remasterWorldStateBuiltRevision;
	std::vector<HdDrawCommand> _hdDrawCommands;
	struct HdSurfaceCursorQuad
	{
		Position tile;
		std::array<float, 8> xy{}; // top, right, bottom, left in logical map space
		std::array<float, 4> depth{};
	};
	std::vector<HdSurfaceCursorQuad> _hdSurfaceCursorQuads;
	// All physical ground quads, including ground outside mission knowledge.
	// The HD fog mask clips this layer; cursor and gameplay keep their own gates.
	std::vector<HdSurfaceCursorQuad> _hdFogSurfaceQuads;
	std::vector<unsigned> _drawOrderBuffer;
	unsigned _drawSequence;
	unsigned _hdLegacyBridgeRanges;
	unsigned _hdLegacyGpuCompatCommands;
	Uint32 _hdLastRealTick;
	// RC11 TEST6 physical framebuffer cache.  The logical Map already owns an
	// invalidation flag; this revision mirrors successful Map::draw() calls so
	// the expensive RGBA pass can reuse an already composed physical viewport.
	unsigned long long _hdPhysicalRevision;
	// BEDROCK terrain vertices contain projected geometry and sampled presentation
	// light. They stay valid while the logical map revision and view level match.
	std::vector<HdGpuBedrockVertex> _hdBedrockGeometry;
	unsigned long long _hdBedrockGeometryRevision = ~0ULL;
	unsigned long long _hdBedrockGeometryGpuRevision = 0;
	int _hdBedrockGeometryViewLevel = -1;
	int _hdBedrockGeometryMapW = 0;
	int _hdBedrockGeometryMapH = 0;
	unsigned long long _hdPhysicalCachedRevision;
	// REAL HD LIGHTING V2: cached presentation-only directional helmet field.
	// OXCE LL_UNITS remains untouched for gameplay compatibility, but is no
	// longer consumed as the graphical personal-light authority.
	std::vector<float> _realHdHelmetLightField;
	std::vector<float> _realHdHelmetLightFieldR;
	std::vector<float> _realHdHelmetLightFieldG;
	std::vector<float> _realHdHelmetLightFieldB;
	// PERF FOUNDATION V1: lighting invalidation is independent from physical
	// presentation redraws. Camera/selector/resize must not rebuild LOS lighting.
	unsigned long long _realHdLightingRevision = 0;
	unsigned long long _realHdHelmetLightFieldRevision = ~0ULL;
	struct HdPhysicalFrameCacheEntry
	{
		Surface::UniqueBufferPtr buffer;
		Surface::UniqueSurfacePtr surface;
		uint64_t sceneHash = 0;
		uint64_t lastUse = 0;
		bool valid = false;
	};
	std::vector<HdPhysicalFrameCacheEntry> _hdPhysicalFrameCache;
	int _hdPhysicalCurrentCacheIndex;
	uint64_t _hdPhysicalUseCounter;
	int _hdPhysicalCacheMaxEntries;
	int _hdPhysicalCacheX, _hdPhysicalCacheY, _hdPhysicalCacheW, _hdPhysicalCacheH;
	double _hdPhysicalCacheScaleX, _hdPhysicalCacheScaleY;
	Uint32 _hdPhysicalLastMs;
	unsigned long long _hdPhysicalPixelsTested, _hdPhysicalPixelsWritten;
	bool _hdPhysicalLastCacheHit;
	// The Legacy Map can temporarily become a full replacement screen during
	// hidden movement/end-turn processing.  Keep that presentation fact explicit
	// so the later physical HD pass cannot redraw BEDROCK or cached RGBA sprites
	// over the replacement message.
	bool _hdPhysicalMapSuppressed;

	// TEST8-F redraw tracer. Diagnostic only.
	uint64_t _hdRedrawCount[HDR_COUNT];
	uint64_t _hdRedrawSnapshot[HDR_COUNT];
	uint64_t _hdRedrawRate[HDR_COUNT];
	Uint32 _hdRedrawTraceTick;
	unsigned _hdRedrawPendingMask;
	unsigned _hdRedrawLastMask;
	void updateHdRedrawTraceRates();
	bool usesFullBodySprite(const BattleUnit *unit) const;
	std::string getHdUnitState(const BattleUnit *unit) const;
	int getHdUnitFrame(const BattleUnit *unit, const std::string &state) const;
	std::string findHdUnitVariant(const std::string &base, const std::string &state, int direction, int frame) const;
	std::string findHdUnitBodyAsset(const BattleUnit *unit) const;
	std::string findHdUnitCombinedAsset(const BattleUnit *unit) const;
	std::string findHdUnitOverlayAsset(const BattleUnit *unit, const std::string &layer, const std::string &hand, const BattleItem *item) const;
	std::string findHdTerrainAsset(const Tile *tile, TilePart part) const;
	std::string findHdSurfaceSetAsset(const std::string &setName, int frame) const;
	std::set<std::string> _hdPngLayerFallbacks;
	const HdVisualRule *findHdTerrainVisualRule(const Tile *tile, TilePart part) const;
	const HdVisualRule *findHdSurfaceSetVisualRule(const std::string &setName, int frame) const;
	bool queueHdVisualRule(const HdVisualRule &rule, const std::string &instanceKey, int x, int y, int shade, bool rightHalfOnly, unsigned order, const GraphSubset *clipMask = nullptr, long long epochTimeMs = -1, int epochTurn = -1);
	bool queueHdTerrainOrConvention(const Tile *tile, TilePart part, int x, int y, int shade, bool rightHalfOnly, unsigned order, const GraphSubset *clipMask = nullptr);
	bool queueHdSurfaceSetOrConvention(const std::string &setName, int frame, const std::string &instanceKey, int x, int y, int shade, bool rightHalfOnly, unsigned order, const GraphSubset *clipMask = nullptr);
	unsigned nextDrawSequence();
	void markSourceDrawOrder(SurfaceRaw<const Uint8> src, int x, int y, bool rightHalfOnly, unsigned order);
	void markChangedDrawOrder(const std::vector<Uint8> &before, const GraphSubset &area, unsigned order);
	std::vector<Uint8> snapshotArea(const GraphSubset &area) const;
	void trackedBlitRaw(SurfaceRaw<Uint8> destination, SurfaceRaw<const Uint8> source, int x, int y, int shade = 0, bool rightHalfOnly = false, int newBaseColor = 0);
	void trackedBlitNShade(const Surface *source, SurfaceRaw<Uint8> destination, int x, int y, int shade = 0, bool half = false, int newBaseColor = 0);
	void drawHdSurfaceSetOrLegacy(SurfaceRaw<Uint8> destination, const std::string &setName, int frame, SurfaceRaw<const Uint8> legacy, int x, int y, int shade = 0, bool rightHalfOnly = false, int newBaseColor = 0, bool postVisibility = false);
	void queueHdAsset(const std::string &assetPath, int nativeScale, int offsetX, int offsetY, int x, int y, int shade, bool rightHalfOnly, unsigned order, const GraphSubset *clipMask = nullptr, const std::string &colorMode = "auto", const std::string &materialProfile = "");
	void rebuildRemasterWorldState();
	void rebuildHdStaticSceneCommands();
	void appendHdDynamicUnitCommands();
	void appendHdCursorCommand();
	void appendHdCombatEffectsCommands();
	void appendHdTacticalIndicators();
	void updateHdCursorInfo();
	void updateHdProjectileCamera();
	struct HdTacticalNumber { int value, x, y, color; };
	std::vector<HdTacticalNumber> _hdTacticalNumbers;
	std::vector<Tile*> _hdPresentationTiles;
	bool _hdCursorInfoVisible = false;
	bool _hdExplosionFlash = false;
	struct HdSedimentBurst { Position voxel; Uint32 started; int diameter; int turn; bool observed; };
	std::vector<HdSedimentBurst> _hdSedimentBursts;
	size_t _hdObservedCraterCount = 0;
	bool _hdCraterEventsInitialized = false;
	void queueHdSemanticFrame(const std::string &family, int frame, const Position &world, int x, int y, int shade = 0, int color = 0, bool overlay = false);
	void queueLegacyIndexedAsset(SurfaceRaw<const Uint8> source, int x, int y, int shade, bool rightHalfOnly, unsigned order, int newBaseColor = 0, const GraphSubset *clipMask = nullptr);
	bool queueLegacyIfGpuReady(SurfaceRaw<const Uint8> source, int x, int y, int shade, bool rightHalfOnly, unsigned order, int newBaseColor = 0, const GraphSubset *clipMask = nullptr, const Tile *visibilityTile = nullptr, TilePart visibilityPart = O_FLOOR);

	void drawUnit(UnitSprite &unitSprite, Tile *unitTile, Tile *currTile, Position tileScreenPosition, bool topLayer, BattleUnit* movingUnit = nullptr);
	void drawTerrain(Surface *surface);
	int getTerrainLevel(const Position& pos, int size) const;
	int getWallShade(TilePart part, Tile* tileFrot);
	int _iconHeight, _iconWidth, _messageColor;
	int _hostileBarColor, _neutralBarColor, _borderBarColor;
	const std::vector<Uint8> *_transparencies;
	bool _showObstacles;
	bool _showInfoOnCursor;
public:
	/// Creates a new map at the specified position and size.
	Map(Game* game, int width, int height, int x, int y, int visibleMapHeight);
	/// Cleans up the map.
	~Map();
	/// Initializes the map.
	void init();
	/// Pre-decodes the HD resources belonging to the current tactical mission.
	void prewarmHdMissionResources();
	/// Reload authored HD assets from their current VFS files, including same-path replacements.
	void reloadHdResources();
	/// Handles timers.
	void think() override;
	/// Draws the surface.
	void draw() override;
	/// Composites all queued RGBA replacements using the recorded legacy draw order.
	void blitHdOverlays(SDL_Surface *destination);
	/// Composites/caches the same replacements directly in the final physical framebuffer.
	bool blitHdOverlaysGpu(const Screen *screen, int physicalWidth, int physicalHeight);
	void blitHdOverlaysPhysical(SDL_Surface *destination, const Screen *screen);
	Uint32 getHdPhysicalLastMs() const { return _hdPhysicalLastMs; }
	unsigned long long getHdPhysicalPixelsTested() const { return _hdPhysicalPixelsTested; }
	unsigned long long getHdPhysicalPixelsWritten() const { return _hdPhysicalPixelsWritten; }
	bool getHdPhysicalLastCacheHit() const { return _hdPhysicalLastCacheHit; }
	std::string getRemasterWorldTrace();
	unsigned getHdPhysicalCommandCount() const { return (unsigned)_hdDrawCommands.size(); }
	void invalidateHd(HdRedrawReason reason);
	void invalidate(bool valid = true);
	std::string getHdRedrawTrace() const;
	/// Debug-only REAL HD FOV probe: compares source discovery with voxel targetability per Z/TilePart.
	std::string getHdFovAuditTrace() const;
	void refreshAIProgress(int progress);
	/// Sets the palette.
	void setPalette(const SDL_Color *colors, int firstcolor = 0, int ncolors = 256) override;
	void refreshHiddenMovementBackground();
	/// Special handling for mouse press.
	void mousePress(Action *action, State *state) override;
	/// Special handling for mouse release.
	void mouseRelease(Action *action, State *state) override;
	/// Special handling for mouse over
	void mouseOver(Action *action, State *state) override;
	/// Special handling for key presses.
	void keyboardPress(Action *action, State *state) override;
	/// Special handling for key releases.
	void keyboardRelease(Action *action, State *state) override;
	/// Rotates the tile frames 0-7
	void animate(bool redraw);
	/// Sets the battlescape selector position relative to mouse position.
	void setSelectorPosition(int mx, int my);
	/// Gets the currently selected position.
	void getSelectorPosition(Position *pos) const;
	/// Calculates the offset of a soldier, when it is walking in the middle of 2 tiles.
	UnitWalkingOffset calculateWalkingOffset(const BattleUnit *unit) const;
	/// Sets the 3D cursor type.
	void setCursorType(CursorType type, int size = 1);
	/// Gets the 3D cursor type.
	CursorType getCursorType() const;

	/// Sets projectile.
	void setProjectile(Projectile *projectile);
	/// Gets projectile.
	Projectile *getProjectile() const;
	/// Sets follow projectile flag.
	void setFollowProjectile(bool followProjectile) { _followProjectile = followProjectile; }
	/// Gets follow projectile flag.
	bool getFollowProjectile() const { return _followProjectile; }
	/// Gets alt pressed flag.
	bool isAltPressed() const { return _isAltPressed; }
	/// Gets ctrl pressed flag.
	bool isCtrlPressed() const { return _isCtrlPressed; }
	/// Add new vapor particle.
	void addVaporParticle(Position pos, Particle particle);
	/// Get all vapor for tile.
	Collections::Range<const Particle*> getVaporParticle(const Tile* tile, int topLayer) const;
	/// Gets explosion set.
	std::list<Explosion*> *getExplosions();

	/// Gets the pointer to the camera.
	Camera *getCamera();
	/// Mouse-scrolls the camera.
	void scrollMouse();
	/// Keyboard-scrolls the camera.
	void scrollKey();
	/// fades in/out
	void fadeShade();
	/// Get waypoints vector.
	std::vector<Position> *getWaypoints();
	/// Set mouse-buttons' pressed state.
	void setButtonsPressed(Uint8 button, bool pressed);
	/// Sets the unitDying flag.
	void setUnitDying(bool flag);
	/// Refreshes the battlescape selector after scrolling.
	void refreshSelectorPosition();
	/// Expands/collapses the usable tactical viewport when the HUD is hidden/shown.
	void setHudHidden(bool hidden);
	/// Reserves a world-logical viewport height for the independent UI-space HUD.
	void setHudVisibleMapHeightOverride(int height);
	int getVisibleMapHeight() const { return _visibleMapHeight; }
	/// Special handling for updating map height.
	void setHeight(int height) override;
	/// Special handling for updating map width.
	void setWidth(int width) override;
	/// Get the vertical position of the hidden movement screen.
	int getMessageY() const;
	/// Get the icon height.
	int getIconHeight() const;
	/// Get the icon width.
	int getIconWidth() const;
	/// Convert a map position to a sound angle.
	int getSoundAngle(const Position& pos) const;
	/// Reset the camera smoothing bool.
	void resetCameraSmoothing();
	/// Set whether the screen should "flash" or not.
	void setBlastFlash(bool flash);
	/// Check if the screen is flashing this.
	bool getBlastFlash() const;
	/// Modify shade for fading
	void rebuildRealHdHelmetLightField();
	float realHdHelmetLightAt(const Position &position);
	void realHdHelmetLightColorAt(const Position &position, float &r, float &g, float &b);
	float realHdLightingAt(Tile *tile);
	void realHdLightingTintAt(const Tile *tile, float &r, float &g, float &b);
	void applyRealHdLightingTintToCommands(size_t begin, const Tile *tile);
	int realHdShade(Tile *tile);
	int reShade(Tile *tile);
	/// toggle the night-vision mode
	void enableNightVision();
	void toggleNightVision();
	void toggleDebugVisionMode();
	void persistToggles();
	/// Resets obstacle markers.
	void resetObstacles();
	/// Enables obstacle markers.
	void enableObstacles();
	/// Disables obstacle markers.
	void disableObstacles();
};

}
