#include "villen/filter/image.hpp"

namespace villen::filter {

Plane extractPlane(const Image& img, int c) {
    Plane p(img.width, img.height);
    for (int y = 0; y < img.height; ++y)
        for (int x = 0; x < img.width; ++x) p.at(x, y) = img.at(x, y, c);
    return p;
}

void insertPlane(Image& img, int c, const Plane& plane) {
    for (int y = 0; y < img.height; ++y)
        for (int x = 0; x < img.width; ++x) img.at(x, y, c) = plane.at(x, y);
}

Plane lumaPlane(const Image& img) {
    Plane p(img.width, img.height);
    if (img.channels == 1) {
        p.px = img.px;  // already single-channel
        return p;
    }
    for (int y = 0; y < img.height; ++y)
        for (int x = 0; x < img.width; ++x) {
            int r = img.at(x, y, 0), g = img.at(x, y, 1), b = img.at(x, y, 2);
            p.at(x, y) = static_cast<std::uint8_t>((77 * r + 150 * g + 29 * b) >> 8);
        }
    return p;
}

}  // namespace villen::filter
