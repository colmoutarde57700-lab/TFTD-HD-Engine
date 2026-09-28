#pragma once
#include "HdCanvas.h"
#include <string>
#include <type_traits>

namespace OpenXcom
{
// Exact, process-local content signature; not a lossy hash or a dirty flag.
// Do not serialize struct padding, addresses, or shared_ptr identities.
class HdCanvasSignature
{
    std::string _value;
public:
    template<typename T> void scalar(T v)
    {
        static_assert(std::is_arithmetic<T>::value || std::is_enum<T>::value, "scalar required");
        _value.append(reinterpret_cast<const char*>(&v), sizeof(v));
    }
    void text(const std::string &s) { scalar(s.size()); _value.append(s); }
    void rect(HdRect r) { scalar(r.x); scalar(r.y); scalar(r.w); scalar(r.h); }
    void transform(HdCanvasTransform t) { scalar(t.x); scalar(t.y); scalar(t.sx); scalar(t.sy); }
    void color(HdRgba c) { scalar(c.r); scalar(c.g); scalar(c.b); scalar(c.a); }
    void canvas(const HdCanvas &scene)
    {
        rect(scene.bounds()); scalar(scene.commands().size());
        for (const auto &c : scene.commands())
        {
            scalar(c.op); rect(c.bounds); color(c.color); scalar(c.lineWidth);
            scalar(c.points.size());
            for (auto p : c.points) { scalar(p.x); scalar(p.y); }
            text(c.image.path); scalar(c.image.provider); scalar(c.image.nativeScale);
            rect(c.source); scalar(c.flipX); scalar(c.flipY); scalar(c.opacity);
            scalar(c.imageStyle.light); scalar(c.imageStyle.tint); scalar(c.imageStyle.flatTint);
            color(c.imageStyle.tintColor); transform(c.transform);
            scalar(bool(c.palette));
            if (c.palette) for (auto p : *c.palette) color(p);
            scalar(bool(c.layer));
            if (c.layer) canvas(*c.layer);
        }
    }
    std::string take() { return std::move(_value); }
};
}
