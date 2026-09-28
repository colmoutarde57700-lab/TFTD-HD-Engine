#pragma once
#include "HdCanvas.h"

namespace OpenXcom
{
// Temporary marker requested for the transition UI. It is emitted only by
// the owned HD scene producer, never baked into a native image or framebuffer.
// Existing artwork and controls keep their own geometry and resource identity.
inline void hdAppendPipelineBadge(HdCanvas &canvas)
{
    const auto bounds = canvas.bounds();
    if (bounds.w < 26 || bounds.h < 14) return;
    HdCanvas badge(13, 9);
    badge.rectangle({0, 0, 13, 9}, {40, 192, 207, 255});
    badge.rectangle({1, 1, 11, 7}, {12, 29, 36, 255});
    constexpr const char *letters[] = {"1010110", "1010101", "1110101", "1010101", "1010110"};
    for (int y = 0; y < 5; ++y)
        for (int x = 0; x < 7; ++x)
            if (letters[y][x] == '1')
                badge.rectangle({double(3 + x), double(2 + y), 1, 1}, {240, 251, 252, 255});
    canvas.composite(badge, {bounds.w - 15, 2, 1, 1});
}
}
