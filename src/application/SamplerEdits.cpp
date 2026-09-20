#include "SamplerEdits.h"
#include "core/Contexts.h"
#include <algorithm>
#include <cmath>
#include <limits>
namespace flora {
using Json = nlohmann::json;
namespace {
unsigned integer(const Json &value, unsigned low, unsigned high) {
    if ((!value.is_number_unsigned() && (!value.is_number_integer() || value.get<int64_t>() < 0)) ||
        value.get<uint64_t>() < low || value.get<uint64_t>() > high)
        throw std::runtime_error("Sampler integer outside its valid range");
    return value.get<unsigned>();
}
float floating(const Json &v) {
    if (!v.is_number() || !std::isfinite(v.get<double>()) || !std::isfinite(v.get<float>()))
        throw std::runtime_error("Sampler value requires a finite float32");
    return v.get<float>();
}
} // namespace
unsigned samplerStage(const std::string &name) {
    for (unsigned i = 0; i < 6; ++i)
        if (name == samplerStageNames[i])
            return i;
    throw std::runtime_error("Sampler stage must be vs/hs/ds/gs/ps/cs");
}
unsigned samplerSlot(const Json &value) { return integer(value, 0, 15); }
Json normalizeSampler(const Json &values) {
    if (!values.is_object() || values.empty())
        throw std::runtime_error("Empty sampler descriptor fields");
    Json result = values;
    for (const auto &[key, v] : values.items()) {
        if (key == "filter") {
            auto filter = integer(v, 0, 469);
            const auto base = filter & ~384u;
            constexpr unsigned bases[]{0, 1, 4, 5, 16, 17, 20, 21, 85};
            if (std::find(std::begin(bases), std::end(bases), base) == std::end(bases))
                throw std::runtime_error("Invalid sampler filter enum");
        } else if (key == "address_u" || key == "address_v" || key == "address_w")
            integer(v, 1, 5);
        else if (key == "comparison_func")
            integer(v, 1, 8);
        else if (key == "max_anisotropy")
            integer(v, 0, 16);
        else if (key == "border_color") {
            if (!v.is_array() || v.size() != 4)
                throw std::runtime_error("Sampler border color requires four components");
            for (unsigned i = 0; i < 4; ++i) {
                const auto f = floating(v[i]);
                if (f < 0 || f > 1)
                    throw std::runtime_error("Sampler border color must be in [0,1]");
                result[key][i] = f;
            }
        } else if (key == "mip_lod_bias" || key == "min_lod" || key == "max_lod") {
            auto f = floating(v);
            if (key == "mip_lod_bias" && (f < -16 || f > float(15.99)))
                throw std::runtime_error("Sampler LOD bias must be in [-16,15.99]");
            result[key] = f;
        } else
            throw std::runtime_error("Unknown sampler field: " + key);
    }
    if (result.contains("min_lod") && result.contains("max_lod") && result["min_lod"] > result["max_lod"])
        throw std::runtime_error("Sampler min_lod must not exceed max_lod");
    if ((result.value("filter", 0u) & 64) && result.value("max_anisotropy", 1u) == 0)
        throw std::runtime_error("Anisotropic filtering requires max_anisotropy in [1,16]");
    return result;
}
Json samplerDescriptor(const Frame &frame, Id resource) {
    if (!resource)
        return {{"filter", 21},
                {"address_u", 3},
                {"address_v", 3},
                {"address_w", 3},
                {"mip_lod_bias", 0.f},
                {"max_anisotropy", 1},
                {"comparison_func", 1},
                {"border_color", {1.f, 1.f, 1.f, 1.f}},
                {"min_lod", -std::numeric_limits<float>::max()},
                {"max_lod", std::numeric_limits<float>::max()}};
    validateSamplerResource(frame, resource);
    Reader r(frame.payload(resource));
    r.skip(16);
    Json result;
    for (auto key : {"filter", "address_u", "address_v", "address_w"})
        result[key] = r.read<uint32_t>();
    result["mip_lod_bias"] = r.read<float>();
    result["max_anisotropy"] = r.read<uint32_t>();
    result["comparison_func"] = r.read<uint32_t>();
    result["border_color"] = r.array<float, 4>();
    result["min_lod"] = r.read<float>();
    result["max_lod"] = r.read<float>();
    r.end();
    return result;
}
D3D11_SAMPLER_DESC nativeSampler(const Json &values) {
    const auto v = normalizeSampler(values);
    D3D11_SAMPLER_DESC d{};
    d.Filter = D3D11_FILTER(v.at("filter").get<unsigned>());
    d.AddressU = D3D11_TEXTURE_ADDRESS_MODE(v.at("address_u").get<unsigned>());
    d.AddressV = D3D11_TEXTURE_ADDRESS_MODE(v.at("address_v").get<unsigned>());
    d.AddressW = D3D11_TEXTURE_ADDRESS_MODE(v.at("address_w").get<unsigned>());
    d.MipLODBias = v.at("mip_lod_bias").get<float>();
    d.MaxAnisotropy = v.at("max_anisotropy").get<unsigned>();
    d.ComparisonFunc = D3D11_COMPARISON_FUNC(v.at("comparison_func").get<unsigned>());
    for (unsigned i = 0; i < 4; ++i)
        d.BorderColor[i] = v.at("border_color")[i].get<float>();
    d.MinLOD = v.at("min_lod").get<float>();
    d.MaxLOD = v.at("max_lod").get<float>();
    return d;
}
State samplerState(const Frame &frame, Id event, const std::map<Id, SamplerBinding> &setters) {
    const auto &entry = frame.entry(event);
    if (entry.category != 7 || !isDraw(entry.type))
        throw std::runtime_error("Select a draw or dispatch");
    const auto draw = frame.event(event);
    requireImmediateContext(frame, draw.context);
    return effectiveSamplerBindings(frame, event, frame.state(draw.state), setters);
}
} // namespace flora
