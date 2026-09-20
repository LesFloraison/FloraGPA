#pragma once
#include "PredicateCapture.h"
namespace flora::testing {
inline Capture depthStencilCapture(bool backFace = false) {
    auto c = predicateCapture();
    c.entries.erase(std::remove_if(c.entries.begin(), c.entries.end(),
                                   [](const auto &e) { return e.category == 7 || e.category == 3; }),
                    c.entries.end());
    c.add(720, 5, 0x85, statePack(Id(0), Id(0), 1u, 1u, 1u, 1u, 45u, 1u, 0u, 0u, 64u, 0u, 0u, Id(0)));
    c.add(722, 5, 0x8e, statePack(Id(0), Id(0), Id(720), 45u, 3u, 0u, 0u, 0u, 0u));
    c.add(724, 5, 0x89, statePack(Id(0), Id(0), 3u, 1u, uint32_t(backFace), 0, 0.f, 0.f, 1u, 0u, 0u, 0u));
    c.add(800, 7, 0x32, statePack(Id(0), Id(1), Id(21), uint8_t(1), 0.f, 0.f, 0.f, 1.f));
    c.add(810, 7, 0x31, statePack(Id(0), Id(1), Id(722), 3u, .5f, uint8_t(0x53)));
    State s{};
    s.topology = 4;
    s.sampleMask = UINT32_MAX;
    s.omStart = 8;
    s.rtCount = 1;
    s.rtv[0] = 21;
    s.dsv = 722;
    s.rasterizer = 724;
    s.viewports = 22;
    s.stages[0].shader = 30;
    s.stages[4].shader = 32;
    c.add(990, 3, 3, snapshot(s));
    c.add(1990, 3, 3, snapshot(s));
    c.add(1000, 7, 0x37, statePack(Id(990), Id(0), Id(1), 3u, 0u));
    c.add(2000, 7, 0x37, statePack(Id(1990), Id(0), Id(1), 3u, 0u));
    return c;
}
} // namespace flora::testing
