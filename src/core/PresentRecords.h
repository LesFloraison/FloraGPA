#pragma once
#include "Frame.h"
namespace flora {
struct PresentRecord {
    Id chain{}, backbuffer{};
    uint32_t syncInterval{}, flags{}, swapEffect{};
    bool test{}, occluded{}, unbindRtv{};
};
// Validates the captured call and the currently recovered single-frame boundary.
// No HWND, capture pointer, or saved framebuffer pixels are used by this path.
PresentRecord validatePresentRecord(const Frame &frame, Id event);
} // namespace flora
