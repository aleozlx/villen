// Villen filter engine — the pixel buffer the morphology operates on.
//
// `Image` is a flat, interleaved 8-bit raster: `channels` samples per pixel,
// row-major, no padding. It is the only data the pure engine touches — no GL, no
// JPEG, no sockets (DESIGN-filter.md §2). The host decodes JPEG into one of these,
// runs `filter::process`, and re-encodes the result; the GPU backend uploads the
// same bytes to a texture. Keeping the buffer this plain is what lets the CPU
// reference and the APU backend be bit-compared (§4.3).
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace villen::filter {

struct Image {
    int width = 0;
    int height = 0;
    int channels = 0;  // 1 = grayscale, 3 = RGB (the two MVP shapes, §3.3)
    std::vector<std::uint8_t> px;  // size = width*height*channels, interleaved

    Image() = default;
    Image(int w, int h, int c)
        : width(w), height(h), channels(c),
          px(static_cast<std::size_t>(w) * h * c, 0) {}

    bool empty() const { return px.empty(); }
    std::size_t pixelCount() const {
        return static_cast<std::size_t>(width) * height;
    }

    // One channel of one pixel. No bounds checking — callers clamp coordinates
    // (clamp-to-edge is the engine's border rule, §3.2).
    std::uint8_t& at(int x, int y, int c) {
        return px[(static_cast<std::size_t>(y) * width + x) * channels + c];
    }
    std::uint8_t at(int x, int y, int c) const {
        return px[(static_cast<std::size_t>(y) * width + x) * channels + c];
    }
};

// A single-channel view materialized as its own buffer. The morphology
// primitives work on planes so `min`/`max` are unambiguous (there is no total
// order on RGB triples — §3.3); colour handling splits/merges planes around them.
struct Plane {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> px;  // size = width*height

    Plane() = default;
    Plane(int w, int h) : width(w), height(h),
                          px(static_cast<std::size_t>(w) * h, 0) {}

    std::uint8_t& at(int x, int y) {
        return px[static_cast<std::size_t>(y) * width + x];
    }
    std::uint8_t at(int x, int y) const {
        return px[static_cast<std::size_t>(y) * width + x];
    }
};

// Lift channel `c` of `img` into its own plane.
Plane extractPlane(const Image& img, int c);
// Write `plane` back into channel `c` of `img` (sizes must match).
void insertPlane(Image& img, int c, const Plane& plane);

// Integer luma (BT.601-ish, exact and deterministic): (77R + 150G + 29B) >> 8.
// Used by the `gray`/`luma` colour modes (§3.3); the weights sum to 256 so the
// shift is a clean divide. A 1-channel image is already its own luma.
Plane lumaPlane(const Image& img);

}  // namespace villen::filter
