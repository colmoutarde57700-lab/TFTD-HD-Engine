#pragma once
#include "HdUiImage.h"
#include "HdUiPalette.h"

namespace OpenXcom
{
// A resource-backed interface picture, before asset resolution. This holds
// identities, layout and style indices only: no Surface, pixels or callbacks.
// Snapshot crops let controls reuse a panel without copying its old raster.
class HdUiPicture
{
    enum class Kind { Image, Fill, Child };
    struct Part
    {
        Kind kind = Kind::Image;
        HdUiImageDefinition image;
        HdRect source, destination;
        std::uint8_t color = 0;
        std::shared_ptr<const HdUiPicture> child;
        HdCanvasTransform transform;
        HdImageStyle style;
        std::shared_ptr<const HdUiIndexMap> paletteMap;
    };
    double _width, _height;
    std::vector<Part> _parts;
public:
    HdUiPicture(double width, double height) : _width(width), _height(height)
    {
        if (!HdRect{0, 0, width, height}.finite() || width < 0 || height < 0)
            throw std::invalid_argument("Invalid HD UI picture extent");
    }
    HdRect bounds() const { return {0, 0, _width, _height}; }
    void image(const HdUiImageDefinition &definition, HdRect source, HdRect destination,
        HdImageStyle style = {}, std::shared_ptr<const HdUiIndexMap> paletteMap = {})
    {
        if (!source.finite() || !destination.finite() || source.w < 0 || source.h < 0 ||
            destination.w < 0 || destination.h < 0)
            throw std::invalid_argument("Invalid HD UI picture crop");
        Part part;
        if (!style.valid()) throw std::invalid_argument("Invalid HD picture image treatment");
        part.style = style; part.paletteMap = std::move(paletteMap);
        part.image = definition; part.source = source; part.destination = destination;
        _parts.push_back(std::move(part));
    }
    void fill(HdRect rectangle, std::uint8_t color)
    {
        if (!rectangle.finite() || rectangle.w < 0 || rectangle.h < 0)
            throw std::invalid_argument("Invalid HD UI picture fill");
        Part part;
        part.kind = Kind::Fill; part.destination = rectangle; part.color = color;
        _parts.push_back(std::move(part));
    }
    void composite(const HdUiPicture &picture, HdCanvasTransform transform = {})
    {
        if (!transform.valid()) throw std::invalid_argument("Invalid HD UI picture transform");
        Part part;
        part.kind = Kind::Child; part.transform = transform;
        part.child = std::make_shared<const HdUiPicture>(picture);
        _parts.push_back(std::move(part));
    }
    HdUiPicture cropped(HdRect region) const
    {
        if (!region.finite() || region.w < 0 || region.h < 0)
            throw std::invalid_argument("Invalid HD UI picture region");
        HdUiPicture result(region.w, region.h);
        result.composite(*this, {-region.x, -region.y, 1, 1});
        return result;
    }
    HdCanvas makeCanvas(HdImageCache &images,
        std::shared_ptr<const std::array<HdRgba, 256>> palette) const
    {
        if (!palette) throw std::invalid_argument("HD UI picture requires explicit style colors");
        HdCanvas content(_width, _height);
        for (const auto &part : _parts)
        {
            if (part.kind == Kind::Image)
            {
                auto colors = palette;
                if (part.paletteMap)
                {
                    auto mapped = std::make_shared<std::array<HdRgba, 256>>();
                    for (size_t i = 0; i < 256; ++i) (*mapped)[i] = (*palette)[(*part.paletteMap)[i]];
                    colors = mapped;
                }
                hdAppendUiImage(content, images, part.image, part.source, part.destination, colors, false, false, part.style);
            }
            else if (part.kind == Kind::Fill)
                content.sourceRectangle(part.destination, (*palette)[part.color]);
            else
            {
                content.composite(part.child->makeCanvas(images, palette), part.transform);
            }
        }
        return content;
    }
    void compose(HdCanvas &canvas, HdImageCache &images,
        std::shared_ptr<const std::array<HdRgba, 256>> palette) const
    {
        canvas.composite(makeCanvas(images, std::move(palette)));
    }
};
}
