#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <vector>

namespace OpenXcom
{
// Geometry derived exclusively from the selected decoded HD PNG's alpha.
// Empty rows do not count towards a silhouette's damage/shield percentage.
inline std::vector<unsigned> hdAlphaRows(const std::vector<unsigned char> &rgba,
    unsigned width, unsigned height, unsigned x0, unsigned y0, unsigned x1, unsigned y1)
{
    if (!width || !height || std::size_t(width) > std::numeric_limits<std::size_t>::max() / height / 4 ||
        rgba.size() != std::size_t(width) * height * 4 || x0 > x1 || y0 > y1 || x1 > width || y1 > height)
        throw std::invalid_argument("Invalid HD alpha scan extent");
    std::vector<unsigned> rows;
    for (unsigned y = y0; y < y1; ++y)
        for (unsigned x = x0; x < x1; ++x)
            if (rgba[(std::size_t(y) * width + x) * 4 + 3]) { rows.push_back(y); break; }
    return rows;
}

struct HdAlphaRowRange { unsigned begin = 0, end = 0; };
inline HdAlphaRowRange hdAlphaFraction(const std::vector<unsigned> &rows, double fraction, bool fromBottom)
{
    if (!std::isfinite(fraction) || fraction < 0 || fraction > 1)
        throw std::invalid_argument("Invalid HD silhouette fraction");
    const std::size_t count = fraction == 1 ? rows.size() : std::size_t(std::floor(rows.size() * fraction));
    if (!count) return {};
    return fromBottom ? HdAlphaRowRange{rows[rows.size() - count], rows.back() + 1} :
        HdAlphaRowRange{rows.front(), rows[count - 1] + 1};
}
}
