#pragma once
#include "ClassCapture.h"
namespace flora::testing {
inline Capture predicateCapture(bool visible = true, uint32_t value = 0, bool hint = false,
                                bool seed = false) {
    auto c = graphicsCounterCapture(false);
    c.entries.erase(std::remove_if(c.entries.begin(), c.entries.end(),
                                   [](const auto &e) { return e.category == 7 || e.id == 32 || e.id == 33; }),
                    c.entries.end());
    auto shader = [&](Id id, const std::string &text) {
        auto code = compileClassProgram(text, "ps_5_0");
        c.add(id, 5, 0x92, statePack(Id(0), Id(0), Id(0), Id(0), Id(0), Id(0), id + 1));
        auto raw = statePack(uint64_t(code.size()));
        raw.insert(raw.end(), code.begin(), code.end());
        append(raw, Id(0));
        c.add(id + 1, 9, 0x81, raw);
    };
    shader(32, "float4 main():SV_Target{return float4(1,0,0,1);}");
    shader(80, "float4 main():SV_Target{return float4(0,1,0,1);}");
    c.add(600, 5, 0x96, statePack(Id(0), Id(0), 5u, hint ? 1u : 0u));
    // This fixture includes a later append dispatch; specify its independent
    // initial counter rather than depending on an undefined native value.
    c.add(700, 7, 0x25e, statePack(Id(0), Id(1), 1u, 1u, uint8_t(1), Id(12), uint8_t(1), 0u));
    c.add(800, 7, 0x32, statePack(Id(0), Id(1), Id(21), uint8_t(1), 0.f, 0.f, 0.f, 1.f));
    if (!seed)
        c.add(900, 7, 0x241, statePack(Id(0), Id(1), Id(600)));
    State s{};
    s.topology = 4;
    s.sampleMask = UINT32_MAX;
    s.omStart = 8;
    s.rtCount = 1;
    s.rtv[0] = 21;
    s.viewports = 22;
    s.stages[0].shader = 30;
    s.stages[4].shader = 32;
    c.add(990, 3, 3, snapshot(s));
    c.add(1000, 7, 0x37, statePack(Id(990), Id(0), Id(1), visible ? 3u : 0u, 0u));
    if (!seed)
        c.add(1100, 7, 0x243, statePack(Id(0), Id(1), Id(600)));
    c.add(1200, 7, 0x248, statePack(Id(0), Id(1), Id(600), value));
    c.add(1300, 7, 0x32, statePack(Id(0), Id(1), Id(21), uint8_t(1), 0.f, 0.f, 1.f, 1.f));
    c.add(1400, 7, 0x3e, statePack(Id(0), Id(1), Id(7), Id(13)));
    s.predicate = 600;
    s.predicateValue = value;
    s.stages[4].shader = 80;
    c.add(1990, 3, 3, snapshot(s));
    c.add(2000, 7, 0x37, statePack(Id(1990), Id(0), Id(1), 3u, 0u));
    s = {};
    s.predicate = 600;
    s.predicateValue = value;
    s.stages[5].shader = 15;
    s.stages[5].cb[0] = 2;
    s.stages[5].srv[0] = 6;
    s.csCount = 2;
    s.csUav[0] = 9;
    s.csUav[1] = 12;
    c.add(2990, 3, 3, snapshot(s));
    c.add(3000, 7, 0x35, statePack(Id(2990), Id(0), Id(1), 1u, 1u, 1u));
    return c;
}
} // namespace flora::testing
