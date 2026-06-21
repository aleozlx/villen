#include "villen/filter/morphology.hpp"

#include <algorithm>

namespace villen::filter {
namespace {

// Replicate-border sampling: out-of-range coordinates clamp to the nearest edge
// pixel, so the frame border is well-defined with no wraparound artefacts (§3.2).
inline std::uint8_t sampleClamped(const Plane& f, int x, int y) {
    x = std::clamp(x, 0, f.width - 1);
    y = std::clamp(y, 0, f.height - 1);
    return f.at(x, y);
}

// The shared neighbourhood reduction. `erodeMode` picks min (erode) over the SE;
// otherwise max (dilate). One thread per output pixel in the GPU mirror (§4.2).
Plane reduce(const Plane& f, const StructElem& se, bool erodeMode) {
    Plane out(f.width, f.height);
    const int r = se.radius;
    for (int y = 0; y < f.height; ++y)
        for (int x = 0; x < f.width; ++x) {
            std::uint8_t acc = erodeMode ? 255 : 0;
            for (int dy = -r; dy <= r; ++dy)
                for (int dx = -r; dx <= r; ++dx) {
                    if (!inStructElem(dx, dy, se)) continue;
                    std::uint8_t v = sampleClamped(f, x + dx, y + dy);
                    acc = erodeMode ? std::min(acc, v) : std::max(acc, v);
                }
            out.at(x, y) = acc;
        }
    return out;
}

}  // namespace

Plane erode(const Plane& f, const StructElem& se) { return reduce(f, se, true); }
Plane dilate(const Plane& f, const StructElem& se) { return reduce(f, se, false); }

Plane subtractClamped(const Plane& a, const Plane& b) {
    Plane out(a.width, a.height);
    for (std::size_t i = 0; i < out.px.size(); ++i) {
        int d = static_cast<int>(a.px[i]) - static_cast<int>(b.px[i]);
        out.px[i] = static_cast<std::uint8_t>(d < 0 ? 0 : d);
    }
    return out;
}

Plane threshold(const Plane& f, std::uint8_t t) {
    Plane out(f.width, f.height);
    for (std::size_t i = 0; i < out.px.size(); ++i)
        out.px[i] = f.px[i] >= t ? 255 : 0;
    return out;
}

Plane invert(const Plane& f) {
    Plane out(f.width, f.height);
    for (std::size_t i = 0; i < out.px.size(); ++i)
        out.px[i] = static_cast<std::uint8_t>(255 - f.px[i]);
    return out;
}

}  // namespace villen::filter
