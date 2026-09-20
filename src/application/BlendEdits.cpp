#include "BlendEdits.h"
#include "core/Contexts.h"
#include "replay/BlendState.h"
#include <cmath>
namespace flora {
using Json = nlohmann::json;
namespace {
uint32_t integer(const Json &v, uint32_t low, uint32_t high) {
    if ((!v.is_number_integer() && !v.is_number_unsigned()) ||
        (v.is_number_integer() && !v.is_number_unsigned() && v.get<int64_t>() < 0) ||
        v.get<uint64_t>() < low || v.get<uint64_t>() > high)
        throw std::runtime_error("Invalid blend integer");
    return v.get<uint32_t>();
}
void boolean(const Json &v) {
    if (!v.is_boolean())
        throw std::runtime_error("Blend flag requires a boolean");
}
void validateCombination(const Json &blend) {
    for (const auto &[slot, row] : blend.at("targets").items())
        if ((blend.at("independent_blend").get<bool>() || slot == "0") &&
            row.at("logic_op_enable").get<bool>() &&
            (row.at("blend_enable").get<bool>() || blend.at("independent_blend").get<bool>()))
            throw std::runtime_error("Logic ops require blending disabled and independent_blend=false");
}
} // namespace
Json normalizeBlend(const Json &values) {
    Json out = Json::object();
    auto blend = values.value("blend_state", Json::object());
    if (!blend.is_object() || (values.contains("blend_state") && blend.empty()))
        throw std::runtime_error("Unknown or empty blend_state fields");
    for (const auto &[key, value] : blend.items()) {
        if (key == "alpha_to_coverage" || key == "independent_blend")
            boolean(value);
        else if (key == "targets") {
            if (!value.is_object() || value.empty())
                throw std::runtime_error("Blend targets require string slots 0 to 7");
            for (const auto &[slot, row] : value.items()) {
                if (slot.size() != 1 || slot[0] < '0' || slot[0] > '7' || !row.is_object() || row.empty())
                    throw std::runtime_error("Unknown or empty blend target fields");
                for (const auto &[field, v] : row.items()) {
                    if (field == "blend_enable" || field == "logic_op_enable")
                        boolean(v);
                    else if (field == "src_blend" || field == "dest_blend" || field == "src_blend_alpha" ||
                             field == "dest_blend_alpha") {
                        auto n = integer(v, 1, 19);
                        if (n == 12 || n == 13 ||
                            (field.ends_with("_alpha") &&
                             (n == 3 || n == 4 || n == 9 || n == 10 || n == 16 || n == 17)))
                            throw std::runtime_error("Invalid blend factor");
                    } else if (field == "blend_op" || field == "blend_op_alpha")
                        integer(v, 1, 5);
                    else if (field == "logic_op" || field == "write_mask")
                        integer(v, 0, 15);
                    else
                        throw std::runtime_error("Unknown blend target field: " + field);
                }
            }
        } else
            throw std::runtime_error("Unknown blend state field: " + key);
    }
    if (values.contains("blend_disabled")) {
        if (!blend.empty())
            throw std::runtime_error("Use either blend_disabled or blend_state");
        boolean(values["blend_disabled"]);
        for (int i = 0; i < 8; ++i)
            blend["targets"][std::to_string(i)]["blend_enable"] =
                values["blend_disabled"].get<bool>() ? Json(false) : Json(nullptr);
    }
    if (!blend.empty())
        out["blend_state"] = blend;
    if (values.contains("blend_factor")) {
        const auto &row = values["blend_factor"];
        if (!row.is_array() || row.size() != 4)
            throw std::runtime_error("Blend factor requires four float32 components");
        out["blend_factor"] = Json::array();
        for (const auto &v : row) {
            if (!v.is_number())
                throw std::runtime_error("Blend factor requires finite float32");
            float x = v.get<float>();
            if (!std::isfinite(x) || x < 0 || x > 1)
                throw std::runtime_error("Blend factor must be in [0,1]");
            out["blend_factor"].push_back(x);
        }
    }
    if (values.contains("sample_mask"))
        out["sample_mask"] = integer(values["sample_mask"], 0, UINT32_MAX);
    return out;
}
void mergeBlend(Json &base, const Json &patch) {
    if (patch.contains("blend_state"))
        for (const auto &[key, value] : patch["blend_state"].items()) {
            if (key == "targets") {
                for (const auto &[slot, row] : value.items())
                    for (const auto &[field, v] : row.items())
                        base["blend_state"]["targets"][slot][field] = v;
            } else
                base["blend_state"][key] = value;
        }
    for (const auto key : {"blend_factor", "sample_mask"})
        if (patch.contains(key))
            base[key] = patch[key];
}
Json capturedBlend(const Frame &frame, Id event, const State *base) {
    auto e = frame.event(event);
    if (!isDraw(e.type) || e.type == 0x35 || e.type == 0x36)
        throw std::runtime_error("Graphics pipeline experiment on a dispatch or non-draw");
    requireImmediateContext(frame, e.context);
    const auto s = base ? *base : frame.state(e.state);
    auto d = defaultBlend();
    if (s.blend) {
        const auto &entry = frame.entry(s.blend);
        if (entry.category != 5 || (entry.type != 0x8a && entry.type != 0x10d))
            throw std::runtime_error("Not a blend state resource");
        Reader r(frame.payload(s.blend));
        r.skip(16);
        d = decodeBlend(r.take(r.remaining()), entry.type == 0x10d);
    }
    Json targets = Json::object();
    for (size_t i = 0; i < 8; ++i) {
        const auto &rt = d.RenderTarget[i];
        targets[std::to_string(i)] = {{"blend_enable", bool(rt.BlendEnable)},
                                      {"logic_op_enable", bool(rt.LogicOpEnable)},
                                      {"src_blend", rt.SrcBlend},
                                      {"dest_blend", rt.DestBlend},
                                      {"blend_op", rt.BlendOp},
                                      {"src_blend_alpha", rt.SrcBlendAlpha},
                                      {"dest_blend_alpha", rt.DestBlendAlpha},
                                      {"blend_op_alpha", rt.BlendOpAlpha},
                                      {"logic_op", rt.LogicOp},
                                      {"write_mask", rt.RenderTargetWriteMask}};
    }
    return {{"blend_state",
             {{"alpha_to_coverage", bool(d.AlphaToCoverageEnable)},
              {"independent_blend", bool(d.IndependentBlendEnable)},
              {"targets", targets}}},
            {"blend_factor", s.blendFactor},
            {"sample_mask", s.sampleMask}};
}
Json effectiveBlend(const Frame &frame, Id event, const Json &normalized, const State *base) {
    const auto captured = capturedBlend(frame, event, base);
    auto result = captured;
    mergeBlend(result, normalized);
    for (auto &[slot, row] : result["blend_state"]["targets"].items())
        if (row["blend_enable"].is_null())
            row["blend_enable"] = captured["blend_state"]["targets"][slot]["blend_enable"];
    validateCombination(result["blend_state"]);
    return result;
}
ReplayOptions::BlendEdit blendEdit(const Frame &frame, Id event, const Json &normalized, const State *base) {
    auto values = effectiveBlend(frame, event, normalized, base);
    ReplayOptions::BlendEdit result;
    if (normalized.contains("blend_state")) {
        auto &b = values["blend_state"];
        auto d = defaultBlend();
        d.AlphaToCoverageEnable = b["alpha_to_coverage"].get<bool>();
        d.IndependentBlendEnable = b["independent_blend"].get<bool>();
        for (size_t i = 0; i < 8; ++i) {
            const auto &v = b["targets"][std::to_string(i)];
            d.RenderTarget[i] = {v["blend_enable"].get<bool>(),
                                 v["logic_op_enable"].get<bool>(),
                                 D3D11_BLEND(v["src_blend"].get<UINT>()),
                                 D3D11_BLEND(v["dest_blend"].get<UINT>()),
                                 D3D11_BLEND_OP(v["blend_op"].get<UINT>()),
                                 D3D11_BLEND(v["src_blend_alpha"].get<UINT>()),
                                 D3D11_BLEND(v["dest_blend_alpha"].get<UINT>()),
                                 D3D11_BLEND_OP(v["blend_op_alpha"].get<UINT>()),
                                 D3D11_LOGIC_OP(v["logic_op"].get<UINT>()),
                                 v["write_mask"].get<UINT8>()};
        }
        result.descriptor = d;
    }
    if (normalized.contains("blend_factor"))
        result.factor = values["blend_factor"].get<std::array<float, 4>>();
    if (normalized.contains("sample_mask"))
        result.sampleMask = values["sample_mask"].get<uint32_t>();
    return result;
}
} // namespace flora
