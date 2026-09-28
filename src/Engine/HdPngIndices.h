#pragma once
#include <cstddef>
#include <vector>

namespace OpenXcom
{
// LodePNG's decoded sub-byte pixels are MSB-first, without scanline padding.
// Normalize storage only. Having indices does not opt an image into palette rendering.
inline bool hdUnpackPngIndices(const std::vector<unsigned char> &raw, size_t count,
    unsigned depth, std::vector<unsigned char> &indices)
{
    if (depth != 1 && depth != 2 && depth != 4 && depth != 8) return false;
    const size_t perByte = 8 / depth;
    if (raw.size() < count / perByte + (count % perByte != 0)) return false;
    indices.resize(count);
    const unsigned mask = (1u << depth) - 1;
    for (size_t i = 0; i < count; ++i)
        indices[i] = (raw[i / perByte] >> (8 - depth - unsigned(i % perByte) * depth)) & mask;
    return true;
}
}
