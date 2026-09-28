#pragma once

#include "HdCanvas.h"
#include <SDL.h>

namespace OpenXcom
{
class HdImageCache;
struct HdCanvasRenderStats
{
    // Working storage only; excludes source PNG cache and presentation target.
    std::size_t peakWorkingPixels = 0;
};

// Own HD rasterizer: its only image inputs are the resolved HD resource cache.
// The destination is a physical-resolution RGBA presentation target. The same
// scene can later be submitted to a GPU renderer without changing widgets.
class HdCanvasRenderer
{
public:
    static void render(const HdCanvas &canvas, HdImageCache &images,
        SDL_Surface *destination, HdCanvasTransform transform = {}, HdCanvasRenderStats *stats = nullptr);
};
}
