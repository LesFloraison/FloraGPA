#include "RasterizerEdits.h"
#include "BlendEdits.h"
#include "core/Contexts.h"
#include <cmath>
#include <set>
namespace flora {
using Json = nlohmann::json;
namespace {
const std::set<std::string> booleans{"front_counter_clockwise", "depth_clip_enable", "scissor_enable",
                                     "multisample_enable", "antialiased_line_enable"};
int64_t integer(const Json &value, int64_t low, int64_t high) {
    if ((!value.is_number_integer() && !value.is_number_unsigned()) ||
        (value.is_number_unsigned() && value.get<uint64_t>() > uint64_t(INT64_MAX)))
        throw std::runtime_error("Rasterizer field requires an integer");
    auto v = value.get<int64_t>();
    if (v < low || v > high)
        throw std::runtime_error("Rasterizer integer is out of range");
    return v;
}
float real(const Json &value) {
    if (!value.is_number())
        throw std::runtime_error("Rasterizer field requires finite float32");
    const float out = value.get<float>();
    if (!std::isfinite(out))
        throw std::runtime_error("Rasterizer field exceeds finite float32");
    return out;
}
bool boolean(const Json &value) {
    if (!value.is_boolean())
        throw std::runtime_error("Rasterizer flag requires a boolean");
    return value.get<bool>();
}
State graphics(const Frame &frame, Id event) {
    auto e = frame.event(event);
    if (!isDraw(e.type) || e.type == 0x35 || e.type == 0x36)
        throw std::runtime_error("Graphics pipeline experiment on a dispatch or non-draw");
    requireImmediateContext(frame, e.context);
    return frame.state(e.state);
}
} // namespace
Json depthPipelineFields(const Json &values) {
    Json out = Json::object();
    for (const auto key : {"depth_stencil", "stencil_ref", "depth_test", "depth_write"})
        if (values.contains(key))
            out[key] = values[key];
    return out;
}
Json normalizePipeline(const Json &values) {
    if (!values.is_object() || values.empty())
        throw std::runtime_error("Empty pipeline experiment");
    const std::set<std::string> allowed{"depth_stencil", "stencil_ref", "depth_test",   "depth_write",
                                        "rasterizer",    "wireframe",   "cull_none",    "viewports",
                                        "scissors",      "blend_state", "blend_factor", "sample_mask",
                                        "blend_disabled"};
    for (auto it = values.begin(); it != values.end(); ++it)
        if (!allowed.contains(it.key()))
            throw std::runtime_error("Pipeline field migration pending: " + it.key());
    auto depth = depthPipelineFields(values);
    Json out = depth.empty() ? Json::object() : normalizeDepthStencil(depth);
    out.update(normalizeBlend(values));
    auto rs = values.value("rasterizer", Json::object());
    if (!rs.is_object() || (values.contains("rasterizer") && rs.empty()))
        throw std::runtime_error("Unknown or empty rasterizer fields");
    for (const auto &[alias, key] :
         std::map<std::string, std::string>{{"wireframe", "fill_mode"}, {"cull_none", "cull_mode"}})
        if (values.contains(alias) && rs.contains(key))
            throw std::runtime_error("Use either preset or explicit rasterizer field");
    if (values.contains("wireframe"))
        rs["fill_mode"] = boolean(values["wireframe"]) ? 2 : 3;
    for (auto it = rs.begin(); it != rs.end(); ++it) {
        const auto &key = it.key();
        if (booleans.contains(key))
            boolean(it.value());
        else if (key == "depth_bias_clamp" || key == "slope_scaled_depth_bias")
            it.value() = real(it.value());
        else if (key == "depth_bias")
            integer(it.value(), INT32_MIN, INT32_MAX);
        else if (key == "fill_mode")
            integer(it.value(), 2, 3);
        else if (key == "cull_mode")
            integer(it.value(), 1, 3);
        else if (key == "conservative_raster")
            integer(it.value(), 0, 1);
        else if (key == "forced_sample_count") {
            auto v = integer(it.value(), 0, 16);
            if (v && v != 1 && v != 2 && v != 4 && v != 8 && v != 16)
                throw std::runtime_error("Invalid forced sample count");
        } else
            throw std::runtime_error("Unknown rasterizer field: " + key);
    }
    if (values.contains("cull_none"))
        rs["cull_mode"] = boolean(values["cull_none"]) ? Json(1) : Json(nullptr);
    if (!rs.empty())
        out["rasterizer"] = rs;
    for (const auto key : {"viewports", "scissors"}) {
        if (!values.contains(key))
            continue;
        const bool viewport = std::string(key) == "viewports";
        const auto &rows = values[key];
        if (!rows.is_array() || rows.size() > 16)
            throw std::runtime_error("Rasterizer array requires 0 to 16 rows");
        auto converted = Json::array();
        for (const auto &row : rows) {
            if (!row.is_array() || row.size() != (viewport ? 6u : 4u))
                throw std::runtime_error("Rasterizer array row width mismatch");
            Json r = Json::array();
            for (const auto &v : row)
                r.push_back(viewport ? Json(real(v)) : Json(integer(v, INT32_MIN, INT32_MAX)));
            if (viewport) {
                double x = r[0], y = r[1], w = r[2], h = r[3], minDepth = r[4], maxDepth = r[5];
                if (!(x >= -32768 && x <= 32767 && y >= -32768 && y <= 32767 && w >= 0 && h >= 0 &&
                      x + w <= 32767 && y + h <= 32767 && minDepth >= 0 && minDepth <= maxDepth &&
                      maxDepth <= 1))
                    throw std::runtime_error("Viewport exceeds D3D11 bounds or depth range");
            } else if (r[2].get<int64_t>() < r[0].get<int64_t>() || r[3].get<int64_t>() < r[1].get<int64_t>())
                throw std::runtime_error("Scissor right/bottom precedes left/top");
            converted.push_back(r);
        }
        out[key] = converted;
    }
    return out;
}
void mergePipeline(Json &base, const Json &patch) {
    mergeDepthStencil(base, depthPipelineFields(patch));
    mergeBlend(base, patch);
    if (patch.contains("rasterizer"))
        for (auto it = patch["rasterizer"].begin(); it != patch["rasterizer"].end(); ++it)
            base["rasterizer"][it.key()] = it.value();
    for (const auto key : {"viewports", "scissors"})
        if (patch.contains(key))
            base[key] = patch[key];
}
Json capturedRasterizer(const Frame &frame, Id event) {
    auto s = graphics(frame, event);
    D3D11_RASTERIZER_DESC2 d{};
    d.FillMode = D3D11_FILL_SOLID;
    d.CullMode = D3D11_CULL_BACK;
    d.DepthClipEnable = TRUE;
    if (s.rasterizer) {
        auto &e = frame.entry(s.rasterizer);
        auto size = e.type == 0x89 ? 40u : e.type == 0x10e ? 44u : e.type == 0x10f ? 48u : 0u;
        if (e.category != 5 || !size)
            throw std::runtime_error("Not a rasterizer resource");
        Reader r(frame.payload(s.rasterizer));
        r.skip(16);
        auto bytes = r.take(size);
        r.end();
        std::memcpy(&d, bytes.data(), size);
    }
    Json out{{"rasterizer",
              {{"fill_mode", d.FillMode},
               {"cull_mode", d.CullMode},
               {"front_counter_clockwise", bool(d.FrontCounterClockwise)},
               {"depth_bias", d.DepthBias},
               {"depth_bias_clamp", d.DepthBiasClamp},
               {"slope_scaled_depth_bias", d.SlopeScaledDepthBias},
               {"depth_clip_enable", bool(d.DepthClipEnable)},
               {"scissor_enable", bool(d.ScissorEnable)},
               {"multisample_enable", bool(d.MultisampleEnable)},
               {"antialiased_line_enable", bool(d.AntialiasedLineEnable)},
               {"forced_sample_count", d.ForcedSampleCount},
               {"conservative_raster", d.ConservativeRaster}}}};
    for (const auto key : {"viewports", "scissors"}) {
        bool viewport = std::string(key) == "viewports";
        Id id = viewport ? s.viewports : s.scissors;
        out[key] = Json::array();
        if (!id)
            continue;
        Reader r(frame.payload(id, 9, viewport ? 0x87 : 0x86));
        auto n = r.read<uint32_t>();
        if (n > 16)
            throw std::runtime_error("Captured rasterizer array count exceeds 16");
        for (uint32_t i = 0; i < n; ++i) {
            Json row = Json::array();
            for (int k = 0; k < (viewport ? 6 : 4); ++k)
                row.push_back(viewport ? Json(r.read<float>()) : Json(r.read<int32_t>()));
            out[key].push_back(row);
        }
        r.end();
    }
    return out;
}
Json effectiveRasterizer(const Frame &frame, Id event, const Json &normalized) {
    auto out = capturedRasterizer(frame, event);
    if (normalized.contains("rasterizer"))
        for (auto it = normalized["rasterizer"].begin(); it != normalized["rasterizer"].end(); ++it)
            if (!it.value().is_null())
                out["rasterizer"][it.key()] = it.value();
    for (const auto key : {"viewports", "scissors"})
        if (normalized.contains(key))
            out[key] = normalized[key];
    if (out["rasterizer"]["conservative_raster"] != 0 && out["rasterizer"]["fill_mode"] != 3)
        throw std::runtime_error("Conservative rasterization requires solid fill");
    return out;
}
ReplayOptions::RasterizerEdit rasterizerEdit(const Frame &frame, Id event, const Json &normalized) {
    const auto values = effectiveRasterizer(frame, event, normalized);
    ReplayOptions::RasterizerEdit out;
    if (normalized.contains("rasterizer")) {
        const auto &r = values["rasterizer"];
        D3D11_RASTERIZER_DESC2 d{};
        d.FillMode = D3D11_FILL_MODE(r["fill_mode"].get<UINT>());
        d.CullMode = D3D11_CULL_MODE(r["cull_mode"].get<UINT>());
        d.FrontCounterClockwise = r["front_counter_clockwise"].get<bool>();
        d.DepthBias = r["depth_bias"].get<int32_t>();
        d.DepthBiasClamp = r["depth_bias_clamp"].get<float>();
        d.SlopeScaledDepthBias = r["slope_scaled_depth_bias"].get<float>();
        d.DepthClipEnable = r["depth_clip_enable"].get<bool>();
        d.ScissorEnable = r["scissor_enable"].get<bool>();
        d.MultisampleEnable = r["multisample_enable"].get<bool>();
        d.AntialiasedLineEnable = r["antialiased_line_enable"].get<bool>();
        d.ForcedSampleCount = r["forced_sample_count"].get<UINT>();
        d.ConservativeRaster = D3D11_CONSERVATIVE_RASTERIZATION_MODE(r["conservative_raster"].get<UINT>());
        out.descriptor = d;
    }
    if (normalized.contains("viewports")) {
        out.viewports.emplace();
        for (const auto &r : values["viewports"])
            out.viewports->push_back({r[0], r[1], r[2], r[3], r[4], r[5]});
    }
    if (normalized.contains("scissors")) {
        out.scissors.emplace();
        for (const auto &r : values["scissors"])
            out.scissors->push_back({r[0], r[1], r[2], r[3]});
    }
    return out;
}
} // namespace flora
