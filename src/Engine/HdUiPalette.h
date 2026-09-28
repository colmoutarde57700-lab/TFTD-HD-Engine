#pragma once
#include <array>
#include <algorithm>
#include <cstdint>

namespace OpenXcom
{
using HdUiIndexMap = std::array<std::uint8_t, 256>;
inline HdUiIndexMap hdUiIdentityPalette()
{
    HdUiIndexMap result{};
    for (int i = 1; i < 256; ++i) result[i] = static_cast<std::uint8_t>(i);
    return result;
}
// Style transforms for explicitly indexed transition PNGs. These operate on
// palette metadata, not on an old Surface or a previously rendered image.
inline HdUiIndexMap hdUiOffsetPalette(int offset, int minimum = -1)
{
    auto result = hdUiIdentityPalette();
    if (offset == 0) return result;
    for (int i = 1; i < 256; ++i)
        result[i] = static_cast<std::uint8_t>(minimum == -1 ? i + offset : std::max(minimum, i + offset));
    return result;
}
inline HdUiIndexMap hdUiOffsetBlockPalette(int offset)
{
    auto result = hdUiIdentityPalette();
    for (int i = 1; i < 256; ++i)
    {
        const int start = i / 16 * 16;
        result[i] = static_cast<std::uint8_t>(std::max(start, std::min(start + 16, i + offset)));
    }
    return result;
}
inline HdUiIndexMap hdUiInvertPalette(std::uint8_t middle)
{
    auto result = hdUiIdentityPalette();
    for (int i = 1; i < 256; ++i) result[i] = static_cast<std::uint8_t>(2 * int(middle) - i);
    return result;
}
inline HdUiIndexMap hdUiTftdPressedPalette()
{
    auto result = hdUiIdentityPalette();
    constexpr int from[] = {1, 2, 3, 4, 7, 8, 31, 47, 153, 156, 159};
    constexpr int to[] = {2, 3, 4, 5, 11, 10, 2, 2, 96, 9, 97};
    for (std::size_t i = 0; i < sizeof(from) / sizeof(from[0]); ++i)
        result[from[i]] = static_cast<std::uint8_t>(to[i]);
    return result;
}
}
