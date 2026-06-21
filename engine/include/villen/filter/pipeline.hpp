// Villen filter engine — the data-described pipeline (DESIGN-filter.md §3).
//
// The engine's "rules" are an ordered list of operator stages applied to an
// image. The operator edits this list live in the admin UI (§8); the client
// never decides it (server-authoritative, the chess §3.2 invariant carried over).
// This header is pure data + one entry point — no JSON, no GL, no device code, so
// it unit-tests in CI with no GPU (§11.1). The host parses wire JSON (§5.2) into
// a `Pipeline`; the GPU backend reads the same `Pipeline` and must reproduce
// `process`'s output (§4.3).
#pragma once

#include <cstdint>
#include <vector>

#include "image.hpp"
#include "morphology.hpp"

namespace villen::filter {

// The operators the pipeline can name (§3.1). Erode/Dilate are primitives; the
// rest decompose into them plus a combine. Threshold is a pointwise gate.
enum class Op {
    Erode,
    Dilate,
    Open,      // (f ⊖ B) ⊕ B  — removes small bright specks
    Close,     // (f ⊕ B) ⊖ B  — fills small dark holes
    Gradient,  // (f ⊕ B) − (f ⊖ B)  — edge/outline map
    TopHat,    // f − (f ∘ B)  — bright detail smaller than B
    BlackHat,  // (f • B) − f  — dark detail smaller than B
    Threshold, // f >= t ? 255 : 0
};

// One stage of the pipeline. `se` is ignored by Threshold; `t` is used only by
// Threshold; `invert` post-inverts any stage's output (the §8 invert toggle).
struct Stage {
    Op op = Op::Erode;
    StructElem se{};
    std::uint8_t t = 128;   // Threshold level
    bool invert = false;
};

// How colour is handled (§3.3). PerChannel is the MVP default: treat R,G,B as
// three independent grayscale images (embarrassingly parallel; slight edge
// fringing reads as a feature). Gray collapses to luma and outputs 1 channel.
// Luma processes luma but recolours from the original (no fringing).
enum class Color { PerChannel, Gray, Luma };

struct Pipeline {
    std::vector<Stage> stages;
    Color color = Color::PerChannel;
};

// Apply one stage to a single plane, using its building-block primitives. Exposed
// so the GPU backend and tests can validate stage-by-stage against the reference.
Plane applyStage(const Plane& in, const Stage& stage);

// Run the whole pipeline over an image. Deterministic and allocation-visible; the
// output channel count depends on the colour mode (Gray -> 1, else same as in).
Image process(const Image& in, const Pipeline& pipeline);

}  // namespace villen::filter
