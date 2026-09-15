#pragma once

#include <SDL.h>
#include <cstdint>
#include <string>
#include "HdColorTransform.h"

namespace OpenXcom
{

/**
 * RC12 P5 GPU backend boundary.
 *
 * OXCE continues to own gameplay, visibility and draw ordering.  The GPU only
 * receives presentation data that OXCE has already decided.  In particular,
 * no gameplay/camera/input coordinates are moved into D3D11.
 */
struct HdGpuMapSprite
{
	const char *assetKey = nullptr;
	const unsigned char *rgba = nullptr;
	const unsigned char *indices = nullptr;
	bool legacyIndexed = false;
	HdColorMode colorMode = HdColorMode::Fixed;
	HdEnvironmentProfile environmentProfile = HdEnvironmentProfile::Global;
	unsigned imageWidth = 0;
	unsigned imageHeight = 0;

	int destX = 0;
	int destY = 0;
	int destW = 0;
	int destH = 0;

	double baseRenderX = 0.0;
	double baseRenderY = 0.0;
	double renderToPhysicalX = 1.0;
	double renderToPhysicalY = 1.0;

	unsigned drawOrder = 0;
	int shade = 0;
	bool rightHalfOnly = false;
	bool hasClipMask = false;
	int clipBegX = 0;
	int clipBegY = 0;
	int clipEndX = 0;
	int clipEndY = 0;
};

class HdGpuBackend
{
public:
	static HdGpuBackend &instance();

	/// Begin a GPU frame from the already-correct P4 physical software base.
	bool beginFrame(SDL_Surface *physicalBase);
	/// Transparent software staging layer for HUD/popups/cursor only.
	SDL_Surface *cpuOverlay();
	/// True only for the backend-owned transparent staging surface.
	bool isCpuOverlay(const SDL_Surface *surface) const;
	/// Straight-alpha source-over blit into the transparent staging layer.
	/// SDL 1.2 blits are not used here because their destination-alpha semantics
	/// are only safe for the historical opaque framebuffer.
	bool blitCpuOverlay(SDL_Surface *source, int dstX, int dstY);
	/// Whether Map may submit GPU commands in the current physical pass.
	bool frameActive() const;
	/// Cancel current GPU frame; Screen will immediately run the P4 software fallback.
	void abortFrame();
	/// Alpha-composite CPU HUD layer and present. Returns false on any device/swap failure.
	bool present();

	/// Upload OXCE's logical per-pixel draw-order map and set the physical Map clip.
	bool beginMap(const unsigned *drawOrder, int mapWidth, int mapHeight, const SDL_Color *palette,
		const SDL_Color *neutralPalette, int clipX0, int clipY0, int clipX1, int clipY1);
	/// Draw one semantic HD command directly from its native x1/x4/x8/x16 texture.
	bool drawMapSprite(const HdGpuMapSprite &sprite);
	void endMap();

	/// Diagnostic state from the most recently presented frame.
	bool wasUsedLastFrame() const;
	const char *lastModeName() const;
	unsigned lastMapDrawCalls() const;
	unsigned lastIndexedDrawCalls() const;
	unsigned lastEnvironmentDrawCalls() const;

private:
	HdGpuBackend();
	~HdGpuBackend();
	HdGpuBackend(const HdGpuBackend &) = delete;
	HdGpuBackend &operator=(const HdGpuBackend &) = delete;

	struct Impl;
	Impl *_impl;
};

}
