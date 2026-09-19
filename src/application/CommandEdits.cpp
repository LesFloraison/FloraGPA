#include "CommandEdits.h"
#include <cmath>

namespace flora {
using Json = nlohmann::json;
Json clearValues(uint16_t type, Bytes payload) {
    if (!isClearCommand(type))
        throw std::runtime_error("Select a supported clear command");
    Reader r(payload);
    r.skip(24);
    Json out;
    if (type == 0x31) {
        out = {{"flags", r.read<uint32_t>()}, {"depth", r.read<float>()}, {"stencil", r.read<uint8_t>()}};
    } else {
        if (!r.flag())
            throw std::runtime_error("Clear requires four captured values");
        out["values"] = Json::array();
        for (int i = 0; i < 4; ++i)
            if (type == 0x33)
                out["values"].push_back(r.read<uint32_t>());
            else
                out["values"].push_back(r.read<float>());
    }
    r.end();
    return out;
}
std::vector<uint8_t> patchClear(uint16_t type, Bytes payload, const Json &values) {
    auto original = clearValues(type, payload);
    if (!values.is_object() || values.size() != original.size())
        throw std::runtime_error("Clear edit fields must match the selected command");
    for (auto it = original.begin(); it != original.end(); ++it)
        if (!values.contains(it.key()))
            throw std::runtime_error("Missing clear edit field");
    auto integer = [](const Json &v, uint32_t max) -> uint32_t {
        if ((!v.is_number_unsigned() && (!v.is_number_integer() || v.get<int64_t>() < 0)) ||
            v.get<uint64_t>() > max)
            throw std::runtime_error("Clear integer is outside its unsigned range");
        return v.get<uint32_t>();
    };
    auto real = [](const Json &v) -> float {
        if (!v.is_number())
            throw std::runtime_error("Clear float must be numeric");
        auto result = v.get<float>();
        if (!std::isfinite(result))
            throw std::runtime_error("Clear float must be finite float32");
        return result;
    };
    std::vector<uint8_t> out(payload.begin(), payload.end());
    auto write = [&]<class T>(size_t offset, T value) {
        std::memcpy(out.data() + offset, &value, sizeof value);
    };
    if (type == 0x31) {
        auto flags = integer(values.at("flags"), 3);
        auto depth = real(values.at("depth"));
        if (!flags || depth < 0 || depth > 1)
            throw std::runtime_error("Clear needs depth/stencil flags and depth between zero and one");
        write(24, flags);
        write(28, depth);
        write(32, uint8_t(integer(values.at("stencil"), 255)));
    } else {
        auto &items = values.at("values");
        if (!items.is_array() || items.size() != 4)
            throw std::runtime_error("Clear requires exactly four values");
        for (size_t i = 0; i < 4; ++i)
            if (type == 0x33)
                write(25 + i * 4, integer(items[i], UINT32_MAX));
            else
                write(25 + i * 4, real(items[i]));
    }
    return out;
}
} // namespace flora
