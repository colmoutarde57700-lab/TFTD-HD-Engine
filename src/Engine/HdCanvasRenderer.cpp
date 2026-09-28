#include "HdCanvasRenderer.h"
#include "HdImage.h"
#include "Exception.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace OpenXcom
{
namespace
{
// Premultiplied working pixels allow correct alpha for overlapping primitives
// inside a translucent widget. No intermediate logical-resolution image exists.
struct Pixel
{
    float r = 0, g = 0, b = 0, a = 0;
};

void over(Pixel &destination, Pixel source, double opacity = 1.0)
{
    const float alpha = source.a * static_cast<float>(opacity);
    const float keep = 1.0f - alpha;
    destination.r = source.r * static_cast<float>(opacity) + destination.r * keep;
    destination.g = source.g * static_cast<float>(opacity) + destination.g * keep;
    destination.b = source.b * static_cast<float>(opacity) + destination.b * keep;
    destination.a = alpha + destination.a * keep;
}

Pixel pixel(HdRgba color)
{
    const float alpha = color.a / 255.0f;
    return {color.r / 255.0f * alpha, color.g / 255.0f * alpha,
        color.b / 255.0f * alpha, alpha};
}

class RasterVisitor
{
    struct Layer
    {
        int x, y, width, height;
        std::vector<Pixel> pixels;
        explicit Layer(HdRect region)
            : x(static_cast<int>(std::floor(region.x))), y(static_cast<int>(std::floor(region.y))),
              width(static_cast<int>(std::ceil(region.x + region.w)) - x),
              height(static_cast<int>(std::ceil(region.y + region.h)) - y),
              pixels(static_cast<size_t>(width) * height) {}
        HdRect bounds() const { return {double(x), double(y), double(width), double(height)}; }
        Pixel &at(int px, int py) { return pixels[static_cast<size_t>(py - y) * width + px - x]; }
        const Pixel &at(int px, int py) const { return pixels[static_cast<size_t>(py - y) * width + px - x]; }
    };
    int _width, _height;
    HdImageCache &_images;
    std::vector<Layer> _layers;
    std::vector<bool> _isolatedCanvases;
    size_t _workingPixels = 0, _peakWorkingPixels = 0;
    void pushLayer(HdRect clip)
    {
        _layers.emplace_back(hdIntersect(clip, {0, 0, double(_width), double(_height)}));
        _workingPixels += _layers.back().pixels.size();
        _peakWorkingPixels = std::max(_peakWorkingPixels, _workingPixels);
    }
public:
    RasterVisitor(int width, int height, HdImageCache &images)
        : _width(width), _height(height), _images(images)
    {
        pushLayer({0, 0, double(width), double(height)});
    }

    void beginCanvas(const HdCanvas &canvas, const HdCanvasTransform &, HdRect clip)
    {
        const bool isolated = canvas.requiresIsolation();
        _isolatedCanvases.push_back(isolated);
        if (isolated) pushLayer(clip);
    }
    void endCanvas()
    {
        if (_isolatedCanvases.back()) compositeLayer(1.0);
        _isolatedCanvases.pop_back();
    }
    void beginLayer(double opacity, HdRect clip)
    {
        if (opacity < 1.0) pushLayer(clip);
    }
    void endLayer(double opacity)
    {
        if (opacity >= 1.0) return;
        compositeLayer(opacity);
    }
    void compositeLayer(double opacity)
    {
        const auto &source = _layers.back();
        auto &target = _layers[_layers.size() - 2];
        const int x0 = std::max(source.x, target.x), y0 = std::max(source.y, target.y);
        const int x1 = std::min(source.x + source.width, target.x + target.width);
        const int y1 = std::min(source.y + source.height, target.y + target.height);
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x)
                if (source.at(x, y).a > 0.0f) over(target.at(x, y), source.at(x, y), opacity);
        _workingPixels -= source.pixels.size();
        _layers.pop_back();
    }

    template<typename Sample>
    void raster(HdRect bounds, HdRect clip, const Sample &sample, bool replace = false)
    {
        if (!bounds.finite() || !clip.finite())
            throw Exception("[HD RENDER ERROR] Non-finite physical canvas bounds");
        bounds = hdIntersect(hdIntersect(bounds, clip), _layers.back().bounds());
        if (bounds.empty()) return;
        const int x0 = static_cast<int>(std::floor(bounds.x));
        const int y0 = static_cast<int>(std::floor(bounds.y));
        const int x1 = static_cast<int>(std::ceil(bounds.x + bounds.w));
        const int y1 = static_cast<int>(std::ceil(bounds.y + bounds.h));
        auto &target = _layers.back();
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x)
            {
                const double px = x + 0.5, py = y + 0.5;
                if (px < bounds.x || px >= bounds.x + bounds.w ||
                    py < bounds.y || py >= bounds.y + bounds.h) continue;
                auto &destination = target.at(x, y);
                if (replace) destination = sample(px, py);
                else over(destination, sample(px, py));
            }
    }

    void draw(const HdCanvasCommand &c, const HdCanvasTransform &t, HdRect clip)
    {
        const Pixel color = pixel(c.color);
        const auto inverse = [&](double x, double y) { return HdPoint{(x - t.x) / t.sx, (y - t.y) / t.sy}; };
        if (c.op == HdCanvasOp::Rectangle)
            raster(t.rect(c.bounds), clip, [&](double, double) { return color; });
        else if (c.op == HdCanvasOp::SourceRectangle)
            raster(t.rect(c.bounds), clip, [&](double, double) { return color; }, true);
        else if (c.op == HdCanvasOp::Ellipse)
            raster(t.rect(c.bounds), clip, [&](double x, double y)
            {
                const auto p = inverse(x, y);
                const double dx = (p.x - c.bounds.x) / c.bounds.w * 2.0 - 1.0;
                const double dy = (p.y - c.bounds.y) / c.bounds.h * 2.0 - 1.0;
                return dx * dx + dy * dy <= 1.0 ? color : Pixel{};
            });
        else if (c.op == HdCanvasOp::Line)
        {
            const HdPoint a = c.points[0], b = c.points[1];
            const double radius = c.lineWidth * 0.5;
            const double dx = b.x - a.x, dy = b.y - a.y, length2 = dx * dx + dy * dy;
            const HdRect bounds{std::min(a.x, b.x) - radius, std::min(a.y, b.y) - radius,
                std::abs(dx) + 2 * radius, std::abs(dy) + 2 * radius};
            raster(t.rect(bounds), clip, [&](double x, double y)
            {
                const auto p = inverse(x, y);
                const double along = length2 > 0.0 ? std::max(0.0, std::min(1.0,
                    ((p.x - a.x) * dx + (p.y - a.y) * dy) / length2)) : 0.0;
                const double ex = p.x - a.x - along * dx, ey = p.y - a.y - along * dy;
                return ex * ex + ey * ey <= radius * radius ? color : Pixel{};
            });
        }
        else if (c.op == HdCanvasOp::Polygon)
        {
            double minX = c.points[0].x, maxX = minX, minY = c.points[0].y, maxY = minY;
            for (auto p : c.points)
            {
                minX = std::min(minX, p.x); maxX = std::max(maxX, p.x);
                minY = std::min(minY, p.y); maxY = std::max(maxY, p.y);
            }
            raster(t.rect({minX, minY, maxX - minX, maxY - minY}), clip, [&](double x, double y)
            {
                const auto p = inverse(x, y);
                bool inside = false;
                for (size_t i = 0, j = c.points.size() - 1; i < c.points.size(); j = i++)
                {
                    const auto a = c.points[i], b = c.points[j];
                    if ((a.y > p.y) != (b.y > p.y) &&
                        p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x)
                        inside = !inside;
                }
                return inside ? color : Pixel{};
            });
        }
        else if (c.op == HdCanvasOp::Image)
        {
            const HdImage &image = _images.require(c.image.path);
            if (!image.width || !image.height ||
                image.rgba.size() != static_cast<size_t>(image.width) * image.height * 4)
                throw Exception("[HD RESOURCE ERROR] Invalid decoded HD image dimensions: " + c.image.path);
            if (c.palette && !image.paletteIndexed8)
                throw Exception("[HD RESOURCE ERROR] Indexed palette requested for RGBA image: " + c.image.path);
            if (c.palette && image.indices.size() != static_cast<size_t>(image.width) * image.height)
                throw Exception("[HD RESOURCE ERROR] Invalid normalized PNG index buffer: " + c.image.path);
            raster(t.rect(c.bounds), clip, [&](double x, double y)
            {
                const auto p = inverse(x, y);
                const double localU = (p.x - c.bounds.x) / c.bounds.w;
                const double localV = (p.y - c.bounds.y) / c.bounds.h;
                const double u = c.source.x + (c.flipX ? 1.0 - localU : localU) * c.source.w;
                const double v = c.source.y + (c.flipY ? 1.0 - localV : localV) * c.source.h;
                const unsigned ix = std::min(image.width - 1, static_cast<unsigned>(std::max(0.0, u * image.width)));
                const unsigned iy = std::min(image.height - 1, static_cast<unsigned>(std::max(0.0, v * image.height)));
                const size_t index = static_cast<size_t>(iy) * image.width + ix;
                HdRgba rgba;
                if (c.palette)
                {
                    rgba = (*c.palette)[image.indices[index]];
                    // Retain the PNG transparency as well as explicit palette alpha.
                    rgba.a = static_cast<std::uint8_t>(unsigned(rgba.a) * image.rgba[index * 4 + 3] / 255);
                }
                else rgba = {image.rgba[index * 4], image.rgba[index * 4 + 1],
                    image.rgba[index * 4 + 2], image.rgba[index * 4 + 3]};
                const auto &style = c.imageStyle;
                const double intensity = style.flatTint ? 1.0 : std::max({rgba.r, rgba.g, rgba.b}) / 255.0;
                const auto channel = [&](std::uint8_t source, std::uint8_t tint)
                {
                    return static_cast<std::uint8_t>(std::round((style.tint ? intensity * tint : source) * style.light));
                };
                rgba.r = channel(rgba.r, style.tintColor.r);
                rgba.g = channel(rgba.g, style.tintColor.g);
                rgba.b = channel(rgba.b, style.tintColor.b);
                Pixel result = pixel(rgba);
                const float opacity = static_cast<float>(c.opacity);
                result.r *= opacity; result.g *= opacity; result.b *= opacity; result.a *= opacity;
                return result;
            });
        }
        else throw Exception("[HD RENDER ERROR] Unsupported canvas operation");
    }

    void present(SDL_Surface *destination) const
    {
        if (SDL_MUSTLOCK(destination) && SDL_LockSurface(destination) != 0)
            throw Exception("[HD RENDER ERROR] Cannot lock presentation target");
        const auto byte = [](float value) { return static_cast<Uint8>(std::max(0.0f, std::min(255.0f, std::round(value * 255.0f)))); };
        for (int y = 0; y < _height; ++y)
        {
            auto *row = reinterpret_cast<Uint32 *>(static_cast<unsigned char *>(destination->pixels) + y * destination->pitch);
            for (int x = 0; x < _width; ++x)
            {
                const Pixel source = _layers[0].at(x, y);
                if (source.a == 0.0f) continue;
                Uint8 r, g, b, a;
                SDL_GetRGBA(row[x], destination->format, &r, &g, &b, &a);
                Pixel target = pixel({r, g, b, a});
                over(target, source);
                row[x] = SDL_MapRGBA(destination->format, byte(target.r / target.a),
                    byte(target.g / target.a), byte(target.b / target.a), byte(target.a));
            }
        }
        if (SDL_MUSTLOCK(destination)) SDL_UnlockSurface(destination);
    }
    size_t peakWorkingPixels() const { return _peakWorkingPixels; }
};
}

void HdCanvasRenderer::render(const HdCanvas &canvas, HdImageCache &images,
    SDL_Surface *destination, HdCanvasTransform transform, HdCanvasRenderStats *stats)
{
    if (!destination || !destination->format || destination->format->BytesPerPixel != 4 ||
        destination->w <= 0 || destination->h <= 0)
        throw Exception("[HD RENDER ERROR] A 32-bit physical presentation target is required");
    RasterVisitor visitor(destination->w, destination->h, images);
    const auto &clip = destination->clip_rect;
    hdVisitCanvas(canvas, transform, {double(clip.x), double(clip.y), double(clip.w), double(clip.h)}, visitor);
    visitor.present(destination);
    if (stats) stats->peakWorkingPixels = visitor.peakWorkingPixels();
}
}
