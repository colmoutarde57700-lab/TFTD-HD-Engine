#pragma once
#include "HdCanvas.h"
#include <string>
#include <vector>

namespace OpenXcom
{
class HdImageCache;
enum class HdUiImageLayout { Direct, MirroredGeoscape };

// Semantic image definition. Its extent is layout metadata, never an old
// Surface's raster. The resource catalog supplies explicit transition paths.
struct HdUiImageDefinition
{
    HdAssetKey key;
    double width = 0, height = 0;
    std::vector<HdAssetCandidate> candidates;
    HdUiImageLayout layout = HdUiImageLayout::Direct;
    std::string catalogNote;
};

HdUiImageDefinition hdUiScreenDefinition(const std::string &id, int width, int height, bool manaLayout = false);
// Known screens use catalog layout; custom images explicitly use PNG pixels / 16.
HdUiImageDefinition hdUiNaturalScreenDefinition(const std::string &id, HdImageCache &images);
HdUiImageDefinition hdUiWidgetDefinition(const std::string &id, int width, int height);
HdUiImageDefinition hdUiFrameDefinition(const std::string &family, int frame, int width, int height);
// Hit testing follows the selected direct HD image, including authored alpha.
bool hdUiImageHitTest(HdImageCache &images, const HdUiImageDefinition &definition, HdPoint point);
struct HdUiBorders { double left = 0, top = 0, right = 0, bottom = 0; };
void hdAppendUiSlicedImage(HdCanvas &canvas, HdImageCache &images,
    const HdUiImageDefinition &definition, HdRect destination, HdUiBorders borders,
    std::shared_ptr<const std::array<HdRgba, 256>> transitionPalette = {});

// Clip in the source's logical coordinate system, then submit a normalized
// crop of the original HD image. Out-of-bounds source regions stay empty.
// authoredStyle applies only to authored art; transition art uses its supplied
// indexed palette. A semantic treatment must not be applied twice.
void hdAppendUiImage(HdCanvas &canvas, HdImageCache &images,
    const HdUiImageDefinition &definition, HdRect source, HdRect destination,
    std::shared_ptr<const std::array<HdRgba, 256>> transitionPalette = {},
    bool flipX = false, bool flipY = false, HdImageStyle authoredStyle = {});
}
