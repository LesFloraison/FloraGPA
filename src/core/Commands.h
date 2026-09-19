#pragma once
#include "Frame.h"

namespace flora {
bool isWritableCommand(uint16_t type);
bool isClearCommand(uint16_t type);
void requireImmediateContext(const Frame &frame, Id context);
void validateWritableCommand(const Frame &frame, Id event);
struct UpdateSourceLayout {
    Id destination{}, data{};
    uint32_t subresource{}, width{}, height{}, depth{}, rowPitch{}, slicePitch{};
    uint64_t size{};
    bool hasBox{};
    std::array<uint32_t, 6> box{};
};
UpdateSourceLayout updateSourceLayout(const Frame &frame, Id event);
} // namespace flora
