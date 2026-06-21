#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "villen/filter/morphology.hpp"
#include "villen/filter/pipeline.hpp"
#include "villen/filter/presets.hpp"

// Unit tests for the pure CPU morphology reference (DESIGN-filter.md §13 step 1,
// §14 #1). These "prove the math" with no GPU and no host: each operator is
// checked on a tiny fixture, and the whole pipeline is checked end to end. Later,
// the APU backend is validated for byte-equality against this very reference
// (§4.3), so these tests double as the GPU oracle.
using namespace villen::filter;

namespace {

// Build a single-channel plane from a flat row-major list (handy for fixtures).
Plane planeOf(int w, int h, std::initializer_list<int> vals) {
    Plane p(w, h);
    int i = 0;
    for (int v : vals) p.px[i++] = static_cast<std::uint8_t>(v);
    return p;
}

}  // namespace

TEST_CASE("erode removes a lone bright pixel") {
    // A single 255 on black: every neighbourhood touches a 0, so min wipes it.
    Plane f = planeOf(3, 3, {0, 0, 0,
                             0, 255, 0,
                             0, 0, 0});
    Plane e = erode(f, {SE::Box, 1});
    for (std::uint8_t v : e.px) CHECK(v == 0);
}

TEST_CASE("dilate grows a lone bright pixel by the SE shape") {
    Plane f = planeOf(3, 3, {0, 0, 0,
                             0, 255, 0,
                             0, 0, 0});

    SUBCASE("box fills the whole 3x3") {
        Plane d = dilate(f, {SE::Box, 1});
        for (std::uint8_t v : d.px) CHECK(v == 255);
    }
    SUBCASE("disk r1 makes a plus (corners stay dark)") {
        Plane d = dilate(f, {SE::Disk, 1});
        CHECK(d.at(1, 1) == 255);  // centre
        CHECK(d.at(0, 1) == 255);  // arms
        CHECK(d.at(2, 1) == 255);
        CHECK(d.at(1, 0) == 255);
        CHECK(d.at(1, 2) == 255);
        CHECK(d.at(0, 0) == 0);    // corners excluded by the disk predicate
        CHECK(d.at(2, 2) == 0);
    }
    SUBCASE("cross r1 also makes a plus") {
        Plane d = dilate(f, {SE::Cross, 1});
        CHECK(d.at(1, 1) == 255);
        CHECK(d.at(0, 0) == 0);
        CHECK(d.at(2, 0) == 0);
    }
}

TEST_CASE("clamp-to-edge: a flat image is a morphology fixed point") {
    // No wraparound, replicate border -> a uniform image survives any op.
    Plane white(4, 4);
    for (auto& v : white.px) v = 255;
    Plane eroded = erode(white, {SE::Box, 2});
    for (std::uint8_t v : eroded.px) CHECK(v == 255);

    Plane black(4, 4);  // all zero
    Plane dilated = dilate(black, {SE::Box, 2});
    for (std::uint8_t v : dilated.px) CHECK(v == 0);
}

TEST_CASE("open removes a small speck but keeps a larger shape") {
    // 5x5: a lone speck at (0,0) and a solid 3x3 block at the bottom-right.
    Plane f = planeOf(5, 5, {255, 0, 0, 0, 0,
                             0, 0, 0, 0, 0,
                             0, 0, 255, 255, 255,
                             0, 0, 255, 255, 255,
                             0, 0, 255, 255, 255});
    Stage open{Op::Open, {SE::Box, 1}, 0, false};
    Plane r = applyStage(f, open);

    CHECK(r.at(0, 0) == 0);    // speck erased (it can't survive an erosion)
    CHECK(r.at(3, 3) == 255);  // the block's interior is preserved
}

TEST_CASE("close fills a small dark hole") {
    Plane f(5, 5);
    for (auto& v : f.px) v = 255;
    f.at(2, 2) = 0;  // a one-pixel hole in a white field
    Plane r = applyStage(f, {Op::Close, {SE::Box, 1}, 0, false});
    for (std::uint8_t v : r.px) CHECK(v == 255);  // hole filled, field intact
}

TEST_CASE("gradient of a step edge is a bright line at the boundary") {
    // Columns 0,1 dark; columns 2,3,4 bright. The edge sits between col 1 and 2.
    Plane f = planeOf(5, 3, {0, 0, 255, 255, 255,
                             0, 0, 255, 255, 255,
                             0, 0, 255, 255, 255});
    Plane g = applyStage(f, {Op::Gradient, {SE::Box, 1}, 0, false});
    for (int y = 0; y < 3; ++y) {
        CHECK(g.at(0, y) == 0);    // flat interior -> no gradient
        CHECK(g.at(1, y) == 255);  // boundary columns light up
        CHECK(g.at(2, y) == 255);
        CHECK(g.at(3, y) == 0);
        CHECK(g.at(4, y) == 0);
    }
}

TEST_CASE("subtract saturates and threshold binarises") {
    Plane a = planeOf(2, 1, {10, 200});
    Plane b = planeOf(2, 1, {50, 100});
    Plane d = subtractClamped(a, b);
    CHECK(d.at(0, 0) == 0);    // 10-50 clamps to 0, not wraps to 216
    CHECK(d.at(1, 0) == 100);  // 200-100

    Plane t = threshold(planeOf(3, 1, {127, 128, 200}), 128);
    CHECK(t.at(0, 0) == 0);
    CHECK(t.at(1, 0) == 255);
    CHECK(t.at(2, 0) == 255);
}

TEST_CASE("invert toggle post-inverts a stage") {
    Plane f = planeOf(2, 1, {0, 255});
    Plane r = applyStage(f, {Op::Erode, {SE::Box, 0}, 0, /*invert=*/true});
    // r==0 erode is identity, then invert: 0->255, 255->0.
    CHECK(r.at(0, 0) == 255);
    CHECK(r.at(1, 0) == 0);
}

TEST_CASE("per-channel colour processes R,G,B independently") {
    // R has a speck, G a block, B is empty. Open must touch only its own channel.
    Image img(3, 3, 3);
    img.at(1, 1, 0) = 255;                       // R: lone speck
    for (int y = 0; y < 3; ++y)
        for (int x = 0; x < 3; ++x) img.at(x, y, 1) = 255;  // G: solid

    Pipeline p;
    p.color = Color::PerChannel;
    p.stages.push_back({Op::Open, {SE::Box, 1}, 0, false});
    Image out = process(img, p);

    CHECK(out.channels == 3);
    CHECK(out.at(1, 1, 0) == 0);    // R speck removed by open
    CHECK(out.at(1, 1, 1) == 255);  // G solid survives
    CHECK(out.at(1, 1, 2) == 0);    // B untouched (was empty)
}

TEST_CASE("gray colour mode collapses to a single luma channel") {
    Image img(2, 1, 3);
    img.at(0, 0, 0) = 255;  // pure red
    img.at(1, 0, 1) = 255;  // pure green
    Pipeline p;
    p.color = Color::Gray;  // empty stage list -> just the luma conversion
    Image out = process(img, p);

    CHECK(out.channels == 1);
    CHECK(out.at(0, 0, 0) == (77 * 255 >> 8));   // red luma weight
    CHECK(out.at(1, 0, 0) == (150 * 255 >> 8));  // green luma weight
}

TEST_CASE("process is deterministic — the GPU oracle (§4.3)") {
    // A fixed pipeline over a fixed input must be byte-stable run to run; this is
    // exactly the invariant the APU backend is later checked against.
    Image img(4, 4, 3);
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x)
            for (int c = 0; c < 3; ++c)
                img.at(x, y, c) = static_cast<std::uint8_t>((x * 37 + y * 53 + c * 71) & 0xFF);

    Pipeline p = presets::edgeThreshold(2, 40);
    Image a = process(img, p);
    Image b = process(img, p);
    CHECK(a.px == b.px);
    CHECK(a.width == 4);
    CHECK(a.height == 4);
}

TEST_CASE("presets resolve by name, unknown falls back to identity") {
    CHECK(presets::byName("gradient").stages.size() == 1);
    CHECK(presets::byName("edge").stages.size() == 2);
    CHECK(presets::byName("nope").stages.empty());  // identity
}
