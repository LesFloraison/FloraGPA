#pragma once
#include "Frame.h"
namespace flora {
struct ClearViewCommand {
    Id context{}, view{};
    std::array<float, 4> color{};
    uint32_t count{};
    bool hasRectangles{};
    std::vector<std::array<int32_t, 4>> rectangles;
};
struct ClearViewTarget {
    Id resource{};
    uint16_t viewType{};
    uint32_t format{}, dimension{};
};
ClearViewCommand readClearView(Bytes bytes);
ClearViewTarget validateClearView(const Frame &frame, const ClearViewCommand &command);
} // namespace flora
