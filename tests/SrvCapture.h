#pragma once
#include "SamplerCapture.h"
namespace flora::testing {
inline Capture srvCapture() {
    auto c = samplerCapture();
    c.entries.erase(std::remove_if(c.entries.begin(), c.entries.end(),
                                   [](const auto &e) { return e.id == 730 || e.id == 731 || e.id == 732; }),
                    c.entries.end());
    c.add(730, 5, 0x85, statePack(Id(0), Id(0), 2u, 2u, 2u, 1u, 2u, 1u, 0u, 0u, 8u, 0u, 0u, Id(731)));
    auto pixels = statePack(80u);
    for (int i = 0; i < 5; ++i) {
        append(pixels, i == 4 ? .75f : .125f);
        append(pixels, 0.f);
        append(pixels, 0.f);
        append(pixels, 1.f);
    }
    c.add(731, 9, 1, pixels);
    c.add(732, 5, 0x8c, statePack(Id(0), Id(0), Id(730), 2u, 4u, 0u, 2u, 0u, 0u));
    return c;
}
} // namespace flora::testing
