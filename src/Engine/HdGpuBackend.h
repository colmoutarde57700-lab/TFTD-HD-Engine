#include <array>
#pragma once

#include <SDL.h>
#include <cstdint>
#include <string>
#include "HdColorTransform.h"
#include "PresentationContext.h"
#include "HdUnit3DAsset.h"

namespace OpenXcom
{

/**
 * Real HD GPU presentation boundary.
 *
 * OXCE remains authoritative for gameplay and semantic world state.  The
 * Battlescape GPU compositor owns physical composition for Real HD output.
 * Legacy raster order is accepted only through an explicit compatibility
 * bridge and is never sampled by authored HD primitives.
 */
struct HdGpuMapSprite
{
    bool roofCaustic = false;
    float roofWorldX=0, roofWorldY=0, roofTime=0;
    std::array<unsigned,16> roofSunRows{};
	bool premultiplied = false;
	const char *assetKey = nullptr;
	const unsigned char *rgba = nullptr;
	const unsigned char *indices = nullptr;
	bool legacyIndexed = false;
	HdColorMode colorMode = HdColorMode::Fixed;
	unsigned imageWidth = 0;
	unsigned imageHeight = 0;
	unsigned sourcePitch = 0; // bytes; 0 = tightly packed

	int destX = 0;
	int destY = 0;
	int destW = 0;
	int destH = 0;

	double baseRenderX = 0.0;
	double baseRenderY = 0.0;
	double renderToPhysicalX = 1.0;
	double renderToPhysicalY = 1.0;

	// Real HD presentation sequence. This remains only a compatibility/painter
	// sequencing key for 2D sprite adapters; it is not geometric depth.
	unsigned drawOrder = 0;
	int shade = 0;
	// Presentation-only physical-light tint. Neutral for all ordinary lighting;
	// Magnetic-Ion lateral emitters may shift this slightly colder.
	float lightTintR = 1.0f;
	float lightTintG = 1.0f;
	float lightTintB = 1.0f;
	int legacyBaseColor = 0; // Surface::blitRaw newBaseColor semantics (0 = unchanged)
	// HD MATERIAL GRADE V1. True-colour post-process parameters for authored HD world art.
	// These are independent from the legacy palette and from BEDROCK grading.
	bool materialGradeEnabled = false;
	float materialTintR = 1.0f;
	float materialTintG = 1.0f;
	float materialTintB = 1.0f;
	float materialExposure = 1.0f;
	float materialContrast = 1.0f;
	float materialSaturation = 1.0f;
	bool rightHalfOnly = false;
	bool hasClipMask = false;
	int clipBegX = 0;
	int clipBegY = 0;
	int clipEndX = 0;
	int clipEndY = 0;
};

/**
 * Generic presentation sprite used outside the Battlescape map.
 *
 * Unlike HdGpuMapSprite this has no map draw-order or voxel semantics. It is
 * the common physical presentation primitive for HUD/UI/Basescape/Geoscape
 * assets. Legacy surfaces and authored HD PNGs can therefore share the same
 * D3D11 compositor while OXCE keeps ownership of layout and input geometry.
 */
struct HdGpuPresentationSprite
{
	const char *assetKey = nullptr;
	const unsigned char *rgba = nullptr;
	const unsigned char *indices = nullptr;
	bool legacyIndexed = false;
	unsigned imageWidth = 0;
	unsigned imageHeight = 0;

	int destX = 0;
	int destY = 0;
	int destW = 0;
	int destH = 0;

	Uint8 opacity = 255;
	bool hasColorKey = false;
	Uint8 colorKey = 0;
	bool hasClip = false;
	int clipX0 = 0;
	int clipY0 = 0;
	int clipX1 = 0;
	int clipY1 = 0;
	const SDL_Color *palette = nullptr;
};


/** One vertex of the presentation-only primary terrain mesh. */
struct HdGpuBedrockVertex
{
	float logicalX = 0.0f;
	float logicalY = 0.0f;
	float worldU = 0.0f;
	float worldV = 0.0f;
	// Non-repeating world-space UV into the merged crater field atlas.
	float craterU = 0.0f;
	float craterV = 0.0f;
	float light = 1.0f;
	float lightTintR = 1.0f;
	float lightTintG = 1.0f;
	float lightTintB = 1.0f;
	// 0 = top/surface material, 1 = exposed vertical substrate material.
	float materialRole = 0.0f;
	// Orthographic isometric camera-space depth source in OXCE voxel units.
	// For the Battlescape projection, x+y+z varies along the camera ray while
	// projected screen coordinates remain constant. Larger values are nearer.
	float viewDepth = 0.0f;
	// Physical map coordinates used only by the Real HD fog sampler.
	float fogX = 0.0f;
	float fogY = 0.0f;
	float fogLayer = 0.0f;
};


struct HdGpuLocalLight
{
	// World-space centre/radius, retained for material-space consumers.
	float x = 0.0f;
	float y = 0.0f;
	float z = 0.0f;
	float radius = 0.0f;
	float r = 1.0f;
	float g = 1.0f;
	float b = 1.0f;
	float intensity = 0.0f;

	// Physical-screen projection for the universal post-light pass.
	float screenX = 0.0f;
	float screenY = 0.0f;
	float screenRadiusX = 1.0f;
	float screenRadiusY = 1.0f;
};

struct HdGpuSmokeBlob
{
	float screenX = 0.0f;
	float screenY = 0.0f;
	float screenRadiusX = 1.0f;
	float screenRadiusY = 1.0f;
	float density = 0.0f;
	float phase = 0.0f;
	float r = 0.52f, g = 0.60f, b = 0.62f;
	float opacity = 1.0f;
};

struct HdGpuVisibilityVertex
{
	// Physical framebuffer coordinates. alpha=0 keeps world pixel, alpha=1 masks to black.
	float x = 0.0f;
	float y = 0.0f;
	float alpha = 0.0f;
};

// Native tactical cursor geometry. It uses the world's geometric depth and
// is drawn in the map order so known foreground sprites can occlude it.
struct HdGpuCursorVertex
{
	float x = 0.0f, y = 0.0f;
	float viewDepth = 0.0f;
	float alpha = 1.0f;
};

/**
 * One vertex of the generic Battlescape world-coverage prepass.
 *
 * Unlike HdGpuMapSprite this is real geometry: logicalX/logicalY are the
 * projected world position and viewDepth is the independent camera-ray depth
 * (x+y+z in voxel units). No Legacy painter-order information is present.
 */
struct HdGpuWorldCoverageVertex
{
	float logicalX = 0.0f;
	float logicalY = 0.0f;
	float viewDepth = 0.0f;
};

struct HdGpuWorldCoverage
{
	const HdGpuWorldCoverageVertex *vertices = nullptr;
	unsigned vertexCount = 0;
	int destX = 0;
	int destY = 0;
	int destW = 0;
	int destH = 0;
	double mapPhysicalOriginX = 0.0;
	double mapPhysicalOriginY = 0.0;
	double renderScaleX = 1.0;
	double renderScaleY = 1.0;
};

enum class HdGpuWorldDepthMode
{
	Disabled = 0,
	OpaqueWrite,
	TranslucentRead
};

struct HdGpuBedrock
{
	const unsigned char *fogCoverage = nullptr;
	unsigned fogWidth = 0;
	unsigned fogHeight = 0;
	unsigned fogLayerHeight = 0;
	int fogSoftnessPermille = 0;
	const char *assetKey = nullptr;
	const unsigned char *rgba = nullptr;
	unsigned imageWidth = 0;
	unsigned imageHeight = 0;
	const char *normalKey = nullptr;
	const unsigned char *normalRgba = nullptr;
	unsigned normalWidth = 0;
	unsigned normalHeight = 0;
	const char *roughnessKey = nullptr;
	const unsigned char *roughnessRgba = nullptr;
	unsigned roughnessWidth = 0;
	unsigned roughnessHeight = 0;
	const char *aoKey = nullptr;
	const unsigned char *aoRgba = nullptr;
	unsigned aoWidth = 0;
	unsigned aoHeight = 0;
	bool usePbrMaterial = false;
	// Experimental AO-derived microheight, in thousandths of a tactical voxel.
	int sandMicroreliefPermille = 0;
	float causticStrength = 0.0f;
	float causticPhase = 0.0f;

	// Optional second material slot used by exposed vertical faces.
	const char *verticalAssetKey = nullptr;
	const unsigned char *verticalRgba = nullptr;
	unsigned verticalImageWidth = 0;
	unsigned verticalImageHeight = 0;
	const char *verticalNormalKey = nullptr;
	const unsigned char *verticalNormalRgba = nullptr;
	unsigned verticalNormalWidth = 0;
	unsigned verticalNormalHeight = 0;
	const char *verticalRoughnessKey = nullptr;
	const unsigned char *verticalRoughnessRgba = nullptr;
	unsigned verticalRoughnessWidth = 0;
	unsigned verticalRoughnessHeight = 0;
	const char *verticalAoKey = nullptr;
	const unsigned char *verticalAoRgba = nullptr;
	unsigned verticalAoWidth = 0;
	unsigned verticalAoHeight = 0;
	bool useVerticalPbrMaterial = false;

	// Optional already-merged RGBA8 BEDROCK blast-impact field. R=blast core, G=blast rim, B=blast halo.
	// This is never a Legacy decal stack: nearby impacts are composited before HD shading.
	const unsigned char *craterField = nullptr;
	unsigned craterFieldWidth = 0;
	unsigned craterFieldHeight = 0;
	unsigned long long craterFieldRevision = 0;

	// Optional already-merged RGBA8 BEDROCK weapon-impact field. RGB = authored CORE/RIM/HALO,
	// A = legacy single-mask fallback / coverage. Same world-space resolution as craterField when present.
	const unsigned char *weaponField = nullptr;
	unsigned weaponFieldWidth = 0;
	unsigned weaponFieldHeight = 0;
	unsigned long long weaponFieldRevision = 0;

	// Optional presentation-only RGB local lights. They never alter OXCE light/LOS gameplay.
	const HdGpuLocalLight *localLights = nullptr;
	unsigned localLightCount = 0;
	unsigned localLightMapW = 0;
	unsigned localLightMapH = 0;
	unsigned localLightMapZ = 0;

	const HdGpuBedrockVertex *vertices = nullptr;
	unsigned vertexCount = 0;
	// Unique content revision across maps; zero means always upload.
	unsigned long long vertexRevision = 0;
	int destX = 0;
	int destY = 0;
	int destW = 0;
	int destH = 0;
	double mapPhysicalOriginX = 0.0;
	double mapPhysicalOriginY = 0.0;
	double renderScaleX = 1.0;
	double renderScaleY = 1.0;
	// Geometric depth is independent of Legacy painter order. Vertices carry raw
	// camera-ray depth (x+y+z in voxel units); the map frame owns normalization.
	HdGpuWorldDepthMode worldDepthMode = HdGpuWorldDepthMode::OpaqueWrite;
	HdEnvironmentProfile environmentProfile = HdEnvironmentProfile::Global;
	bool applyEnvironment = true;
	// BEDROCK raw environment-grade strengths sent directly to the shader.
	// 0 = no grade contribution, 1000 = full LUT result, >1000 = deliberate extrapolation beyond LUT result.
	int environmentGradeTopPermille = 480;
	int environmentGradeVerticalPermille = 540;
	int environmentGradeCoveredPermille = 290;
	// Independent raw luminance multiplier. 1000 = neutral, >1000 brighter.
	int depthLuminancePermille = 1000;
};

/**
 * Compatibility bridge for world pixels that still originate from an OXCE CPU
 * rasterizer.  It is deliberately isolated from authored HD primitives: only
 * this bridge reads the Legacy order map, and it replays explicit order ranges
 * into the GPU compositor. It never writes geometric world depth.
 */
struct HdGpuMapLegacyOverlay
{
	int destX = 0;
	int destY = 0;
	int destW = 0;
	int destH = 0;
	double mapPhysicalOriginX = 0.0;
	double mapPhysicalOriginY = 0.0;
	double renderScaleX = 1.0;
	double renderScaleY = 1.0;
	int sourceLogicalX = 0;
	int sourceLogicalY = 0;
	unsigned minDrawOrder = 1;
	unsigned maxDrawOrder = 0;
};

class HdGpuBackend
{
public:
	static HdGpuBackend &instance();

	/// Preflight the D3D11 presentation path before OXCE rasterizes the logical states.
	/// WORLD_GPU_V1 uses this to decide whether static Legacy Battlescape sprites may
	/// be emitted as GPU commands instead of being rasterized into the Map surface.
	bool preparePresentationFrame(SDL_Surface *physicalTarget, SDL_Surface *logicalBase);
	bool directWorldReady() const;

	/// Begin a GPU frame from the already-correct P4 physical software base.
	bool beginFrame(SDL_Surface *physicalBase);
	/// Renderer Architecture V1: upload the logical OXCE canvas and perform the
	/// logical->physical nearest-neighbour presentation directly on D3D11.
	/// This bypasses the historical full-frame CPU Zoom::flipWithZoom path while
	/// preserving exactly the same logical canvas and letterbox geometry.
	bool beginFrameLogical(SDL_Surface *physicalTarget, SDL_Surface *logicalBase, const PresentationContext &presentation);
	/// Strict REAL HD: clear and present only backend-owned pixels. Never upload
	/// the OXCE logical canvas or a software fallback framebuffer.
	bool beginFrameHdOnly(SDL_Surface *physicalTarget);
	/// Transparent software staging layer for HUD/popups/cursor only.
	SDL_Surface *cpuOverlay();
	/// True only for the backend-owned transparent staging surface.
	bool isCpuOverlay(const SDL_Surface *surface) const;
	/// Straight-alpha source-over blit into the transparent staging layer.
	/// SDL 1.2 blits are not used here because their destination-alpha semantics
	/// are only safe for the historical opaque framebuffer.
	bool blitCpuOverlay(SDL_Surface *source, int dstX, int dstY);
	/// Flush pending CPU overlay pixels before a direct GPU presentation draw.
	/// This preserves exact z-order when legacy and HD UI are mixed.
	bool flushCpuOverlay();
	/// REAL HD diagnostic text drawn directly on the swap chain. No OXCE
	/// Text/Surface, SDL raster, or native fallback participates in this path.
	bool drawDebugOverlay(const std::string &utf8);
	/// Draw an authored or indexed presentation asset directly on the current
	/// D3D11 backbuffer. No Battlescape draw-order semantics are applied.
	bool drawPresentationSprite(const HdGpuPresentationSprite &sprite);
	/// Draw one ordinary OXCE Surface directly on D3D11 at physical size. This
	/// is the Legacy fallback path of the unified presentation renderer and
	/// avoids CPU zoomSurface() for 8-bit UI/HUD surfaces.
	bool drawLegacySurface(SDL_Surface *source, const void *stableKey,
		int destX, int destY, int destW, int destH, Uint8 opacity = 255);

	/// GEOSCAPE HD CAMERA V5: draw the authored unit-scale globe through a fixed-FOV
	/// perspective camera. Zoom is camera distance; mesh scale never changes.
	bool drawGeoscapeGlobeProof(const char *assetKey, int destX, int destY, int destW, int destH,
		float centerLon, float centerLat, float cameraDistance, float fovYDegrees);
	/// Whether Map may submit GPU commands in the current physical pass.
	bool frameActive() const;
	/// Cancel current GPU frame; Screen will immediately run the P4 software fallback.
	void abortFrame();
	/// Alpha-composite CPU HUD layer and present. Returns false on any device/swap failure.
	bool present();

	/// Begin the Real HD world compositor from frame/map facts only. This lifecycle
	/// no longer requires any product of the Legacy raster.
	bool beginMap(int mapLogicalWidth, int mapLogicalHeight,
		float worldDepthMin, float worldDepthMax, const SDL_Color *palette, const SDL_Color *neutralPalette,
		int clipX0, int clipY0, int clipX1, int clipY1);
	/// Upload the Legacy per-pixel painter order only when the explicit compatibility
	/// bridge will actually replay unmigrated Legacy pixels.
	bool setMapLegacyBridgeOrder(const unsigned *legacyBridgeOrder, int mapWidth, int mapHeight);
	/// Draw one semantic HD command directly from its native x1/x4/x8/x16 texture.
	bool drawMapSprite(const HdGpuMapSprite &sprite);
	bool drawMapUnit3D(const HdGpuMapSprite &placement, const std::string &asset, const HdUnit3DPose &pose);
	/// Populate the generic Real HD geometric depth/coverage target from semantic
	/// world geometry. This is a depth-only prepass and never touches world colour.
	bool drawMapWorldCoverage(const HdGpuWorldCoverage &coverage);
	/// Draw the primary continuous terrain mesh.
	bool drawBedrock(const HdGpuBedrock &bedrock);
	/// Universal presentation-only RGB light pass over the complete Battlescape world.
	/// This runs after all map sprites, so Legacy, HD, BEDROCK and future map assets inherit it.
	bool drawMapLocalLights(const HdGpuLocalLight *lights, unsigned lightCount);
	/// Presentation-only continuous smoke blobs built from OXCE Tile::getSmoke().
	/// Gameplay smoke/LOS remains entirely authoritative in TileEngine.
	bool drawMapSmokeVolume(const HdGpuSmokeBlob *blobs, unsigned blobCount);
	/// Legacy V4 geometric visibility mask (kept only for patch compatibility; no longer used by takeover V1).
	bool drawMapVisibilityMask(const HdGpuVisibilityVertex *vertices, unsigned vertexCount);
	/// Presentation-only feather of the projected, source-approved world
	/// silhouette. Unknown geometry remains absent; tactical knowledge is unchanged.
	bool applyMapVisionFeather(const HdGpuVisibilityVertex *visibleSurface,
		unsigned vertexCount, int softnessPermille);
	bool drawMapSurfaceCursor(const HdGpuCursorVertex *vertices, unsigned vertexCount,
		bool yellow, bool violet = false);
	/// REAL HD FOV V1 diagnostic hard-mask fallback. The active path is source-owned
	/// and multi-Z; these parameters are retained only for source/config compatibility.
	bool drawMapVisibilityTakeover(const unsigned char *hiddenCells, unsigned mapW, unsigned mapH,
		int destX, int destY, int destW, int destH, double renderScaleX, double renderScaleY,
		int mapOffsetX, int mapOffsetY, int spriteWidth, int spriteHeight, int viewLevel,
		int featherPermille, int roundPermille, int insetPermille, int contrastPermille,
		int aaPermille, int opacityPermille);
	/// Drop cached authored HD textures/material mip chains so same-path files can be reloaded.
	void clearHdImageCache();
	/// Replay one explicit Legacy compatibility range into the GPU-owned world.
	bool drawMapLegacyOverlay(const HdGpuMapLegacyOverlay &overlay);
	void endMap();

	/// Diagnostic state from the most recently presented frame.
	bool wasUsedLastFrame() const;
	const char *lastModeName() const;
	unsigned lastMapDrawCalls() const;
	unsigned lastIndexedDrawCalls() const;
	unsigned lastEnvironmentDrawCalls() const;
	unsigned lastPresentationDrawCalls() const;
	unsigned lastLegacyPresentationDrawCalls() const;

	// PERF FOUNDATION V1 diagnostics. True GPU timings use timestamp/disjoint
	// queries read asynchronously; they never Flush/wait for profiling. Memory
	// numbers are deterministic renderer-owned estimates, not DXGI residency.
	double lastTrueGpuFrameUs() const;
	double lastTrueGpuMapUs() const;
	bool hasTrueGpuTiming() const;
	std::uint64_t resolvedTrueGpuTimingFrames() const;
	std::uint64_t droppedTrueGpuTimingFrames() const;
	unsigned pendingTrueGpuTimingFrames() const;
	unsigned cachedHdImageCount() const;
	std::uint64_t estimatedCachedHdImageBytes() const;
	std::uint64_t estimatedTrackedGpuBytes() const;

private:
	HdGpuBackend();
	~HdGpuBackend();
	HdGpuBackend(const HdGpuBackend &) = delete;
	HdGpuBackend &operator=(const HdGpuBackend &) = delete;

	bool drawMapSpriteResource(const HdGpuMapSprite &sprite, void *resource);
	struct Impl;
	Impl *_impl;
};

}
