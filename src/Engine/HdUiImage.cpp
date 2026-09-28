#include "HdUiImage.h"
#include "HdImage.h"
#include "HdUiScreenCatalog.h"
#include "HdUiFrameCatalog.h"
#include "Exception.h"
#include <algorithm>

namespace OpenXcom
{
HdUiImageDefinition hdUiFrameDefinition(const std::string &family, int frame, int width, int height)
{
    if (family.empty() || frame < 0 || width <= 0 || height <= 0)
        throw Exception("[HD UI ERROR] Invalid frame identity: " + family);
    std::string canonical = family;
    for (char &c : canonical) if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
    const auto number = std::to_string(frame);
    const auto file = std::string(number.size() < 4 ? 4 - number.size() : 0, '0') + number + ".png";
    const auto shortFile = std::string(number.size() < 3 ? 3 - number.size() : 0, '0') + number + ".png";
    HdUiImageDefinition definition{{HdAssetDomain::Interface, canonical, frame}, double(width), double(height), {
        {HdAssetProvider::RealHd, "Resources/TFTD_HD/RealHD/UI/Frames/" + canonical + "/" + file, 16},
        {HdAssetProvider::RealHd, "Resources/TFTD_HD/RealHD/SurfaceSets/" + canonical + "/" + file, 16},
        {HdAssetProvider::RealHd, "Resources/TFTD_HD/RealHD/SurfaceSets/" + canonical + "/" + shortFile, 16},
        {HdAssetProvider::Remastered, "Resources/TFTD_HD/UI/Frames/" + canonical + "/" + file, 16},
        {HdAssetProvider::Remastered, "Resources/TFTD_HD/SurfaceSets/" + canonical + "/" + file, 16},
        {HdAssetProvider::Remastered, "Resources/TFTD_HD/SurfaceSets/" + canonical + "/" + shortFile, 16},
        {HdAssetProvider::LegacyHd, "Resources/TFTD_HD/LegacyIndexed/SurfaceSets/" + canonical + "/" + file, 16}}};
    if (shortFile != file)
        definition.candidates.push_back({HdAssetProvider::LegacyHd,
            "Resources/TFTD_HD/LegacyIndexed/SurfaceSets/" + canonical + "/" + shortFile, 16});
    for (const auto &entry : hdUiFrameCatalog)
        if (canonical == entry.family && frame == entry.frame)
        {
            if (width != entry.width || height != entry.height)
            {
                // A mod can override dimensions. Do not abort state creation,
                // and do not stretch the unrelated standard transition image.
                definition.catalogNote = "catalog frame skipped: requested=" + std::to_string(width) + "x" +
                    std::to_string(height) + " standard=" + std::to_string(entry.width) + "x" + std::to_string(entry.height);
                break;
            }
            definition.candidates.push_back({HdAssetProvider::LegacyHd, entry.path, 16});
            break;
        }
    return definition;
}

HdUiImageDefinition hdUiWidgetDefinition(const std::string &id, int width, int height)
{
    if (id.empty() || width <= 0 || height <= 0)
        throw Exception("[HD UI ERROR] Invalid widget image definition: " + id);
    return {{HdAssetDomain::Interface, id, 0}, double(width), double(height), {
        {HdAssetProvider::RealHd, "Resources/TFTD_HD/RealHD/UI/Widgets/" + id + ".png", 16},
        {HdAssetProvider::Remastered, "Resources/TFTD_HD/UI/Widgets/" + id + ".png", 16},
        {HdAssetProvider::LegacyHd, "Resources/TFTD_HD/LegacyIndexed/UI/Widgets/" + id + ".png", 16}}};
}

HdUiImageDefinition hdUiNaturalScreenDefinition(const std::string &id, HdImageCache &images)
{
    std::string canonical = id;
    for (char &c : canonical) if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
    for (const auto &entry : hdUiScreenCatalog)
        if (canonical == entry.id) return hdUiScreenDefinition(id, entry.width, entry.height);
    auto definition = hdUiScreenDefinition(id, 1, 1);
    const auto selected = images.resolve(definition.key, definition.candidates);
    if (!selected) throw Exception("[HD UI ERROR] Missing or invalid custom screen: " + id);
    const auto &image = images.require(selected.asset.path);
    const auto scale = selected.asset.nativeScale;
    if (!image.width || !image.height || image.width % scale || image.height % scale)
        throw Exception("[HD UI ERROR] Custom screen must declare a whole layout at PNG scale 16: " + id);
    definition.width = image.width / scale;
    definition.height = image.height / scale;
    return definition;
}

bool hdUiImageHitTest(HdImageCache &images, const HdUiImageDefinition &definition, HdPoint point)
{
    if (!std::isfinite(point.x) || !std::isfinite(point.y) ||
        definition.layout != HdUiImageLayout::Direct)
        throw Exception("[HD UI ERROR] Invalid direct image hit test");
    if (point.x < 0 || point.y < 0 || point.x >= definition.width || point.y >= definition.height) return false;
    HdCanvas picture(definition.width, definition.height);
    hdAppendUiImage(picture, images, definition, picture.bounds(), picture.bounds());
    if (picture.commands().size() != 1 || picture.commands()[0].op != HdCanvasOp::Image)
        throw Exception("[HD UI ERROR] Direct hit test did not resolve one HD image");
    const auto &image = images.require(picture.commands()[0].image.path);
    const auto x = std::min(image.width - 1, static_cast<unsigned>(point.x * image.width / definition.width));
    const auto y = std::min(image.height - 1, static_cast<unsigned>(point.y * image.height / definition.height));
    return image.rgba[(static_cast<size_t>(y) * image.width + x) * 4 + 3] != 0;
}

void hdAppendUiSlicedImage(HdCanvas &canvas, HdImageCache &images,
    const HdUiImageDefinition &definition, HdRect destination, HdUiBorders borders,
    std::shared_ptr<const std::array<HdRgba, 256>> transitionPalette)
{
    if (!destination.finite() || destination.w < 0 || destination.h < 0 ||
        !std::isfinite(borders.left) || !std::isfinite(borders.top) ||
        !std::isfinite(borders.right) || !std::isfinite(borders.bottom) ||
        borders.left < 0 || borders.top < 0 || borders.right < 0 || borders.bottom < 0 ||
        borders.left + borders.right >= definition.width || borders.top + borders.bottom >= definition.height ||
        borders.left + borders.right > destination.w || borders.top + borders.bottom > destination.h)
        throw Exception("[HD UI ERROR] Invalid sliced image bounds: " + definition.key.family);
    const double sx[] = {0, borders.left, definition.width - borders.right, definition.width};
    const double sy[] = {0, borders.top, definition.height - borders.bottom, definition.height};
    const double dx[] = {destination.x, destination.x + borders.left,
        destination.x + destination.w - borders.right, destination.x + destination.w};
    const double dy[] = {destination.y, destination.y + borders.top,
        destination.y + destination.h - borders.bottom, destination.y + destination.h};
    for (int y = 0; y < 3; ++y)
        for (int x = 0; x < 3; ++x)
            hdAppendUiImage(canvas, images, definition,
                {sx[x], sy[y], sx[x+1] - sx[x], sy[y+1] - sy[y]},
                {dx[x], dy[y], dx[x+1] - dx[x], dy[y+1] - dy[y]}, transitionPalette);
}

HdUiImageDefinition hdUiScreenDefinition(const std::string &id, int width, int height, bool manaLayout)
{
    if (id.empty() || width <= 0 || height <= 0)
        throw Exception("[HD UI ERROR] Invalid image identity or layout extent: " + id);
    std::string canonical = id;
    for (char &c : canonical) if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
    HdUiImageDefinition definition;
    definition.key = {HdAssetDomain::Interface, canonical, 0};
    definition.width = width; definition.height = height;
    if (canonical == "ALTGEOBORD.SCR") definition.layout = HdUiImageLayout::MirroredGeoscape;
    definition.candidates = {
        {HdAssetProvider::RealHd, "Resources/TFTD_HD/RealHD/UI/Backgrounds/" + id + ".png", 16},
        {HdAssetProvider::RealHd, "Resources/TFTD_HD/RealHD/UI/Images/" + id + ".png", 16},
        {HdAssetProvider::Remastered, "Resources/TFTD_HD/UI/Backgrounds/" + id + ".png", 16},
        {HdAssetProvider::Remastered, "Resources/TFTD_HD/UI/Images/" + id + ".png", 16},
        {HdAssetProvider::LegacyHd, "Resources/TFTD_HD/LegacyIndexed/UI/" + id + ".png", 16}};
    for (const auto &entry : hdUiScreenCatalog)
        if (canonical == entry.id)
        {
            if (width != entry.width || height != entry.height)
            {
                definition.catalogNote = "catalog screen skipped: requested=" + std::to_string(width) + "x" +
                    std::to_string(height) + " standard=" + std::to_string(entry.width) + "x" + std::to_string(entry.height);
                break;
            }
            if (canonical == "BACK06.SCR" || canonical == "UNIBORD.PCK")
                definition.candidates.push_back({HdAssetProvider::LegacyHd,
                    "Resources/TFTD_HD/LegacyIndexed/UI/Layouts/" + canonical +
                    (manaLayout ? ".mana_on.png" : ".mana_off.png"), 16});
            else definition.candidates.push_back({HdAssetProvider::LegacyHd, entry.path, 16});
            break;
        }
    return definition;
}

void hdAppendUiImage(HdCanvas &canvas, HdImageCache &images,
    const HdUiImageDefinition &definition, HdRect source, HdRect destination,
    std::shared_ptr<const std::array<HdRgba, 256>> transitionPalette, bool flipX, bool flipY, HdImageStyle authoredStyle)
{
    if (!authoredStyle.valid()) throw Exception("[HD UI ERROR] Invalid authored image treatment");
    if (!std::isfinite(definition.width) || !std::isfinite(definition.height) ||
        definition.width <= 0 || definition.height <= 0 ||
        !source.finite() || !destination.finite() || source.w < 0 || source.h < 0 ||
        destination.w < 0 || destination.h < 0)
        throw Exception("[HD UI ERROR] Invalid image crop or extent: " + definition.key.family);
    if (source.empty() || destination.empty()) return;
    const auto clipped = hdIntersect(source, {0, 0, definition.width, definition.height});
    if (clipped.empty()) return;
    const auto resolved = images.resolve(definition.key, definition.candidates);
    if (!resolved && resolved.failure != HdAssetFailure::InvalidRequest && definition.layout == HdUiImageLayout::MirroredGeoscape)
    {
        if (definition.width != 768 || definition.height != 600 || flipX || flipY)
            throw Exception("[HD UI ERROR] Invalid mirrored Geoscape layout request");
        // Reproduce the existing nine mirrored panels with one HD resource.
        // No 768x600 native raster, nor a 12288x9600 intermediate PNG, is made.
        const auto base = hdUiScreenDefinition("GEOBORD.SCR", 320, 200);
        const double scaleX = destination.w / source.w, scaleY = destination.h / source.h;
        for (int row = 0; row < 3; ++row)
            for (int column = 0; column < 3; ++column)
            {
                const HdRect tile{double(column * 256), double(row * 200), 256, 200};
                const auto visible = hdIntersect(clipped, tile);
                if (visible.empty()) continue;
                const bool mirrorX = column != 1, mirrorY = row != 1;
                const double x = visible.x - tile.x, y = visible.y - tile.y;
                hdAppendUiImage(canvas, images, base,
                    {mirrorX ? 256 - x - visible.w : x, mirrorY ? 200 - y - visible.h : y, visible.w, visible.h},
                    {destination.x + (visible.x - source.x) * scaleX,
                     destination.y + (visible.y - source.y) * scaleY, visible.w * scaleX, visible.h * scaleY},
                    transitionPalette, mirrorX, mirrorY, authoredStyle);
            }
        return;
    }
    if (!resolved)
        throw Exception("[HD UI ERROR] Missing or invalid HD image: " + definition.key.family +
            " selected=" + resolved.asset.path + " " + definition.catalogNote);
    const auto &image = images.require(resolved.asset.path);
    if (!image.width || !image.height || image.rgba.size() / 4 != size_t(image.width) * image.height)
        throw Exception("[HD UI ERROR] Invalid decoded image: " + resolved.asset.path);
    if (resolved.asset.provider == HdAssetProvider::LegacyHd)
    {
        if (image.width != definition.width * resolved.asset.nativeScale ||
            image.height != definition.height * resolved.asset.nativeScale)
            throw Exception("[HD UI ERROR] Transition image extent mismatch: " + resolved.asset.path);
        // RGBA transition exports carry their baked colours, not palette indices.
        if (!image.paletteIndexed8) transitionPalette.reset();
    }
    else transitionPalette.reset(); // Authored art never inherits index semantics.
    const double sx = destination.w / source.w, sy = destination.h / source.h;
    canvas.image(resolved,
        {destination.x + (flipX ? source.x + source.w - clipped.x - clipped.w : clipped.x - source.x) * sx,
         destination.y + (flipY ? source.y + source.h - clipped.y - clipped.h : clipped.y - source.y) * sy,
         clipped.w * sx, clipped.h * sy},
        {clipped.x / definition.width, clipped.y / definition.height,
         clipped.w / definition.width, clipped.h / definition.height}, 1.0, std::move(transitionPalette), flipX, flipY,
         resolved.asset.provider == HdAssetProvider::LegacyHd && image.paletteIndexed8 ? HdImageStyle{} : authoredStyle);
}
}
