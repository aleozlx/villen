#include "villen/filter/pipeline.hpp"

#include <algorithm>

namespace villen::filter {
namespace {

Plane openPlane(const Plane& f, const StructElem& se) {
    return dilate(erode(f, se), se);  // f ∘ B
}
Plane closePlane(const Plane& f, const StructElem& se) {
    return erode(dilate(f, se), se);  // f • B
}

// The morphology of one stage, before the invert toggle. Composites are spelled
// out as their erode/dilate/combine decomposition (§3.1) so the GPU backend can
// reproduce the exact same primitive sequence.
Plane stageCore(const Plane& f, const Stage& s) {
    switch (s.op) {
        case Op::Erode:    return erode(f, s.se);
        case Op::Dilate:   return dilate(f, s.se);
        case Op::Open:     return openPlane(f, s.se);
        case Op::Close:    return closePlane(f, s.se);
        case Op::Gradient: return subtractClamped(dilate(f, s.se), erode(f, s.se));
        case Op::TopHat:   return subtractClamped(f, openPlane(f, s.se));
        case Op::BlackHat: return subtractClamped(closePlane(f, s.se), f);
        case Op::Threshold: return threshold(f, s.t);
    }
    return f;
}

// Recolour an RGB image so each pixel keeps its hue but takes the processed
// luma's brightness (§3.3 "luma" mode): out = rgb * Lout / Lin. Avoids the
// per-channel edge fringing at the cost of integer division rounding.
Image recolourFromLuma(const Image& src, const Plane& lin, const Plane& lout) {
    Image out(src.width, src.height, 3);
    for (int y = 0; y < src.height; ++y)
        for (int x = 0; x < src.width; ++x) {
            int li = lin.at(x, y);
            int lo = lout.at(x, y);
            for (int c = 0; c < 3; ++c) {
                int v = li > 0 ? src.at(x, y, c) * lo / li : lo;
                out.at(x, y, c) = static_cast<std::uint8_t>(std::min(v, 255));
            }
        }
    return out;
}

}  // namespace

Plane applyStage(const Plane& in, const Stage& stage) {
    Plane out = stageCore(in, stage);
    return stage.invert ? invert(out) : out;
}

Image process(const Image& in, const Pipeline& pipeline) {
    if (in.empty()) return in;

    auto runStages = [&](Plane p) {
        for (const Stage& s : pipeline.stages) p = applyStage(p, s);
        return p;
    };

    switch (pipeline.color) {
        case Color::PerChannel: {
            // Each channel is its own grayscale image; run the full pipeline on
            // each and recombine. Exact integer ops, no cross-channel coupling.
            Image out(in.width, in.height, in.channels);
            for (int c = 0; c < in.channels; ++c)
                insertPlane(out, c, runStages(extractPlane(in, c)));
            return out;
        }
        case Color::Gray: {
            Plane g = runStages(lumaPlane(in));
            Image out(in.width, in.height, 1);
            out.px = std::move(g.px);
            return out;
        }
        case Color::Luma: {
            Plane lin = lumaPlane(in);
            Plane lout = runStages(lin);
            if (in.channels == 1) {
                Image out(in.width, in.height, 1);
                out.px = std::move(lout.px);
                return out;
            }
            return recolourFromLuma(in, lin, lout);
        }
    }
    return in;
}

}  // namespace villen::filter
