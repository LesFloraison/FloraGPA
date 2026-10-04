#pragma once
#include "Frame.h"
namespace flora {
struct DiscardRecord {
    uint16_t type{};
    Id link{}, context{}, target{};
    uint32_t count{};
    bool hasRectangles{};
    std::vector<std::array<int32_t, 4>> rectangles;
};
struct DiscardTarget {
    Id resource{};
    uint16_t viewType{};
};
bool isDiscardRecord(uint16_t type);
DiscardRecord readDiscardRecord(uint16_t type, Bytes payload);
DiscardTarget validateDiscardRecord(const Frame &frame, const DiscardRecord &record);
bool ambiguousDiscardRectangles(const DiscardRecord &record);
} // namespace flora
