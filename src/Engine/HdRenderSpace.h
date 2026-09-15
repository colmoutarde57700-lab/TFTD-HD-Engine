#pragma once

#include <algorithm>
#include <cmath>

namespace OpenXcom
{

/**
 * RC12 P4: renderer-native coordinate system.
 *
 * OXCE gameplay, voxels, tile selection, Camera and input remain on the
 * historical 32x40 coordinate system.  Presentation uses a completely
 * separate x16 render space: one gameplay cell is represented as 512x640
 * renderer pixels.
 *
 * Asset resolution is independent from world size.  A 32x40 Legacy sprite
 * (nativeScale=1), a 256x320 HD sprite (nativeScale=8) and a 512x640 HD sprite
 * (nativeScale=16) all occupy the same apparent geometry.  Only their native
 * sampling/detail differs.
 */
struct HdRenderSpace
{
	static constexpr int LegacyTileWidth = 32;
	static constexpr int LegacyTileHeight = 40;
	static constexpr int Scale = 16;
	static constexpr int TileWidth = LegacyTileWidth * Scale;   // 512
	static constexpr int TileHeight = LegacyTileHeight * Scale; // 640
	static constexpr int AutoNativeScale = 0;

	static constexpr int legacyToRender(int v) { return v * Scale; }
	static constexpr double legacyToRender(double v) { return v * (double)Scale; }
	static constexpr double renderToLegacy(double v) { return v / (double)Scale; }

	// Convention assets use a 32-pixel-wide Legacy cell as the scale reference.
	// This deliberately accepts 1/2/4/8/16 (and remains forward compatible with
	// larger integer scales) instead of hard-coding a single HD resolution.
	static int inferNativeScale(unsigned width, unsigned /*height*/ = 0)
	{
		if (width < (unsigned)LegacyTileWidth) return 1;
		return std::max(1, (int)std::lround((double)width / (double)LegacyTileWidth));
	}

	static int resolveNativeScale(int declaredScale, unsigned width, unsigned height = 0)
	{
		return declaredScale > 0 ? declaredScale : inferNativeScale(width, height);
	}

	static double assetPixelsToRender(double pixels, int nativeScale)
	{
		return pixels * (double)Scale / (double)std::max(1, nativeScale);
	}
};

}
