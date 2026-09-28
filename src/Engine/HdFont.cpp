#include "HdFont.h"
#include "HdImage.h"
#include "HdPngResolver.h"
#include "Exception.h"
#include <algorithm>
#include <iomanip>
#include <sstream>

namespace OpenXcom
{
namespace
{
std::string unicodeName(std::uint32_t codepoint)
{
    std::ostringstream out;
    out << 'U' << std::uppercase << std::hex << std::setw(4) << std::setfill('0') << codepoint;
    return out.str();
}
}

HdFontFace::HdFontFace(std::string family, int width, int height, int spacing, bool monospace)
    : _family(std::move(family)), _width(width), _height(height), _spacing(spacing), _monospace(monospace)
{
    // FontSmall uses spacing=-1. It is kerning, not an invalid dimension.
    if (_family.empty() || width <= 0 || height <= 0 ||
        static_cast<std::int64_t>(width) + spacing <= 0 || static_cast<std::int64_t>(height) + spacing <= 0)
        throw Exception("[HD FONT ERROR] Invalid font definition");
}

void HdFontFace::define(std::uint32_t codepoint, int sourceIndex)
{
    if (codepoint > 0x10ffff || (codepoint >= 0xd800 && codepoint <= 0xdfff) || sourceIndex < 0)
        throw Exception("[HD FONT ERROR] Invalid glyph identity");
    _definitions[codepoint] = sourceIndex;
    _glyphs.erase(codepoint);
}

bool HdFontFace::defines(std::uint32_t codepoint) const
{
    return _definitions.find(codepoint) != _definitions.end();
}

const HdGlyph &HdFontFace::glyph(std::uint32_t codepoint, HdImageCache &images)
{
    auto definition = _definitions.find(codepoint);
    if (definition == _definitions.end())
    {
        // Conventional unknown-character substitution is semantic. A PNG that
        // is missing for a defined glyph does NOT take this branch.
        definition = _definitions.find('?');
        if (definition == _definitions.end())
            throw Exception("[HD FONT ERROR] Undefined glyph " + _family + "/" + unicodeName(codepoint));
        codepoint = '?';
    }
    const auto found = _glyphs.find(codepoint);
    if (found != _glyphs.end()) return found->second;

    const std::string name = unicodeName(codepoint);
    const std::string indexedName = hdPngFrameNumber(definition->second, 4) + "_" + name + ".png";
    const HdAssetKey key{HdAssetDomain::Font, _family, static_cast<int>(codepoint)};
    HdGlyph result;
    result.asset = images.resolve(key, {
        {HdAssetProvider::RealHd, "Resources/TFTD_HD/RealHD/Fonts/" + _family + "/" + name + ".png", 16},
        {HdAssetProvider::RealHd, "Resources/TFTD_HD/RealHD/Fonts/" + _family + "/" + indexedName, 16},
        {HdAssetProvider::Remastered, "Resources/TFTD_HD/Fonts/" + _family + "/" + name + ".png", 16},
        {HdAssetProvider::Remastered, "Resources/TFTD_HD/Fonts/" + _family + "/" + indexedName, 16},
        {HdAssetProvider::LegacyHd, "Resources/TFTD_HD/LegacyIndexed/Fonts/" + _family + "/" + indexedName, 16},
        {HdAssetProvider::LegacyHd, "Bibliotheque/Polices/" + _family + "/" + indexedName, 16}});
    if (!result.asset)
        throw Exception("[HD FONT ERROR] Missing or invalid HD glyph " + _family + "/" + name);
    const HdImage &image = images.require(result.asset.asset.path);
    result.paletteIndexed = image.paletteIndexed8;
    const unsigned scale = static_cast<unsigned>(result.asset.asset.nativeScale);
    if (image.width != static_cast<unsigned>(_width) * scale ||
        image.height != static_cast<unsigned>(_height) * scale ||
        image.rgba.size() / 4 != static_cast<size_t>(image.width) * image.height)
        throw Exception("[HD FONT ERROR] Glyph dimensions do not match its declared cell: " + result.asset.asset.path);

    unsigned left = image.width, right = 0;
    bool ink = false;
    for (unsigned y = 0; y < image.height; ++y)
        for (unsigned x = 0; x < image.width; ++x)
            if (image.rgba[(static_cast<size_t>(y) * image.width + x) * 4 + 3] != 0)
            {
                ink = true;
                left = std::min(left, x);
                right = std::max(right, x + 1);
            }
    result.empty = !ink;
    const unsigned cellLeft = !_monospace && ink ? left / scale : 0;
    const unsigned cellRight = _monospace ? static_cast<unsigned>(_width) :
        (ink ? (right + scale - 1) / scale : 1);
    result.width = cellRight - cellLeft;
    result.height = _height;
    result.advance = result.width + _spacing;
    result.lineAdvance = result.height + _spacing;
    if (result.advance < 0.0)
        throw Exception("[HD FONT ERROR] Negative glyph advance: " + result.asset.asset.path);
    result.source = {double(cellLeft * scale) / image.width, 0.0,
        double((cellRight - cellLeft) * scale) / image.width, 1.0};
    return _glyphs.emplace(codepoint, std::move(result)).first->second;
}

double HdFontFace::append(HdCanvas &canvas, HdImageCache &images, std::uint32_t codepoint,
    double x, double y, const HdTextColor &color)
{
    const HdGlyph &g = glyph(codepoint, images);
    if (g.empty) return g.advance;
    std::shared_ptr<std::array<HdRgba, 256>> palette;
    HdImageStyle treatment;
    if (g.asset.asset.provider == HdAssetProvider::LegacyHd && g.paletteIndexed)
    {
        palette = std::make_shared<std::array<HdRgba, 256>>();
        for (int i = 0; i < 256; ++i)
        {
            const int invert = color.inverseMid ? 2 * (color.inverseMid - i) : 0;
            const auto index = static_cast<std::uint8_t>(color.offset + i * color.multiplier + invert);
            (*palette)[i] = color.palette[index];
        }
        (*palette)[0].a = 0;
    }
    else if (g.asset.asset.provider == HdAssetProvider::LegacyHd)
    {
        // A true RGBA transition glyph is an intensity/alpha mask, not an
        // index buffer. Preserve alpha and shading, apply the semantic text ink.
        const int invert = color.inverseMid ? 2 * (color.inverseMid - 1) : 0;
        treatment.tint = true;
        treatment.tintColor = color.palette[static_cast<std::uint8_t>(color.offset + color.multiplier + invert)];
    }
    // Authored glyph colours are not interpreted as Legacy indices merely
    // because an artist saved their PNG in palette format.
    canvas.image(g.asset, {x, y, g.width, g.height}, g.source, 1.0, palette, false, false, treatment);
    return g.advance;
}
}
