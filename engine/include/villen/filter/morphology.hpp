// Villen filter engine — the morphology primitives (DESIGN-filter.md §3.1).
//
// Two neighbourhood reductions (erode = min, dilate = max over a flat structuring
// element) plus a handful of pixel-wise combines. Everything else — open, close,
// gradient, top-hat, black-hat — *decomposes* into these (§3.1), so the GPU
// backend needs only an erode kernel, a dilate kernel, and a combine kernel
// (§4.2). These functions are the executable definition the APU must match
// bit-for-bit (§4.3): pure 8-bit integer min/max/saturating-subtract, no float.
#pragma once

#include <cstdint>

#include "image.hpp"

namespace villen::filter {

// Flat structuring-element shape. The neighbourhood is the (2r+1)² box filtered
// by the shape predicate, evaluated with clamp-to-edge at the border (§3.2).
enum class SE { Box, Cross, Disk };

struct StructElem {
    SE shape = SE::Box;
    int radius = 1;  // r >= 0; r == 0 is the identity (the single centre pixel)
};

// Is offset (dx,dy) a member of structuring element `se`? Assumes the caller has
// already bounded the offset to [-r, r]² (the dispatch loop does).
inline bool inStructElem(int dx, int dy, const StructElem& se) {
    switch (se.shape) {
        case SE::Box:   return true;                        // full square
        case SE::Cross: return dx == 0 || dy == 0;          // 4-neighbour arms
        case SE::Disk:  return dx * dx + dy * dy <= se.radius * se.radius;
    }
    return true;
}

// f ⊖ B : per output pixel, the min over the SE neighbourhood (clamp-to-edge).
Plane erode(const Plane& f, const StructElem& se);
// f ⊕ B : per output pixel, the max over the SE neighbourhood (clamp-to-edge).
Plane dilate(const Plane& f, const StructElem& se);

// Pixel-wise combines (the "combine kernel", §4.2). Sizes must match.
Plane subtractClamped(const Plane& a, const Plane& b);  // max(a-b, 0), saturating
Plane threshold(const Plane& f, std::uint8_t t);        // f >= t ? 255 : 0
Plane invert(const Plane& f);                            // 255 - f

}  // namespace villen::filter
