#include "DepthStencilEdits.h"
#include "core/Contexts.h"
namespace flora {
using Json = nlohmann::json;
namespace {
constexpr const char *faces[]{"front_face", "back_face"};
constexpr const char *fields[]{"fail_op", "depth_fail_op", "pass_op", "func"};
uint32_t integer(const Json &v, uint32_t low, uint32_t high) {
    if ((!v.is_number_unsigned() && (!v.is_number_integer() || v.get<int64_t>() < 0)) ||
        v.get<uint64_t>() < low || v.get<uint64_t>() > high)
        throw std::runtime_error("Depth/stencil field is outside its unsigned range");
    return v.get<uint32_t>();
}
bool boolean(const Json &v) {
    if (!v.is_boolean())
        throw std::runtime_error("Depth/stencil enable field must be boolean");
    return v.get<bool>();
}
State graphicsState(const Frame &frame, Id event) {
    auto e = frame.event(event);
    if (!isDraw(e.type) || e.type == 0x35 || e.type == 0x36)
        throw std::runtime_error("Select a graphics draw for depth/stencil editing");
    requireImmediateContext(frame, e.context);
    return frame.state(e.state);
}
Json face(const D3D11_DEPTH_STENCILOP_DESC &f) {
    return {{"fail_op", f.StencilFailOp},
            {"depth_fail_op", f.StencilDepthFailOp},
            {"pass_op", f.StencilPassOp},
            {"func", f.StencilFunc}};
}
D3D11_DEPTH_STENCILOP_DESC packedFace(const Json &f) {
    return {D3D11_STENCIL_OP(f.at("fail_op").get<uint32_t>()),
            D3D11_STENCIL_OP(f.at("depth_fail_op").get<uint32_t>()),
            D3D11_STENCIL_OP(f.at("pass_op").get<uint32_t>()),
            D3D11_COMPARISON_FUNC(f.at("func").get<uint32_t>())};
}
} // namespace
Json normalizeDepthStencil(const Json &values) {
    if (!values.is_object() || values.empty())
        throw std::runtime_error("Empty pipeline edit");
    for (auto it = values.begin(); it != values.end(); ++it)
        if (it.key() != "depth_stencil" && it.key() != "stencil_ref" && it.key() != "depth_test" &&
            it.key() != "depth_write")
            throw std::runtime_error("Pipeline field migration pending: " + it.key());
    auto ds = values.value("depth_stencil", Json::object());
    if (!ds.is_object() || (values.contains("depth_stencil") && ds.empty()))
        throw std::runtime_error("Unknown or empty depth/stencil fields");
    for (const auto &[alias, key] : std::map<std::string, std::string>{{"depth_test", "depth_enable"},
                                                                       {"depth_write", "depth_write_mask"}})
        if (values.contains(alias)) {
            if (ds.contains(key))
                throw std::runtime_error("Use either preset or explicit field");
            bool v = boolean(values.at(alias));
            ds[key] = alias == "depth_test" ? Json(v) : Json(uint32_t(v));
        }
    for (auto it = ds.begin(); it != ds.end(); ++it) {
        const auto &key = it.key();
        if (key == "depth_enable" || key == "stencil_enable")
            boolean(it.value());
        else if (key == "front_face" || key == "back_face") {
            auto &f = it.value();
            if (!f.is_object() || f.empty())
                throw std::runtime_error("Empty stencil face");
            for (auto field = f.begin(); field != f.end(); ++field) {
                if (std::find(std::begin(fields), std::end(fields), field.key()) == std::end(fields))
                    throw std::runtime_error("Unknown stencil face field");
                integer(field.value(), 1, 8);
            }
        } else if (key == "depth_func")
            integer(it.value(), 1, 8);
        else if (key == "depth_write_mask")
            integer(it.value(), 0, 1);
        else if (key == "stencil_read_mask" || key == "stencil_write_mask")
            integer(it.value(), 0, 255);
        else
            throw std::runtime_error("Unknown depth/stencil field: " + key);
    }
    Json out = Json::object();
    if (!ds.empty())
        out["depth_stencil"] = ds;
    if (values.contains("stencil_ref"))
        out["stencil_ref"] = integer(values.at("stencil_ref"), 0, UINT32_MAX);
    return out;
}
void mergeDepthStencil(Json &base, const Json &patch) {
    for (auto it = patch.begin(); it != patch.end(); ++it) {
        if (it.key() != "depth_stencil") {
            base[it.key()] = it.value();
            continue;
        }
        for (auto field = it.value().begin(); field != it.value().end(); ++field)
            if (field.key() == "front_face" || field.key() == "back_face")
                for (auto op = field.value().begin(); op != field.value().end(); ++op)
                    base["depth_stencil"][field.key()][op.key()] = op.value();
            else
                base["depth_stencil"][field.key()] = field.value();
    }
}
Json capturedDepthStencil(const Frame &frame, Id event, const State *base) {
    auto s = graphicsState(frame, event);
    if (base)
        s = *base;
    D3D11_DEPTH_STENCIL_DESC d{};
    d.DepthEnable = TRUE;
    d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    d.DepthFunc = D3D11_COMPARISON_LESS;
    d.StencilReadMask = d.StencilWriteMask = 255;
    d.FrontFace = d.BackFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP,
                                D3D11_COMPARISON_ALWAYS};
    if (s.depthState) {
        Reader r(frame.payload(s.depthState, 5, 0x8b));
        r.skip(16);
        d = r.read<D3D11_DEPTH_STENCIL_DESC>();
        r.end();
    }
    return {{"depth_stencil",
             {{"depth_enable", bool(d.DepthEnable)},
              {"depth_write_mask", d.DepthWriteMask},
              {"depth_func", d.DepthFunc},
              {"stencil_enable", bool(d.StencilEnable)},
              {"stencil_read_mask", d.StencilReadMask},
              {"stencil_write_mask", d.StencilWriteMask},
              {"front_face", face(d.FrontFace)},
              {"back_face", face(d.BackFace)}}},
            {"stencil_ref", s.stencilRef}};
}
Json effectiveDepthStencil(const Frame &frame, Id event, const Json &normalized, const State *base) {
    auto result = capturedDepthStencil(frame, event, base);
    mergeDepthStencil(result, normalized);
    return result;
}
ReplayOptions::DepthStencilEdit depthStencilEdit(const Frame &frame, Id event, const Json &normalized,
                                                 const State *base) {
    auto result = effectiveDepthStencil(frame, event, normalized, base);
    ReplayOptions::DepthStencilEdit out;
    if (normalized.contains("stencil_ref"))
        out.reference = result["stencil_ref"].get<uint32_t>();
    if (normalized.contains("depth_stencil")) {
        const auto &s = result["depth_stencil"];
        D3D11_DEPTH_STENCIL_DESC d{};
        d.DepthEnable = s["depth_enable"].get<bool>();
        d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK(s["depth_write_mask"].get<uint32_t>());
        d.DepthFunc = D3D11_COMPARISON_FUNC(s["depth_func"].get<uint32_t>());
        d.StencilEnable = s["stencil_enable"].get<bool>();
        d.StencilReadMask = s["stencil_read_mask"].get<uint8_t>();
        d.StencilWriteMask = s["stencil_write_mask"].get<uint8_t>();
        d.FrontFace = packedFace(s["front_face"]);
        d.BackFace = packedFace(s["back_face"]);
        out.descriptor = d;
    }
    return out;
}
} // namespace flora
