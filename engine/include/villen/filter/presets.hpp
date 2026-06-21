// Villen filter engine — named pipeline presets (DESIGN-filter.md §8).
//
// The admin UI offers one-click presets ("Gradient", "Open-3 disk", "Edge +
// threshold"); they are just `Pipeline` values, so they live in the pure engine
// where they can be unit-tested with no host. The host maps a client's
// `requestPreset` (§5.2) to one of these — staying the authority over which
// pipeline actually runs.
#pragma once

#include <string_view>

#include "pipeline.hpp"

namespace villen::filter::presets {

// Morphological gradient with a disk SE — the most demo-friendly operator (§3.1).
Pipeline gradient(int radius = 2);

// Open with a disk SE — removes small bright specks while keeping shapes.
Pipeline openDisk(int radius = 3);

// Gradient then a binarising threshold — the starkest, most legible edge map.
Pipeline edgeThreshold(int radius = 2, std::uint8_t level = 64);

// Resolve a preset by name; returns the identity pipeline for an unknown name.
Pipeline byName(std::string_view name);

}  // namespace villen::filter::presets
