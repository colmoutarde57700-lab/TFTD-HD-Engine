#pragma once

#include "HdCanvas.h"
#include <cstdint>
#include <map>
#include <string>

namespace OpenXcom
{
class HdImageCache;

struct HdGlyph
{
    HdAssetResolution asset;
    HdRect source;
    double width = 0;
    double height = 0;
    double advance = 0;
    double lineAdvance = 0;
    bool empty = true;
    // Retained metadata: append must not decode the glyph after the metric
    // cache has released its temporary pixels (Font clears it per character).
    bool paletteIndexed = false;
};

struct HdTextColor
{
    std::array<HdRgba, 256> palette;
    int offset = 0;
    int multiplier = 1;
    int inverseMid = 0;
};

// A font face owns semantic glyph definitions and derives metrics from the HD
// resource itself. It never loads a low-resolution sheet or reads Surface pixels.
// Only requested glyphs are decoded; all language sheets need not be expanded
// into memory together. Recreate the face on mod/resource reload.
class HdFontFace
{
    std::string _family;
    int _width, _height, _spacing;
    bool _monospace;
    std::map<std::uint32_t, int> _definitions;
    std::map<std::uint32_t, HdGlyph> _glyphs;
public:
    HdFontFace(std::string family, int width, int height, int spacing, bool monospace);
    void define(std::uint32_t codepoint, int sourceIndex);
    bool defines(std::uint32_t codepoint) const;
    const HdGlyph &glyph(std::uint32_t codepoint, HdImageCache &images);
    double append(HdCanvas &canvas, HdImageCache &images, std::uint32_t codepoint,
        double x, double y, const HdTextColor &color);
};
}
