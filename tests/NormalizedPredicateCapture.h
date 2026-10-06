#pragma once
#include "PredicateCapture.h"
namespace flora::testing {
inline Capture normalizedPredicateCapture(uint32_t value = 1) {
    auto c = predicateCapture(false, value, false, true);
    c.entries.erase(std::remove_if(c.entries.begin(), c.entries.end(),
                                    [](const auto &e) { return e.id == 600; }), c.entries.end());
    auto witness = statePack(Id(1200), Id(600), uint32_t(0x887a0002));
    const std::array<uint8_t, 16> marker{0x00,0xdf,0x60,0x68,0x75,0x8a,0xb6,0x48,
                                         0xa1,0x5e,0xa8,0x9f,0x69,0x8b,0xf8,0x1f};
    witness.insert(witness.end(), marker.begin(), marker.end());
    append(witness, uint8_t(1));
    append(witness, 0u);
    append(witness, Id(0x12345678)); // Opaque captured pointer; never dereferenced.
    c.add(1201, 7, 0x3166, witness);
    return c;
}
} // namespace flora::testing
