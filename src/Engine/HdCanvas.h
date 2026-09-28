#pragma once

#include "HdAssetContract.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace OpenXcom
{

struct HdPoint
{
    double x = 0.0;
    double y = 0.0;
};

struct HdRect
{
    double x = 0.0;
    double y = 0.0;
    double w = 0.0;
    double h = 0.0;

    bool empty() const { return w <= 0.0 || h <= 0.0; }
    bool finite() const
    {
        return std::isfinite(x) && std::isfinite(y) && std::isfinite(w) && std::isfinite(h) &&
            std::isfinite(x + w) && std::isfinite(y + h);
    }
};

inline HdRect hdIntersect(HdRect a, HdRect b)
{
    const double x = std::max(a.x, b.x), y = std::max(a.y, b.y);
    return {x, y, std::max(0.0, std::min(a.x + a.w, b.x + b.w) - x),
        std::max(0.0, std::min(a.y + a.h, b.y + b.h) - y)};
}

struct HdRgba
{
    std::uint8_t r = 0, g = 0, b = 0, a = 255;
};

// Explicit RGBA image treatment; independent of indexed sprite shaders.
// Tint uses maximum RGB as intensity, or uniform intensity for an explicit
// flat silhouette tint. Both preserve source alpha.
struct HdImageStyle
{
    double light = 1.0;
    bool tint = false;
    bool flatTint = false;
    HdRgba tintColor{255, 255, 255, 255};
    bool valid() const { return std::isfinite(light) && light >= 0.0 && light <= 1.0; }
};

// Interface coordinates are independent of the physical output resolution.
// This transform is layout only; world geometry has a separate depth contract.
struct HdCanvasTransform
{
    double x = 0.0, y = 0.0;
    double sx = 1.0, sy = 1.0;

    HdPoint point(HdPoint p) const { return {x + p.x * sx, y + p.y * sy}; }
    HdRect rect(HdRect r) const { return {x + r.x * sx, y + r.y * sy, r.w * sx, r.h * sy}; }
    HdCanvasTransform child(const HdCanvasTransform &c) const
    {
        return {x + c.x * sx, y + c.y * sy, sx * c.sx, sy * c.sy};
    }
    bool valid() const
    {
        return std::isfinite(x) && std::isfinite(y) && std::isfinite(sx) &&
            std::isfinite(sy) && sx > 0.0 && sy > 0.0;
    }
};

class HdCanvas;

enum class HdCanvasOp { Rectangle, SourceRectangle, Line, Ellipse, Polygon, Image, Layer };

struct HdCanvasCommand
{
    HdCanvasOp op = HdCanvasOp::Rectangle;
    HdRect bounds;
    HdRgba color;
    std::vector<HdPoint> points;
    double lineWidth = 1.0;
    HdAssetCandidate image;
    HdAssetKey key;
    // Only explicitly indexed transition images may use an active palette.
    // Authored RGBA art keeps its own colours unless an explicit imageStyle
    // requests a semantic treatment. Font adapters can provide a
    // recoloured palette without passing a low-resolution glyph raster.
    std::shared_ptr<const std::array<HdRgba, 256>> palette;
    // Normalized source crop. The source remains the original HD image;
    // cropping never first reduces it to an old logical-resolution bitmap.
    HdRect source{0.0, 0.0, 1.0, 1.0};
    bool flipX = false, flipY = false;
    HdImageStyle imageStyle;
    std::shared_ptr<const HdCanvas> layer;
    HdCanvasTransform transform;
    double opacity = 1.0;
};

// A retained display list. No source pixel buffer, Surface, SDL_Surface,
// renderer callback or old framebuffer can be attached to a command.
// Child lists are snapshots: redrawing a widget cannot mutate a frame already
// submitted for presentation. All nodes are clipped by their own canvas.
class HdCanvas
{
    HdRect _bounds;
    std::vector<HdCanvasCommand> _commands;

    static void validateRect(HdRect rect)
    {
        if (!rect.finite() || rect.w < 0.0 || rect.h < 0.0)
            throw std::invalid_argument("Invalid HD canvas rectangle");
    }
    static void validateOpacity(double opacity)
    {
        if (!std::isfinite(opacity) || opacity < 0.0 || opacity > 1.0)
            throw std::invalid_argument("Invalid HD canvas opacity");
    }
public:
    HdCanvas(double width, double height) : _bounds{0.0, 0.0, width, height}
    {
        validateRect(_bounds);
    }
    HdRect bounds() const { return _bounds; }
    const std::vector<HdCanvasCommand> &commands() const { return _commands; }
    void clear() { _commands.clear(); }
    void rectangle(HdRect bounds, HdRgba color)
    {
        validateRect(bounds);
        if (bounds.empty() || color.a == 0) return;
        HdCanvasCommand c;
        c.bounds = bounds; c.color = color;
        _commands.push_back(std::move(c));
    }
    // Replace only this canvas's own content, including with transparency.
    // A canvas containing this operation must be isolated before compositing:
    // clearing a button must reveal the parent, never erase the parent itself.
    void sourceRectangle(HdRect bounds, HdRgba color)
    {
        validateRect(bounds);
        if (bounds.empty()) return;
        if (color.a == 255) { rectangle(bounds, color); return; }
        HdCanvasCommand c;
        c.op = HdCanvasOp::SourceRectangle; c.bounds = bounds; c.color = color;
        _commands.push_back(std::move(c));
    }
    bool requiresIsolation() const
    {
        return std::any_of(_commands.begin(), _commands.end(), [](const HdCanvasCommand &c)
            { return c.op == HdCanvasOp::SourceRectangle; });
    }
    void ellipse(HdRect bounds, HdRgba color)
    {
        validateRect(bounds);
        if (bounds.empty() || color.a == 0) return;
        HdCanvasCommand c;
        c.op = HdCanvasOp::Ellipse; c.bounds = bounds; c.color = color;
        _commands.push_back(std::move(c));
    }
    void line(HdPoint from, HdPoint to, double width, HdRgba color)
    {
        if (!std::isfinite(from.x) || !std::isfinite(from.y) ||
            !std::isfinite(to.x) || !std::isfinite(to.y) ||
            !std::isfinite(width) || width <= 0.0)
            throw std::invalid_argument("Invalid HD canvas line");
        if (color.a == 0) return;
        HdCanvasCommand c;
        c.op = HdCanvasOp::Line; c.points = {from, to};
        c.lineWidth = width; c.color = color;
        _commands.push_back(std::move(c));
    }
    void polygon(const std::vector<HdPoint> &points, HdRgba color)
    {
        if (points.size() < 3 || color.a == 0) return;
        for (auto point : points)
            if (!std::isfinite(point.x) || !std::isfinite(point.y))
                throw std::invalid_argument("Invalid HD canvas polygon");
        HdCanvasCommand c;
        c.op = HdCanvasOp::Polygon; c.points = points; c.color = color;
        _commands.push_back(std::move(c));
    }
    void image(const HdAssetResolution &resolved, HdRect destination,
        HdRect source = {0.0, 0.0, 1.0, 1.0}, double opacity = 1.0,
        std::shared_ptr<const std::array<HdRgba, 256>> palette = {}, bool flipX = false, bool flipY = false,
        HdImageStyle style = {})
    {
        validateRect(destination); validateRect(source); validateOpacity(opacity);
        if (!style.valid()) throw std::invalid_argument("Invalid HD image treatment");
        if (!resolved || resolved.asset.path.empty() || resolved.asset.nativeScale <= 0)
            throw std::invalid_argument("Unresolved HD canvas image");
        if (source.x < 0.0 || source.y < 0.0 || source.x + source.w > 1.0 || source.y + source.h > 1.0)
            throw std::invalid_argument("HD canvas image crop outside source");
        if (destination.empty() || source.empty() || opacity == 0.0) return;
        HdCanvasCommand c;
        c.op = HdCanvasOp::Image; c.image = resolved.asset; c.key = resolved.key;
        c.bounds = destination; c.source = source; c.opacity = opacity;
        c.flipX = flipX; c.flipY = flipY;
        c.imageStyle = style;
        c.palette = std::move(palette);
        _commands.push_back(std::move(c));
    }
    void composite(const HdCanvas &canvas, HdCanvasTransform transform = {}, double opacity = 1.0)
    {
        if (!transform.valid()) throw std::invalid_argument("Invalid HD canvas transform");
        validateOpacity(opacity);
        if (canvas.bounds().empty() || canvas.commands().empty() || opacity == 0.0) return;
        HdCanvasCommand c;
        c.op = HdCanvasOp::Layer; c.transform = transform; c.opacity = opacity;
        c.layer = std::make_shared<const HdCanvas>(canvas);
        _commands.push_back(std::move(c));
    }
};

// Traversal delivers commands in explicit HD order and a physical clip. It
// never derives occlusion from changed pixels in another renderer's canvas.
// Group opacity is deliberately retained in the tree, not flattened into each
// child's alpha (which would blend overlapping children incorrectly).
template<typename Visitor>
void hdVisitCanvas(const HdCanvas &canvas, const HdCanvasTransform &transform,
    HdRect parentClip, Visitor &visitor)
{
    if (!transform.valid() || !parentClip.finite())
        throw std::invalid_argument("Invalid HD canvas traversal");
    const HdRect transformedBounds = transform.rect(canvas.bounds());
    if (!transformedBounds.finite()) throw std::invalid_argument("HD canvas transform overflow");
    const HdRect clip = hdIntersect(parentClip, transformedBounds);
    if (clip.empty()) return;
    visitor.beginCanvas(canvas, transform, clip);
    for (const auto &command : canvas.commands())
    {
        if (command.op == HdCanvasOp::Layer)
        {
            const auto childTransform = transform.child(command.transform);
            const auto childBounds = childTransform.rect(command.layer->bounds());
            if (!childTransform.valid() || !childBounds.finite())
                throw std::invalid_argument("HD child canvas transform overflow");
            const auto childClip = hdIntersect(clip, childBounds);
            if (childClip.empty()) continue;
            visitor.beginLayer(command.opacity, childClip);
            hdVisitCanvas(*command.layer, childTransform, childClip, visitor);
            visitor.endLayer(command.opacity);
        }
        else visitor.draw(command, transform, clip);
    }
    visitor.endCanvas();
}

}
