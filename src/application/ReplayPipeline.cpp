#include "ReplayPipeline.h"
#include "ApiCommands.h"
#include "CommandState.h"
#include <algorithm>
#include <cmath>

namespace flora {
namespace {
using Json = nlohmann::json;
Json finite(float value) {
    if (std::isfinite(value))
        return value;
    return std::isnan(value) ? "nan" : value > 0 ? "inf" : "-inf";
}
template <class T, size_t N> struct GetterArray {
    std::array<T *, N> values{};
    ~GetterArray() {
        for (auto p : values)
            if (p)
                p->Release();
    }
    T **data() { return values.data(); }
};
class Identities {
    struct Identity {
        Com<IUnknown> object;
        std::set<Id> ids;
        std::string token;
    };
    std::map<IUnknown *, Identity> objects_;
    size_t nextToken_ = 0;
    Identity &identity(IUnknown *object) {
        Com<IUnknown> canonical;
        check(object->QueryInterface(IID_PPV_ARGS(&canonical)), "Get native object identity");
        auto [it, inserted] = objects_.try_emplace(canonical.Get());
        if (inserted)
            it->second.object = std::move(canonical);
        return it->second;
    }

  public:
    void observe(const std::map<Id, Com<IUnknown>> &objects) {
        for (const auto &[id, object] : objects)
            if (object)
                identity(object.Get()).ids.insert(id);
    }
    std::pair<Json, Json> describe(IUnknown *object) {
        if (!object)
            return {0, nullptr};
        auto &entry = identity(object);
        if (entry.token.empty())
            entry.token = "object-" + std::to_string(++nextToken_);
        Json metadata{{"runtime_object", entry.token}, {"captured_ids", entry.ids}};
        return {entry.ids.size() == 1 ? Json(*entry.ids.begin()) : metadata, metadata};
    }
};
using Fields = std::map<UINT, std::vector<std::string>>;
const Fields srvFields{{1, {"first_element", "num_elements"}},
                       {2, {"most_detailed_mip", "mip_levels"}},
                       {3, {"most_detailed_mip", "mip_levels", "first_array_slice", "array_size"}},
                       {4, {"most_detailed_mip", "mip_levels"}},
                       {5, {"most_detailed_mip", "mip_levels", "first_array_slice", "array_size"}},
                       {6, {}},
                       {7, {"first_array_slice", "array_size"}},
                       {8, {"most_detailed_mip", "mip_levels"}},
                       {9, {"most_detailed_mip", "mip_levels"}},
                       {10, {"most_detailed_mip", "mip_levels", "first_2d_array_face", "num_cubes"}},
                       {11, {"first_element", "num_elements", "flags"}}};
const Fields rtvFields{{1, {"first_element", "num_elements"}},
                       {2, {"mip_slice"}},
                       {3, {"mip_slice", "first_array_slice", "array_size"}},
                       {4, {"mip_slice"}},
                       {5, {"mip_slice", "first_array_slice", "array_size"}},
                       {6, {}},
                       {7, {"first_array_slice", "array_size"}},
                       {8, {"mip_slice", "first_w_slice", "w_size"}}};
const Fields dsvFields{{1, {"mip_slice"}},
                       {2, {"mip_slice", "first_array_slice", "array_size"}},
                       {3, {"mip_slice"}},
                       {4, {"mip_slice", "first_array_slice", "array_size"}},
                       {5, {}},
                       {6, {"first_array_slice", "array_size"}}};
const Fields uavFields{
    {1, {"first_element", "num_elements", "flags"}},       {2, {"mip_slice"}},
    {3, {"mip_slice", "first_array_slice", "array_size"}}, {4, {"mip_slice"}},
    {5, {"mip_slice", "first_array_slice", "array_size"}}, {8, {"mip_slice", "first_w_slice", "w_size"}}};
template <class Descriptor>
Json viewDescriptor(const Descriptor &desc, const Fields &fields, bool dsv = false) {
    Reader r(Bytes(reinterpret_cast<const uint8_t *>(&desc), sizeof desc));
    auto format = r.read<UINT>(), dimension = r.read<UINT>();
    Json out{{"format", format}, {"dimension", dimension}};
    if (dsv)
        out["flags"] = r.read<UINT>();
    auto found = fields.find(dimension);
    if (found == fields.end())
        throw std::runtime_error("Unsupported native view dimension");
    for (const auto &key : found->second)
        out[key] = r.read<UINT>();
    return out;
}
Json descriptor(ID3D11ShaderResourceView *p) {
    D3D11_SHADER_RESOURCE_VIEW_DESC d{};
    p->GetDesc(&d);
    return viewDescriptor(d, srvFields);
}
Json descriptor(ID3D11RenderTargetView *p) {
    D3D11_RENDER_TARGET_VIEW_DESC d{};
    p->GetDesc(&d);
    return viewDescriptor(d, rtvFields);
}
Json descriptor(ID3D11DepthStencilView *p) {
    D3D11_DEPTH_STENCIL_VIEW_DESC d{};
    p->GetDesc(&d);
    return viewDescriptor(d, dsvFields, true);
}
Json descriptor(ID3D11UnorderedAccessView *p) {
    D3D11_UNORDERED_ACCESS_VIEW_DESC d{};
    p->GetDesc(&d);
    return viewDescriptor(d, uavFields);
}
Json descriptor(ID3D11SamplerState *p) {
    D3D11_SAMPLER_DESC d{};
    p->GetDesc(&d);
    return {{"filter", d.Filter},
            {"address_u", d.AddressU},
            {"address_v", d.AddressV},
            {"address_w", d.AddressW},
            {"mip_lod_bias", finite(d.MipLODBias)},
            {"max_anisotropy", d.MaxAnisotropy},
            {"comparison_func", d.ComparisonFunc},
            {"border_color",
             {finite(d.BorderColor[0]), finite(d.BorderColor[1]), finite(d.BorderColor[2]),
              finite(d.BorderColor[3])}},
            {"min_lod", finite(d.MinLOD)},
            {"max_lod", finite(d.MaxLOD)}};
}
Json descriptor(ID3D11RasterizerState *p) {
    D3D11_RASTERIZER_DESC2 d{};
    d.FillMode = D3D11_FILL_SOLID;
    d.CullMode = D3D11_CULL_BACK;
    d.DepthClipEnable = TRUE;
    if (p) {
        Com<ID3D11RasterizerState2> v2;
        Com<ID3D11RasterizerState1> v1;
        if (SUCCEEDED(p->QueryInterface(IID_PPV_ARGS(&v2))))
            v2->GetDesc2(&d);
        else if (SUCCEEDED(p->QueryInterface(IID_PPV_ARGS(&v1)))) {
            D3D11_RASTERIZER_DESC1 base{};
            v1->GetDesc1(&base);
            std::memcpy(&d, &base, sizeof base);
        } else {
            D3D11_RASTERIZER_DESC base{};
            p->GetDesc(&base);
            std::memcpy(&d, &base, sizeof base);
        }
    }
    auto flag = [&](BOOL value) -> Json { return p ? Json(value) : Json(bool(value)); };
    return {{"fill_mode", d.FillMode},
            {"cull_mode", d.CullMode},
            {"front_counter_clockwise", flag(d.FrontCounterClockwise)},
            {"depth_bias", d.DepthBias},
            {"depth_bias_clamp", finite(d.DepthBiasClamp)},
            {"slope_scaled_depth_bias", finite(d.SlopeScaledDepthBias)},
            {"depth_clip_enable", flag(d.DepthClipEnable)},
            {"scissor_enable", flag(d.ScissorEnable)},
            {"multisample_enable", flag(d.MultisampleEnable)},
            {"antialiased_line_enable", flag(d.AntialiasedLineEnable)},
            {"forced_sample_count", d.ForcedSampleCount},
            {"conservative_raster", d.ConservativeRaster}};
}
Json descriptor(ID3D11BlendState *p) {
    D3D11_BLEND_DESC1 d{};
    for (auto &rt : d.RenderTarget) {
        rt.SrcBlend = rt.SrcBlendAlpha = D3D11_BLEND_ONE;
        rt.DestBlend = rt.DestBlendAlpha = D3D11_BLEND_ZERO;
        rt.BlendOp = rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        rt.LogicOp = D3D11_LOGIC_OP_NOOP;
        rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    }
    if (p) {
        Com<ID3D11BlendState1> v1;
        if (SUCCEEDED(p->QueryInterface(IID_PPV_ARGS(&v1))))
            v1->GetDesc1(&d);
        else {
            D3D11_BLEND_DESC base{};
            p->GetDesc(&base);
            d.AlphaToCoverageEnable = base.AlphaToCoverageEnable;
            d.IndependentBlendEnable = base.IndependentBlendEnable;
            for (size_t i = 0; i < 8; ++i) {
                auto &out = d.RenderTarget[i];
                const auto &in = base.RenderTarget[i];
                out.BlendEnable = in.BlendEnable;
                out.SrcBlend = in.SrcBlend;
                out.DestBlend = in.DestBlend;
                out.BlendOp = in.BlendOp;
                out.SrcBlendAlpha = in.SrcBlendAlpha;
                out.DestBlendAlpha = in.DestBlendAlpha;
                out.BlendOpAlpha = in.BlendOpAlpha;
                out.RenderTargetWriteMask = in.RenderTargetWriteMask;
            }
        }
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
    return {{"alpha_to_coverage", bool(d.AlphaToCoverageEnable)},
            {"independent_blend", bool(d.IndependentBlendEnable)},
            {"targets", targets}};
}
Json descriptor(ID3D11DepthStencilState *p) {
    D3D11_DEPTH_STENCIL_DESC d{};
    d.DepthEnable = TRUE;
    d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    d.DepthFunc = D3D11_COMPARISON_LESS;
    d.StencilReadMask = d.StencilWriteMask = 0xff;
    d.FrontFace = d.BackFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP,
                                D3D11_COMPARISON_ALWAYS};
    if (p)
        p->GetDesc(&d);
    auto face = [](const D3D11_DEPTH_STENCILOP_DESC &v) -> Json {
        return {{"fail_op", v.StencilFailOp},
                {"depth_fail_op", v.StencilDepthFailOp},
                {"pass_op", v.StencilPassOp},
                {"func", v.StencilFunc}};
    };
    return {{"depth_enable", bool(d.DepthEnable)},
            {"depth_write_mask", d.DepthWriteMask},
            {"depth_func", d.DepthFunc},
            {"stencil_enable", bool(d.StencilEnable)},
            {"stencil_read_mask", d.StencilReadMask},
            {"stencil_write_mask", d.StencilWriteMask},
            {"front_face", face(d.FrontFace)},
            {"back_face", face(d.BackFace)}};
}

class Snapshot {
    ID3D11DeviceContext *context_;
    Com<ID3D11DeviceContext1> context1_;
    Identities &identities_;
    Json values_ = Json::object(), objects_ = Json::object(), reasons_ = Json::object();
    template <class T> void resource(const std::string &key, T *object) {
        auto [value, metadata] = identities_.describe(object);
        values_[key] = std::move(value);
        if (!object)
            return;
        if constexpr (std::is_same_v<T, ID3D11ShaderResourceView> || std::is_same_v<T, ID3D11SamplerState> ||
                      std::is_same_v<T, ID3D11RenderTargetView> ||
                      std::is_same_v<T, ID3D11DepthStencilView> ||
                      std::is_same_v<T, ID3D11UnorderedAccessView>)
            metadata["descriptor"] = descriptor(object);
        objects_[key] = std::move(metadata);
    }
    template <class T, size_t N>
    void array(const std::string &prefix, UINT count,
               void (STDMETHODCALLTYPE ID3D11DeviceContext::*get)(UINT, UINT, T **)) {
        GetterArray<T, N> values;
        (context_->*get)(0, count, values.data());
        for (UINT i = 0; i < count; ++i)
            resource(prefix + '.' + std::to_string(i), values.values[i]);
    }
    template <class Shader>
    void stage(size_t n, const std::string &name,
               void (STDMETHODCALLTYPE ID3D11DeviceContext::*get)(Shader **, ID3D11ClassInstance **,
                                                                  UINT *)) {
        Com<Shader> shader;
        GetterArray<ID3D11ClassInstance, 256> classes;
        UINT count = 256;
        (context_->*get)(&shader, classes.data(), &count);
        resource(name + ".shader", shader.Get());
        Json classIds = Json::array(), classObjects = Json::array();
        for (UINT i = 0; i < std::min(count, 256u); ++i) {
            auto [id, object] = identities_.describe(classes.values[i]);
            classIds.push_back(id);
            classObjects.push_back(object);
        }
        values_[name + ".classes"] = classIds;
        if (!classObjects.empty())
            objects_[name + ".classes"] = classObjects;
        using CB1 =
            void (STDMETHODCALLTYPE ID3D11DeviceContext1::*)(UINT, UINT, ID3D11Buffer **, UINT *, UINT *);
        static constexpr CB1 cb1[]{
            &ID3D11DeviceContext1::VSGetConstantBuffers1, &ID3D11DeviceContext1::HSGetConstantBuffers1,
            &ID3D11DeviceContext1::DSGetConstantBuffers1, &ID3D11DeviceContext1::GSGetConstantBuffers1,
            &ID3D11DeviceContext1::PSGetConstantBuffers1, &ID3D11DeviceContext1::CSGetConstantBuffers1};
        using CB = void (STDMETHODCALLTYPE ID3D11DeviceContext::*)(UINT, UINT, ID3D11Buffer **);
        static constexpr CB cb[]{
            &ID3D11DeviceContext::VSGetConstantBuffers, &ID3D11DeviceContext::HSGetConstantBuffers,
            &ID3D11DeviceContext::DSGetConstantBuffers, &ID3D11DeviceContext::GSGetConstantBuffers,
            &ID3D11DeviceContext::PSGetConstantBuffers, &ID3D11DeviceContext::CSGetConstantBuffers};
        if (context1_) {
            GetterArray<ID3D11Buffer, 14> buffers;
            std::array<UINT, 14> first{}, length{};
            (context1_.Get()->*cb1[n])(0, 14, buffers.data(), first.data(), length.data());
            for (size_t i = 0; i < 14; ++i) {
                resource(name + ".cb." + std::to_string(i), buffers.values[i]);
                values_[name + ".cb_range." + std::to_string(i)] = {first[i], length[i]};
            }
        } else {
            array<ID3D11Buffer, 14>(name + ".cb", 14, cb[n]);
            for (size_t i = 0; i < 14; ++i)
                reasons_[name + ".cb_range." + std::to_string(i)] = "Context1 getter unavailable";
        }
        using SRV = void (STDMETHODCALLTYPE ID3D11DeviceContext::*)(UINT, UINT, ID3D11ShaderResourceView **);
        static constexpr SRV srv[]{
            &ID3D11DeviceContext::VSGetShaderResources, &ID3D11DeviceContext::HSGetShaderResources,
            &ID3D11DeviceContext::DSGetShaderResources, &ID3D11DeviceContext::GSGetShaderResources,
            &ID3D11DeviceContext::PSGetShaderResources, &ID3D11DeviceContext::CSGetShaderResources};
        using SAM = void (STDMETHODCALLTYPE ID3D11DeviceContext::*)(UINT, UINT, ID3D11SamplerState **);
        static constexpr SAM sam[]{&ID3D11DeviceContext::VSGetSamplers, &ID3D11DeviceContext::HSGetSamplers,
                                   &ID3D11DeviceContext::DSGetSamplers, &ID3D11DeviceContext::GSGetSamplers,
                                   &ID3D11DeviceContext::PSGetSamplers, &ID3D11DeviceContext::CSGetSamplers};
        array<ID3D11ShaderResourceView, 128>(name + ".srv", 128, srv[n]);
        array<ID3D11SamplerState, 16>(name + ".samplers", 16, sam[n]);
    }

  public:
    Snapshot(ID3D11DeviceContext *context, Identities &identities)
        : context_(context), identities_(identities) {
        context->QueryInterface(IID_PPV_ARGS(&context1_));
    }
    Json read() {
        stage(0, "vs", &ID3D11DeviceContext::VSGetShader);
        stage(1, "hs", &ID3D11DeviceContext::HSGetShader);
        stage(2, "ds", &ID3D11DeviceContext::DSGetShader);
        stage(3, "gs", &ID3D11DeviceContext::GSGetShader);
        stage(4, "ps", &ID3D11DeviceContext::PSGetShader);
        stage(5, "cs", &ID3D11DeviceContext::CSGetShader);
        Com<ID3D11InputLayout> layout;
        context_->IAGetInputLayout(&layout);
        resource("input_layout", layout.Get());
        GetterArray<ID3D11Buffer, 32> vb;
        std::array<UINT, 32> strides{}, offsets{};
        context_->IAGetVertexBuffers(0, 32, vb.data(), strides.data(), offsets.data());
        for (size_t i = 0; i < 32; ++i) {
            resource("vb." + std::to_string(i), vb.values[i]);
            values_["strides." + std::to_string(i)] = strides[i];
            values_["offsets." + std::to_string(i)] = offsets[i];
        }
        Com<ID3D11Buffer> ib;
        DXGI_FORMAT format{};
        UINT offset{};
        context_->IAGetIndexBuffer(&ib, &format, &offset);
        resource("ib", ib.Get());
        values_["ib_format"] = format;
        values_["ib_offset"] = offset;
        D3D11_PRIMITIVE_TOPOLOGY topology{};
        context_->IAGetPrimitiveTopology(&topology);
        values_["topology"] = topology;
        Com<ID3D11RasterizerState> rasterizer;
        context_->RSGetState(&rasterizer);
        resource("rasterizer", rasterizer.Get());
        values_["rasterizer.descriptor"] = descriptor(rasterizer.Get());
        std::array<D3D11_VIEWPORT, 16> viewports{};
        UINT count = 16;
        context_->RSGetViewports(&count, viewports.data());
        values_["viewports"] = Json::array();
        for (UINT i = 0; i < std::min(count, 16u); ++i) {
            const auto &v = viewports[i];
            values_["viewports"].push_back({finite(v.TopLeftX), finite(v.TopLeftY), finite(v.Width),
                                            finite(v.Height), finite(v.MinDepth), finite(v.MaxDepth)});
        }
        std::array<D3D11_RECT, 16> scissors{};
        count = 16;
        context_->RSGetScissorRects(&count, scissors.data());
        values_["scissors"] = Json::array();
        for (UINT i = 0; i < std::min(count, 16u); ++i) {
            const auto &v = scissors[i];
            values_["scissors"].push_back({v.left, v.top, v.right, v.bottom});
        }
        Com<ID3D11BlendState> blend;
        std::array<float, 4> factors{};
        UINT mask{};
        context_->OMGetBlendState(&blend, factors.data(), &mask);
        resource("blend", blend.Get());
        values_["blend.descriptor"] = descriptor(blend.Get());
        values_["sample_mask"] = mask;
        for (size_t i = 0; i < 4; ++i)
            values_["blend_factor." + std::to_string(i)] = finite(factors[i]);
        Com<ID3D11DepthStencilState> depthState;
        UINT stencil{};
        context_->OMGetDepthStencilState(&depthState, &stencil);
        resource("depth_state", depthState.Get());
        values_["depth_state.descriptor"] = descriptor(depthState.Get());
        values_["stencil_ref"] = stencil;
        Com<ID3D11Predicate> predicate;
        BOOL predicateValue{};
        context_->GetPredication(&predicate, &predicateValue);
        resource("predicate", predicate.Get());
        values_["predicate_value"] = predicateValue;
        Com<ID3D11Device> device;
        context_->GetDevice(&device);
        UINT limit = device->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_1 ? 64 : 8;
        GetterArray<ID3D11RenderTargetView, 8> rtv;
        GetterArray<ID3D11UnorderedAccessView, 64> uav;
        Com<ID3D11DepthStencilView> dsv;
        context_->OMGetRenderTargetsAndUnorderedAccessViews(8, rtv.data(), &dsv, 0, limit, uav.data());
        resource("dsv", dsv.Get());
        for (size_t i = 0; i < 8; ++i)
            resource("rtv." + std::to_string(i), rtv.values[i]);
        for (size_t i = 0; i < limit; ++i)
            resource("om.uav." + std::to_string(i), uav.values[i]);
        array<ID3D11UnorderedAccessView, 64>("cs.uav", limit,
                                             &ID3D11DeviceContext::CSGetUnorderedAccessViews);
        for (UINT i = limit; i < 64; ++i)
            for (auto prefix : {"om.uav.", "cs.uav."})
                reasons_[std::string(prefix) + std::to_string(i)] = "Slot unsupported by this device";
        GetterArray<ID3D11Buffer, 4> so;
        context_->SOGetTargets(4, so.data());
        for (size_t i = 0; i < 4; ++i) {
            resource("so.targets." + std::to_string(i), so.values[i]);
            reasons_["so.offsets." + std::to_string(i)] = "DX11 has no getter for the live SO write cursor";
        }
        auto keys = commandStateFieldNames();
        keys.insert(keys.end(), {"rasterizer.descriptor", "blend.descriptor", "depth_state.descriptor"});
        Json fields = Json::array();
        for (const auto &key : keys) {
            bool known = values_.contains(key);
            Json field{{"field", key},
                       {"value", known ? values_[key] : Json(nullptr)},
                       {"known", known},
                       {"source", {{"kind", known ? "native_getter" : "unavailable"}, {"event", nullptr}}},
                       {"resource_id", nullptr}};
            if (objects_.contains(key)) {
                const auto &object = objects_[key];
                field["object"] = object;
                if (object.is_object() && object["captured_ids"].size() == 1)
                    field["resource_id"] = object["captured_ids"][0];
            }
            if (reasons_.contains(key))
                field["source"]["reason"] = reasons_[key];
            fields.push_back(std::move(field));
        }
        return fields;
    }
};
} // namespace

Json inspectReplayPipeline(const Frame &frame, Replay &replay, bool experimentApplied,
                           const std::function<void(Id, size_t, size_t)> &progress) {
    const auto &options = replay.options();
    const auto event = options.until;
    const bool after = !options.before;
    if (!event || frame.entry(event).category != 7)
        throw std::runtime_error("Pipeline boundary requires an API command");
    auto command = inspectCommand(frame, event);
    auto context = stateCommandContext(frame, command);
    if (!context)
        throw std::runtime_error("Pipeline boundary requires an identified immediate context");
    Identities identities;
    Json fields;
    replay.run(progress, [&](Id id, bool isAfter, ID3D11DeviceContext *ctx, const auto &objects) {
        identities.observe(objects);
        if (id == event && isAfter == after)
            fields = Snapshot(ctx, identities).read();
    });
    if (fields.is_null()) {
        if (command.value("draw", false))
            throw std::runtime_error("Draw pipeline boundary was not observed");
        replay.inspectNativeState([&](ID3D11DeviceContext *ctx, const auto &objects) {
            identities.observe(objects);
            fields = Snapshot(ctx, identities).read();
        });
    }
    size_t known = 0;
    for (auto &field : fields) {
        field["source"]["event"] = event;
        if (field["known"] == true)
            ++known;
    }
    return {{"event", event},
            {"api", command.at("name")},
            {"context", *context},
            {"value_time", after ? "after_command" : "before_command"},
            {"source", "native_replay"},
            {"experiment_applied", experimentApplied},
            {"fields", fields},
            {"command_enabled", !options.disabled.contains(event)},
            {"known_fields", known},
            {"unknown_fields", fields.size() - known},
            {"counts", replay.counts},
            {"notes", Json::array()},
            {"limits",
             {"Actual independent replay bindings, including snapshot normalization and unused SRV "
              "filtering; not original captured context state.",
              "Draw boundaries are inside scoped resource experiments after pipeline edits; disabled "
              "commands still have prepared bindings.",
              "Runtime object tokens distinguish clones and generated states; a captured ID denotes "
              "provenance, not unchanged object contents.",
              "SO write cursors have no native getter; hidden UAV counters and query results use resource "
              "snapshots.",
              "Missing bindings must be resolved by replay before this boundary; deferred contexts remain "
              "unsupported."}}};
}
} // namespace flora
