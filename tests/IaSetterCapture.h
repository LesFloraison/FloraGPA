#pragma once
#include "StateCapture.h"
namespace flora::testing {
inline std::vector<uint8_t> vertexSetter(uint32_t start, const std::vector<Id> &ids,
                                         const std::vector<uint32_t> &strides,
                                         const std::vector<uint32_t> &offsets) {
    auto raw = statePack(Id(0), Id(1), start, uint32_t(ids.size()));
    append(raw, uint8_t(1));
    for (auto id : ids)
        append(raw, id);
    append(raw, uint8_t(1));
    for (auto v : strides)
        append(raw, v);
    append(raw, uint8_t(1));
    for (auto v : offsets)
        append(raw, v);
    return raw;
}
inline Capture iaSetterCapture(int lifetime = 0) {
    auto c = graphicsCounterCapture(false);
    c.buffer(60, 61, D3D11_BIND_VERTEX_BUFFER, 0, {});
    c.buffer(62, 63, D3D11_BIND_VERTEX_BUFFER, 0, {});
    c.buffer(66, 67, D3D11_BIND_INDEX_BUFFER, 0, {});
    c.add(75, 7, 0x242, statePack(Id(0), Id(1)));
    c.add(80, 7, 0x34f0, vertexSetter(0, {60}, {8}, {0}));
    c.add(90, 7, 0x34f0, vertexSetter(0, {60, 60}, {8, 8}, {0, 0}));
    c.add(91, 7, 0x34f1, statePack(Id(0), Id(1), Id(66), 57u, 2u));
    c.add(92, 7, 0x34ef, statePack(Id(0), Id(1), Id(0)));
    if (lifetime == 1) {
        c.add(220, 7, 0x34f0, vertexSetter(1, {60}, {8}, {0}));
        c.add(240, 7, 0x34f0, vertexSetter(0, {60}, {8}, {0}));
        c.add(250, 7, 0x34f1, statePack(Id(0), Id(1), Id(0), 0u, 0u));
        c.add(260, 7, 0x34ef, statePack(Id(0), Id(1), Id(0)));
    }
    if (lifetime == 2)
        c.add(240, 7, 0x242, statePack(Id(0), Id(1)));
    c.add(300, 7, 0x37, statePack(Id(34), Id(0), Id(1), 3u, 0u));
    return c;
}
} // namespace flora::testing
