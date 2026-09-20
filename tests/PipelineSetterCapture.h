#pragma once
#include "MsaaCapture.h"
namespace flora::testing {
inline Capture pipelineSetterCapture() {
    auto c = msaaOutputCapture(false);
    c.add(90, 7, 0x34f6, statePack(Id(0), Id(1), 4u));
    c.add(91, 7, 0x3501, statePack(Id(0), Id(1), Id(0), uint8_t(0), UINT32_MAX));
    c.add(92, 7, 0x3502, statePack(Id(0), Id(1), Id(0), 0u));
    c.add(93, 7, 0x3509, statePack(Id(0), Id(1), Id(52)));
    c.add(94, 7, 0x350a, statePack(Id(0), Id(1), 1u, uint8_t(1), 0.f, 0.f, 7.f, 5.f, 0.f, 1.f));
    c.add(95, 7, 0x350b, statePack(Id(0), Id(1), 1u, uint8_t(1), 0, 0, 7, 5));
    c.add(105, 7, 0x34f6, statePack(Id(0), Id(1), 4u));
    c.add(108, 7, 0x242, statePack(Id(0), Id(1)));
    return c;
}
} // namespace flora::testing
