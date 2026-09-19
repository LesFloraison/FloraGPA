#pragma once
#include "SyntheticCapture.h"
namespace flora::testing {
template <class... T> std::vector<uint8_t> statePack(T... values) {
    std::vector<uint8_t> out;
    (append(out, values), ...);
    return out;
}
inline Capture stateCapture() {
    Capture c;
    c.add(1, 5, 0x127, statePack(Id(0), Id(0), 0u, 0u));
    c.add(2, 5, 0x99, statePack(Id(0), Id(0), 0u, 0u));
    c.add(3, 5, 0x99, statePack(Id(0), Id(0), 1u, 0u));
    c.add(5, 5, 0x85, statePack(Id(0), Id(0), 16u, 16u, 2u, 2u, 28u, 1u, 0u, 0u, 40u, 0u, 0u, Id(0)));
    c.add(6, 5, 0x8c, statePack(Id(0), Id(0), Id(5), 28u, 5u, 0u, 1u, 0u, 1u));
    c.add(7, 5, 0x8d, statePack(Id(0), Id(0), Id(5), 28u, 5u, 1u, 1u, 1u));
    c.add(8, 5, 0x8d, statePack(Id(0), Id(0), Id(5), 28u, 5u, 0u, 0u, 1u));
    c.add(9, 5, 0x8e, statePack(Id(0), Id(0), Id(5), 28u, 4u, 1u, 0u, 0u, 1u));
    c.buffer(20, 21, 8, 0, {1, 2, 3, 4});
    c.uav(22, 20, 0);
    c.add(23, 5, 0x8c, statePack(Id(0), Id(0), Id(20), 0u, 1u, 0u, 4u, 0u, 0u));
    c.add(24, 5, 0x86, statePack(Id(0), Id(0), 4u, 4u, 4u, 2u, 28u, 0u, 40u, 0u, 0u, Id(0)));
    c.add(25, 5, 0x8c, statePack(Id(0), Id(0), Id(24), 28u, 8u, 0u, 1u, 0u, 0u));
    c.add(26, 5, 0x8d, statePack(Id(0), Id(0), Id(24), 28u, 8u, 0u, 0u, 1u));
    c.add(27, 5, 0x8d, statePack(Id(0), Id(0), Id(24), 28u, 8u, 1u, 0u, 1u));
    auto command = [&](Id id, uint16_t type, std::vector<uint8_t> tail = {}, Id owner = 1) {
        auto raw = statePack(Id(0), owner);
        raw.insert(raw.end(), tail.begin(), tail.end());
        c.add(id, 7, type, raw);
    };
    command(100, 0x242);
    command(101, 0x34e9, statePack(Id(50), 0u, uint8_t(0)));
    command(102, 0x24f, statePack(0u, 1u, uint8_t(1), Id(20), uint8_t(1), 16u, uint8_t(1), 16u));
    State state{};
    state.sampleMask = UINT32_MAX;
    state.blendFactor.fill(1);
    state.stages[0].cb[0] = 20;
    state.stages[5].shader = 54;
    c.add(90, 3, 3, snapshot(state));
    c.add(103, 7, 0x35, statePack(Id(90), Id(0), Id(1), 1u, 1u, 1u));
    command(110, 0x34e6, statePack(0u, 1u, uint8_t(1), Id(6)));
    command(111, 0x34ff, statePack(1u, uint8_t(1), Id(7), Id(0)));
    command(112, 0x34ff, statePack(1u, uint8_t(1), Id(8), Id(0)));
    command(113, 0x34e6, statePack(0u, 1u, uint8_t(1), Id(6)));
    command(114, 0x34ff, statePack(0u, uint8_t(0), Id(0)));
    command(115, 0x34e6, statePack(0u, 1u, uint8_t(1), Id(6)));
    command(116, 0x34ff, statePack(0u, uint8_t(0), Id(9)));
    command(117, 0x242);
    command(118, 0x3503, statePack(1u, uint8_t(1), Id(20), uint8_t(0)));
    command(119, 0x34f0, statePack(0u, 1u, uint8_t(1), Id(20), uint8_t(1), 4u, uint8_t(1), 0u));
    command(120, 0x34e9, statePack(Id(60), 1u, uint8_t(1), Id(0x100000001)), 2);
    command(121, 0x7fff);
    command(122, 0x34e9, statePack(Id(51), 0u, uint8_t(0)));
    command(123, 0x242);
    command(124, 0x24f, statePack(0u, 1u, uint8_t(1), Id(20), uint8_t(1), 1u, uint8_t(1), 16u));
    command(125, 0x242);
    command(126, 0x24f, statePack(0u, 1u, uint8_t(1), Id(20), uint8_t(1), 0u, uint8_t(0)));
    command(127, 0x242);
    command(128, 0x249, statePack(14u, 1u, uint8_t(1), Id(20)));
    command(129, 0x242);
    command(130, 0x3550, statePack(0, 0u, Id(0)));
    command(131, 0x3550, statePack(0, 0u, Id(9)));
    command(132, 0x242);
    command(133, 0x3531, statePack(uint8_t(1), 4u));
    command(134, 0x327d, statePack(0u));
    command(135, 0x242, {}, 3);
    command(136, 0x242);
    command(137, 0x3501,
            statePack(Id(0), uint8_t(1), 0x7fc00000u, 0x7f800000u, 0xff800000u, 0x80000000u, UINT32_MAX));
    command(138, 0x3500,
            statePack(2u, uint8_t(1), Id(8), Id(8), Id(0), 0u, UINT32_MAX, uint8_t(0), uint8_t(0)));
    command(139, 0x242);
    command(140, 0x3522, statePack(63u, 1u, uint8_t(1), Id(22), uint8_t(0)));
    command(141, 0x34e6, statePack(0u, 1u, uint8_t(1), Id(23)));
    command(142, 0x3522, statePack(63u, 1u, uint8_t(1), Id(0), uint8_t(0)));
    command(143, 0x34f1, statePack(Id(20), 42u, 4u));
    command(144, 0x350a, statePack(1u, uint8_t(1), 0.f, 0.f, 4.f, 4.f, 0.f, 1.f));
    command(145, 0x350b, statePack(1u, uint8_t(1), -2, -3, 4, 5));
    command(146, 0x351a, statePack(Id(70), 1u, uint8_t(1), Id(0x100000001)));
    command(147, 0x351b, statePack(15u, 1u, uint8_t(1), Id(60)));
    command(148, 0x3519, statePack(127u, 1u, uint8_t(1), Id(6)));
    command(149, 0x250, statePack(13u, 1u, uint8_t(1), Id(20), uint8_t(1), 16u, uint8_t(1), 16u));
    command(150, 0x248, statePack(Id(99), 1u));
    command(151, 0x3501, statePack(Id(29), uint8_t(0), 0x12345678u));
    command(152, 0x3502, statePack(Id(30), 256u));
    command(153, 0x34ef, statePack(Id(31)));
    command(154, 0x34f6, statePack(4u));
    command(155, 0x3509, statePack(Id(32)));
    command(156, 0x249, statePack(0u, 1u, uint8_t(0)));
    command(157, 0x24f, statePack(14u, 1u, uint8_t(1), Id(20), uint8_t(1), 1u, uint8_t(1), 16u));
    command(158, 0x242);
    command(159, 0x249, statePack(0u, 1u, uint8_t(0)));
    command(160, 0x34e6, statePack(0u, 1u, uint8_t(1), Id(25)));
    command(161, 0x34ff, statePack(1u, uint8_t(1), Id(26), Id(0)));
    command(162, 0x242);
    command(163, 0x34e6, statePack(0u, 1u, uint8_t(1), Id(25)));
    command(164, 0x34ff, statePack(1u, uint8_t(1), Id(27), Id(0)));
    return c;
}
} // namespace flora::testing
