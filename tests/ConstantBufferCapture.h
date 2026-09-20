#pragma once
#include "StateCapture.h"
namespace flora::testing {
inline std::vector<uint8_t> cbSetter(uint16_t type, uint32_t start, const std::vector<Id> &ids,
                                     std::optional<std::vector<uint32_t>> first = {},
                                     std::optional<std::vector<uint32_t>> count = {}) {
    auto raw = statePack(Id(0), Id(1), start, uint32_t(ids.size()), uint8_t(1));
    for (auto id : ids)
        append(raw, id);
    if (type >= 0x24f && type <= 0x254)
        for (const auto *values : {&first, &count}) {
            append(raw, uint8_t(values->has_value()));
            if (*values)
                for (auto value : **values)
                    append(raw, value);
        }
    return raw;
}
inline Capture constantBufferCapture(unsigned stage = 4, unsigned encoding = 2, int lifetime = 0) {
    auto c = graphicsCounterCapture(false);
    const char source[] = "cbuffer A:register(b2){float4 a;} cbuffer B:register(b3){float4 b;} float4 "
                          "main():SV_Target{return a+2*b;}";
    Com<ID3DBlob> binary;
    check(D3DCompile(source, sizeof(source) - 1, nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0, &binary,
                     nullptr),
          "Compile CB fixture");
    std::erase_if(c.entries, [](auto &e) { return e.id == 33; });
    auto code = statePack(uint64_t(binary->GetBufferSize()));
    auto begin = static_cast<const uint8_t *>(binary->GetBufferPointer());
    code.insert(code.end(), begin, begin + binary->GetBufferSize());
    append(code, Id(0));
    c.add(33, 9, 0x81, code);
    for (Id id : {60, 62}) {
        c.add(id, 5, 0x83,
              statePack(Id(0), Id(0), D3D11_BUFFER_DESC{512, D3D11_USAGE_DEFAULT, 4, 0, 0, 0}, id + 1));
        auto data = statePack(512u);
        data.resize(516);
        put(data, 4 + (id == 60 ? 0 : 4), id == 60 ? .125f : .1875f);
        put(data, 260 + (id == 60 ? 0 : 4), id == 60 ? .25f : .0625f);
        c.add(id + 1, 9, 1, data);
    }
    auto type = uint16_t(encoding == 0   ? 0x249 + stage
                         : encoding == 1 ? constantBufferShimTypes[stage]
                                         : 0x24f + stage);
    c.add(75, 7, 0x242, statePack(Id(0), Id(1)));
    c.add(
        80, 7, uint16_t(0x24f + stage),
        cbSetter(uint16_t(0x24f + stage), 2, {60}, {std::vector<uint32_t>{16}}, {std::vector<uint32_t>{16}}));
    c.add(90, 7, type,
          cbSetter(type, 2, {0, 0}, encoding == 2 ? std::optional(std::vector<uint32_t>{0, 0}) : std::nullopt,
                   encoding == 2 ? std::optional(std::vector<uint32_t>{16, 16}) : std::nullopt));
    c.add(95, 7, uint16_t(0x24f + stage), cbSetter(uint16_t(0x24f + stage), 13, {0}));
    if (lifetime == 1)
        c.add(220, 7, constantBufferShimTypes[stage], cbSetter(constantBufferShimTypes[stage], 2, {0, 0}));
    if (lifetime == 2)
        c.add(220, 7, 0x242, statePack(Id(0), Id(1)));
    c.add(300, 7, 0x37, statePack(Id(34), Id(0), Id(1), 3u, 0u));
    return c;
}
} // namespace flora::testing
