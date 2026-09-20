#pragma once
#include "SrvCapture.h"
namespace flora::testing {
inline Capture srvBindingCapture() {
    auto c = srvCapture();
    c.add(733, 5, 0x8c, statePack(Id(0), Id(0), Id(730), 2u, 4u, 1u, 1u, 0u, 0u));
    c.add(840, 7, 0x37, statePack(Id(990), Id(0), Id(1), 0u, 0u));
    c.add(860, 7, 0x34e6, statePack(Id(0), Id(1), 0u, 1u, uint8_t(1), Id(732)));
    c.add(910, 7, 0x34e6, statePack(Id(0), Id(1), 0u, 1u, uint8_t(1), Id(732)));
    return c;
}
} // namespace flora::testing
