#pragma once
#include <SDL.h>
#include <cstdint>
#include <map>
#include <string>
#include <vector>
#include "HdAssetContract.h"

namespace OpenXcom
{

/**
 * Small true-colour PNG cache used by the HD compatibility layer.
 * Legacy OpenXcom Surface objects stay 8-bit; HD images are kept as RGBA
 * side resources and composited only when the destination is 32-bit.
 */
struct HdImage
{
	bool loadAttempted = false;
	bool loaded = false;
	unsigned width = 0;
	unsigned height = 0;

	// RC12 P8: preserve the source PNG colour model without assigning meaning
	// to it. A paletted PNG may be ordinary authored HD artwork. `indices` are
	// used as TFTD palette indices only when colorMode is explicitly
	// IndexedLegacy (for example through the LegacyIndexed namespace).
	// RGBA is always retained so palette-encoded authored art can use
	// Environment or Fixed exactly like RGB/RGBA source files.
	// PNG palette samples (1/2/4/8 bits in the file), normalized to 8-bit indices.
	bool paletteIndexed8 = false;
	std::vector<unsigned char> indices;
	std::vector<unsigned char> rgba; // always retained for software/fallback paths

	// TEST8-B: source-alpha span profile. One conservative [min,max) span
	// per source row lets the software compositor skip transparent margins
	// without changing the final pixels.
	std::vector<unsigned> alphaRowMinX;
	std::vector<unsigned> alphaRowMaxX;
	bool hasVisiblePixels = false;

	// REAL HD PERF FOUNDATION V1: luma*alpha influence bounds are immutable
	// metadata of the decoded asset. Impact/crater code previously recomputed
	// these bounds by scanning every mask pixel on every rendered Map pass.
	// Compute them once while HdImage already walks the pixels for alpha spans.
	unsigned influenceMinX = 0;
	unsigned influenceMinY = 0;
	unsigned influenceMaxX = 0;
	unsigned influenceMaxY = 0;
	bool hasInfluencePixels = false;
};

class HdImageCache
{
private:
	std::map<std::string, HdImage> _images;
	std::map<std::string, bool> _exists;
	bool _prewarmComplete = false;
	std::uint64_t _generation = 0;
	size_t _postPrewarmLoads = 0;

public:
	HdImage *get(const std::string &path);
	// A selected HD resource must decode successfully. Failure is an explicit
	// diagnostic, never permission to draw a native sprite or stale framebuffer.
	HdImage &require(const std::string &path);
	HdAssetResolution resolve(const HdAssetKey &key, const std::vector<HdAssetCandidate> &candidates, bool traceMissing = true);
	bool exists(const std::string &path);
	bool usable(const std::string &path);
	void markPrewarmComplete() { _prewarmComplete = true; }
	size_t getPostPrewarmLoads() const { return _postPrewarmLoads; }
	size_t imageCount() const;
	std::uint64_t estimatedCpuBytes() const;
	void clear();
	std::uint64_t generation() const { return _generation; }

	/**
	 * Alpha-composite an RGBA image onto a 32-bit SDL surface.
	 * shade follows Battlescape convention: 0 = native brightness, 16 = black.
	 */
	static void blit(SDL_Surface *destination, const HdImage &image,
		int x, int y, int drawWidth, int drawHeight,
		Uint8 opacity = 255, int shade = 0, const SDL_Rect *clip = nullptr,
		bool rightHalfOnly = false);
};

}
