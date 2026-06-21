#include "villen/filter/presets.hpp"

namespace villen::filter::presets {

Pipeline gradient(int radius) {
    Pipeline p;
    p.stages.push_back({Op::Gradient, {SE::Disk, radius}, 0, false});
    return p;
}

Pipeline openDisk(int radius) {
    Pipeline p;
    p.stages.push_back({Op::Open, {SE::Disk, radius}, 0, false});
    return p;
}

Pipeline edgeThreshold(int radius, std::uint8_t level) {
    Pipeline p;
    p.stages.push_back({Op::Gradient, {SE::Disk, radius}, 0, false});
    p.stages.push_back({Op::Threshold, {}, level, false});
    return p;
}

Pipeline byName(std::string_view name) {
    if (name == "gradient") return gradient();
    if (name == "open" || name == "openDisk") return openDisk();
    if (name == "edge" || name == "edgeThreshold") return edgeThreshold();
    return Pipeline{};  // unknown -> identity (empty stage list)
}

}  // namespace villen::filter::presets
