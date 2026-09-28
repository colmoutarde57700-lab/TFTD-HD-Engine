#pragma once
#include <SDL.h>
#include <map>
#include <memory>
#include <string>
#include <cstdint>
#include <algorithm>

namespace OpenXcom
{
// Physical HD pixels only. Native fallbacks are never admitted to this cache.
// Each owner has one most-recent content entry, with a global memory/entry cap.
class HdUiRasterCache
{
public:
    using SurfacePtr = std::shared_ptr<SDL_Surface>;
    struct Entry
    {
        std::string signature, error;
        SurfacePtr surface;
        std::uint64_t touched = 0;
        size_t bytes = 0;
    };
private:
    std::map<const void*, Entry> _entries;
    size_t _bytes = 0, _budget, _maxEntries;
    std::uint64_t _clock = 0, _generation = 0;
public:
    std::uint64_t hits = 0, misses = 0, evictions = 0, rasters = 0, rasterPixels = 0;
    std::uint64_t composeMs = 0, rasterMs = 0, submitMs = 0;
    Uint32 reportTick = 0;
    explicit HdUiRasterCache(size_t budget = 128u * 1024u * 1024u, size_t maxEntries = 512)
        : _budget(budget), _maxEntries(maxEntries) {}
    void clear() { _entries.clear(); _bytes = 0; }
    void generation(std::uint64_t value)
    {
        if (_generation != value) { clear(); _generation = value; }
    }
    size_t bytes() const { return _bytes; }
    size_t size() const { return _entries.size(); }
    const Entry *find(const void *owner, const std::string &signature)
    {
        auto it = _entries.find(owner);
        if (it != _entries.end() && it->second.signature == signature)
        {
            ++hits; it->second.touched = ++_clock; return &it->second;
        }
        ++misses;
        if (it != _entries.end()) { _bytes -= it->second.bytes; _entries.erase(it); }
        return nullptr;
    }
    void store(const void *owner, std::string signature, SurfacePtr surface, std::string error = {})
    {
        auto previous = _entries.find(owner);
        if (previous != _entries.end()) { _bytes -= previous->second.bytes; _entries.erase(previous); }
        const size_t bytes = signature.capacity() + error.capacity() + sizeof(Entry) +
            (surface ? size_t(surface->pitch) * surface->h : 0);
        if (bytes > _budget || !_maxEntries) return;
        while (!_entries.empty() && (_bytes + bytes > _budget || _entries.size() >= _maxEntries))
        {
            auto oldest = std::min_element(_entries.begin(), _entries.end(),
                [](const auto &a, const auto &b) { return a.second.touched < b.second.touched; });
            _bytes -= oldest->second.bytes; _entries.erase(oldest); ++evictions;
        }
        _entries.emplace(owner, Entry{std::move(signature), std::move(error), std::move(surface), ++_clock, bytes});
        _bytes += bytes;
    }
};
}
