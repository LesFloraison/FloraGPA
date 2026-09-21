#include "Coverage.h"
#include "DxbcCoverage.h"
#include "DxbcInspection.h"
#include "EventDescription.h"
#include "FrameOutput.h"
#include "ShaderInspector.h"
#include "core/ClassLinkage.h"
#include "core/StreamOutput.h"
#include "replay/BlendState.h"
#include "replay/InspectionCopyBindings.h"
#include "replay/Unpredicated.h"
#include <QDir>
#include <QImage>
#include <QSaveFile>
#include <algorithm>
#include <cmath>
#include <d3dcompiler.h>

namespace flora {
namespace {
using Json = nlohmann::json;
template <class T> Bytes bytes(const T &value) {
    return {reinterpret_cast<const uint8_t *>(&value), sizeof value};
}
std::vector<uint8_t> compileMarker(bool geometry) {
    const std::string source =
        geometry ? "float4 main():SV_Target{return float4(1,1,1,1);}" : "void main(){}";
    Com<ID3DBlob> code, error;
    check(D3DCompile(source.data(), source.size(), "FloraGPA coverage", nullptr, nullptr, "main", "ps_5_0",
                     D3DCOMPILE_ENABLE_STRICTNESS, 0, &code, &error),
          "Compile coverage marker");
    const auto data = static_cast<const uint8_t *>(code->GetBufferPointer());
    return {data, data + code->GetBufferSize()};
}
bool dualSource(const D3D11_BLEND_DESC1 &blend) {
    for (const auto &rt : blend.RenderTarget)
        if (rt.BlendEnable)
            for (auto value : {rt.SrcBlend, rt.DestBlend, rt.SrcBlendAlpha, rt.DestBlendAlpha})
                if (value >= D3D11_BLEND_SRC1_COLOR && value <= D3D11_BLEND_INV_SRC1_ALPHA)
                    return true;
    return false;
}
struct ExtraUavs {
    ID3D11DeviceContext *context;
    std::array<Com<ID3D11UnorderedAccessView>, 56> held;
    bool active = false;
    ExtraUavs(ID3D11DeviceContext *c, const std::map<uint32_t, uint32_t> &mapping) : context(c) {
        for (auto [from, to] : mapping)
            active |= to >= 8;
        if (active) {
            std::array<ID3D11UnorderedAccessView *, 56> pointers{};
            context->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 8, 56, pointers.data());
            for (size_t i = 0; i < 56; ++i)
                held[i].Attach(pointers[i]);
        }
    }
    ~ExtraUavs() {
        if (!active)
            return;
        std::array<ID3D11UnorderedAccessView *, 64> pointers{};
        std::array<Com<ID3D11UnorderedAccessView>, 8> lower;
        context->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, 8, pointers.data());
        for (size_t i = 0; i < 8; ++i)
            lower[i].Attach(pointers[i]);
        for (size_t i = 0; i < 56; ++i)
            pointers[i + 8] = held[i].Get();
        context->OMSetRenderTargetsAndUnorderedAccessViews(D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL,
                                                           nullptr, nullptr, 0, 64, pointers.data(), nullptr);
    }
};
} // namespace
class CoverageCapture {
    Replay &r;
    Id eventId;
    CoverageOptions options;
    bool geometry, viewport = false, buffer = false, depth = false, isolated = false, active = false,
                   read = false;
    uint32_t slot = 0, count = 0, samples = 1, width = 0, height = 0, forced = 0, conservative = 0;
    std::optional<uint32_t> colorSlot;
    State state{};
    Event event{};
    Id targetId = 0, targetView = 0;
    Resource sourceResource{}, markerResource{};
    Json selected, metadata, viewports = Json::array(), arraySource;
    Com<ID3D11Resource> source, marker;
    Com<ID3D11RenderTargetView> markerView;
    Com<ID3D11BlendState> diagnosticBlend;
    std::array<float, 4> factors{};
    UINT sampleMask = UINT32_MAX;
    std::map<uint32_t, uint32_t> mapping;
    std::map<uint32_t, Id> moved;
    std::map<unsigned, Com<IUnknown>> patchedShaders;
    std::map<unsigned, std::vector<ID3D11ClassInstance *>> instances;
    CoverageResult result;
    Bytes code(Id id) {
        auto replacement = r.options_.shaders.find(id);
        return replacement == r.options_.shaders.end() ? r.frame_.shader(r.frame_.resource(id).data)
                                                       : Bytes(replacement->second);
    }
    std::vector<Id> outputIds() const {
        std::vector<Id> ids(state.rtv.begin(), state.rtv.end());
        ids.insert(ids.end(), state.omExtended.begin(), state.omExtended.end());
        return ids;
    }
    void bind() {
        r.bind(state, false);
        r.applyGraphicsEdits(event, state);
    }
    void submitDiagnostic() {
        const auto &a = event.args;
        auto c = r.context_.Get();
        switch (event.type) {
        case 0x37:
            c->Draw(a[0], a[1]);
            break;
        case 0x38:
            c->Draw(r.drawAutoParameters(event.id).vertexCount, 0);
            break;
        case 0x39:
            c->DrawIndexed(a[0], a[1], int32_t(a[2]));
            break;
        case 0x3a:
            c->DrawIndexedInstanced(a[0], a[1], a[2], int32_t(a[3]), a[4]);
            break;
        case 0x3b:
            c->DrawIndexedInstancedIndirect(r.get<ID3D11Buffer>(event.argumentBuffer), a[0]);
            break;
        case 0x3c:
            c->DrawInstanced(a[0], a[1], a[2], a[3]);
            break;
        case 0x3d:
            c->DrawInstancedIndirect(r.get<ID3D11Buffer>(event.argumentBuffer), a[0]);
            break;
        default:
            throw std::runtime_error("Coverage requires a Draw");
        }
    }
    void createShader(unsigned stage, Bytes raw, bool replacementGeometry = false) {
        const auto id = state.stages[stage].shader;
        auto linkage =
            replacementGeometry ? nullptr : r.get<ID3D11ClassLinkage>(shaderClassLinkage(r.frame_, id));
        Com<IUnknown> object;
#define MAKE_SHADER(N, Type, Method)                                                                         \
    case N: {                                                                                                \
        Com<Type> v;                                                                                         \
        check(r.device_->Method(raw.data(), raw.size(), linkage, &v), "Create coverage shader");             \
        object = v;                                                                                          \
        break;                                                                                               \
    }
        switch (stage) {
            MAKE_SHADER(0, ID3D11VertexShader, CreateVertexShader)
            MAKE_SHADER(1, ID3D11HullShader, CreateHullShader)
            MAKE_SHADER(2, ID3D11DomainShader, CreateDomainShader)
            MAKE_SHADER(3, ID3D11GeometryShader, CreateGeometryShader)
            MAKE_SHADER(4, ID3D11PixelShader, CreatePixelShader)
        default:
            throw std::runtime_error("Invalid graphics diagnostic stage");
        }
#undef MAKE_SHADER
        patchedShaders[stage] = object;
        auto &classes = instances[stage];
        classes.clear();
        if (!replacementGeometry) {
            const auto n = id ? r.interfaceSlots_.at(id) : 0;
            if (n > state.stages[stage].classCount)
                throw std::runtime_error("Missing diagnostic class instances");
            for (unsigned i = 0; i < n; ++i)
                classes.push_back(r.get<ID3D11ClassInstance>(state.stages[stage].classes[i]));
        }
    }
    void bindShaders() {
        for (const auto &[stage, shader] : patchedShaders) {
            const auto &cls = instances.at(stage);
            const auto p = cls.empty() ? nullptr : cls.data();
            const auto n = UINT(cls.size());
#define SET_SHADER(N, Type, Method)                                                                          \
    case N:                                                                                                  \
        r.context_->Method(static_cast<Type *>(shader.Get()), p, n);                                         \
        break
            switch (stage) {
                SET_SHADER(0, ID3D11VertexShader, VSSetShader);
                SET_SHADER(1, ID3D11HullShader, HSSetShader);
                SET_SHADER(2, ID3D11DomainShader, DSSetShader);
                SET_SHADER(3, ID3D11GeometryShader, GSSetShader);
                SET_SHADER(4, ID3D11PixelShader, PSSetShader);
            }
#undef SET_SHADER
        }
    }
    std::vector<uint8_t> relocate(Bytes raw) {
        std::set<uint32_t> occupied;
        const auto ids = outputIds();
        for (uint32_t n = state.omStart; n < state.rtCount; ++n)
            if (ids.at(n))
                occupied.insert(n);
        std::map<unsigned, Bytes> writers;
        for (unsigned stage = 0; stage < 4; ++stage) {
            auto id = state.stages[stage].shader;
            if (!id || r.passthroughShaders_.contains(id))
                continue;
            const auto rawStage = code(id);
            const auto info = inspectShader(rawStage);
            const bool writes =
                std::any_of(info["bindings"].begin(), info["bindings"].end(), [](const auto &b) {
                    const auto t = b.at("type").template get<unsigned>();
                    return t == 4 || t == 6 || (t >= 8 && t <= 11);
                });
            if (!writes)
                continue;
            writers[stage] = rawStage;
            const auto declared = relocateShaderUavs(rawStage, {}, r.uavLimit_).declared;
            occupied.insert(declared.begin(), declared.end());
        }
        auto reserved = reserveCoverageTarget(raw, occupied, r.uavLimit_);
        mapping = reserved.mapping;
        for (auto [stage, rawStage] : writers)
            if (!mapping.empty())
                createShader(stage, relocateShaderUavs(rawStage, mapping, r.uavLimit_).bytes);
        for (uint32_t n = state.omStart; n < state.rtCount; ++n)
            if (ids.at(n))
                moved[mapping.contains(n) ? mapping.at(n) : n] = ids[n];
        return reserved.bytes;
    }
    void depthState(bool forceDisable = false) {
        if (!geometry && options.depthTest && !forceDisable)
            return;
        Com<ID3D11DepthStencilState> current;
        UINT reference = 0;
        r.context_->OMGetDepthStencilState(&current, &reference);
        D3D11_DEPTH_STENCIL_DESC d{};
        d.DepthEnable = TRUE;
        d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        d.DepthFunc = D3D11_COMPARISON_LESS;
        d.StencilReadMask = d.StencilWriteMask = 255;
        d.FrontFace = d.BackFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP,
                                    D3D11_COMPARISON_ALWAYS};
        if (current)
            current->GetDesc(&d);
        if (geometry) {
            d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
            d.FrontFace.StencilFailOp = d.FrontFace.StencilDepthFailOp = d.FrontFace.StencilPassOp =
                D3D11_STENCIL_OP_KEEP;
            d.BackFace.StencilFailOp = d.BackFace.StencilDepthFailOp = d.BackFace.StencilPassOp =
                D3D11_STENCIL_OP_KEEP;
        }
        if (!options.depthTest || forceDisable) {
            d.DepthEnable = FALSE;
            d.StencilEnable = FALSE;
        }
        Com<ID3D11DepthStencilState> native;
        check(r.device_->CreateDepthStencilState(&d, &native), "Create coverage depth state");
        r.context_->OMSetDepthStencilState(native.Get(), reference);
    }
    void markerTexture() {
        const uint32_t dimension = viewport ? 4 : selected.at("dimension").get<uint32_t>();
        const uint32_t layers = viewport ? 1 : selected.at("layer_count").get<uint32_t>();
        uint32_t quality = 0;
        if (!viewport && sourceResource.type == 0x85)
            quality = sourceResource.desc[6];
        markerResource.id = 0;
        markerResource.data = 0;
        D3D11_RENDER_TARGET_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        view.ViewDimension = D3D11_RTV_DIMENSION(dimension);
        if (dimension == 1) {
            markerResource.type = 0x83;
            markerResource.desc = {width * 4, 0, 32, 0, 0, 0};
            D3D11_BUFFER_DESC d{};
            std::memcpy(&d, markerResource.desc.data(), sizeof d);
            Com<ID3D11Buffer> v;
            check(r.device_->CreateBuffer(&d, nullptr, &v), "Create coverage buffer");
            marker = v;
            view.Buffer.NumElements = width;
        } else if (dimension == 2 || dimension == 3) {
            markerResource.type = 0x84;
            markerResource.desc = {width, 1, layers, 28, 0, 32, 0, 0};
            marker = r.createEditTexture(markerResource);
            if (dimension == 3)
                view.Texture1DArray.ArraySize = layers;
        } else if (dimension == 8) {
            markerResource.type = 0x86;
            markerResource.desc = {width, height, layers, 1, 28, 0, 32, 0, 0};
            marker = r.createEditTexture(markerResource);
            view.Texture3D.WSize = layers;
        } else {
            markerResource.type = 0x85;
            markerResource.desc = {width, height, 1, layers, 28, samples, quality, 0, 32, 0, 0};
            marker = r.createEditTexture(markerResource);
            if (dimension == 5)
                view.Texture2DArray.ArraySize = layers;
            if (dimension == 7)
                view.Texture2DMSArray.ArraySize = layers;
        }
        check(r.device_->CreateRenderTargetView(marker.Get(), &view, &markerView), "Create coverage RTV");
        const float clear[4]{};
        Unpredicated guard(r.context_.Get());
        r.context_->ClearRenderTargetView(markerView.Get(), clear);
    }
    void viewportTarget() {
        if (r.options_.warp && !forced && !(sampleMask & 1))
            throw std::runtime_error("WARP viewport coverage with sample 0 masked out and no forced samples "
                                     "cannot preserve native targetless draw behavior");
        UINT n = 16;
        std::array<D3D11_VIEWPORT, 16> values{};
        r.context_->RSGetViewports(&n, values.data());
        double w = 1, h = 1;
        for (UINT i = 0; i < n; ++i) {
            const auto &v = values[i];
            const std::array<float, 6> row{v.TopLeftX, v.TopLeftY, v.Width, v.Height, v.MinDepth, v.MaxDepth};
            for (auto x : row)
                if (!std::isfinite(x))
                    throw std::runtime_error("Viewport coverage requires finite bounds");
            if (v.Width < 0 || v.Height < 0)
                throw std::runtime_error("Viewport coverage requires nonnegative dimensions");
            w = std::max(w, std::ceil(double(v.TopLeftX) + v.Width));
            h = std::max(h, std::ceil(double(v.TopLeftY) + v.Height));
            viewports.push_back(row);
        }
        if (w > 16384 || h > 16384)
            throw std::runtime_error("Viewport coverage extent exceeds the DX11 texture limit");
        width = uint32_t(w);
        height = uint32_t(h);
        samples = 1;
        unsigned stage = state.stages[3].shader ? 3 : state.stages[2].shader ? 2 : 0;
        const auto id = state.stages[stage].shader;
        unsigned stream = 0;
        if (stage == 3 && id)
            if (auto so = shaderStreamOutput(r.frame_, id))
                stream = readStreamOutputDeclaration(r.frame_, so).rasterizedStream;
        if (id)
            for (const auto &[tag, payload] : readDxbcParts(code(id)))
                if (tag == 0x4e47534f || tag == 0x3547534f)
                    for (const auto &s : dxbc_detail::signature(payload, tag == 0x3547534f))
                        if (s.at("system_value") == 4 && s.value("stream", 0u) == stream) {
                            arraySource = s;
                            break;
                        }
        metadata["array_index_source"] = arraySource.is_null()
                                             ? Json(nullptr)
                                             : Json{{"stage", stage == 3   ? "gs"
                                                              : stage == 2 ? "ds"
                                                                           : "vs"},
                                                    {"stream", stream},
                                                    {"register", arraySource.at("register")},
                                                    {"mask", arraySource.at("mask")}};
    }
    void selectTarget() {
        count = std::min({state.rtCount, state.omStart, 8u});
        if (options.target == "auto") {
            for (uint32_t i = 0; i < count; ++i)
                if (state.rtv[i]) {
                    colorSlot = i;
                    break;
                }
        } else if (options.target != "depth") {
            colorSlot = uint32_t(options.target[2] - '0');
            if (*colorSlot >= count || !state.rtv[*colorSlot])
                throw std::runtime_error("Coverage target must name a bound RTV slot or depth/stencil view");
        }
        if (options.target == "depth" && !state.dsv)
            throw std::runtime_error("Coverage target must name a bound RTV slot or depth/stencil view");
        depth = !colorSlot;
        targetView = depth ? state.dsv : state.rtv[*colorSlot];
        viewport = !targetView;
        if (viewport) {
            viewportTarget();
            return;
        }
        Reader view(r.frame_.payload(targetView));
        view.skip(16);
        targetId = view.read<Id>();
        sourceResource = r.frame_.resource(targetId);
        auto native = r.get<ID3D11View>(targetView);
        native->GetResource(&source);
        if (depth) {
            D3D11_DEPTH_STENCIL_VIEW_DESC d{};
            r.get<ID3D11DepthStencilView>(targetView)->GetDesc(&d);
            selected = outputSubresource(sourceResource, bytes(d), options.layer, true);
        } else {
            D3D11_RENDER_TARGET_VIEW_DESC d{};
            r.get<ID3D11RenderTargetView>(targetView)->GetDesc(&d);
            selected = outputSubresource(sourceResource, bytes(d), options.layer, false);
        }
        width = selected.at("width");
        height = selected.at("height");
        buffer = selected.at("dimension") == 1;
        samples = buffer ? 1 : textureInfo(sourceResource).samples;
        if (buffer) {
            std::array<ID3D11RenderTargetView *, 8> views{};
            r.context_->OMGetRenderTargets(8, views.data(), nullptr);
            const bool bound = views.at(*colorSlot) == r.get<ID3D11RenderTargetView>(targetView);
            for (auto v : views)
                if (v)
                    v->Release();
            if (!bound)
                throw std::runtime_error(
                    "Captured buffer RTV is not bound on the native context; incompatible output bindings");
        }
    }
    void bindMarker() {
        std::vector<ID3D11RenderTargetView *> targets;
        if (buffer || viewport)
            targets = {markerView.Get()};
        else {
            for (uint32_t i = 0; i < count; ++i)
                targets.push_back(r.get<ID3D11RenderTargetView>(state.rtv[i]));
            targets.resize(std::max(count, slot + 1), nullptr);
            targets[slot] = markerView.Get();
        }
        auto dsv = options.depthTest && !viewport ? r.get<ID3D11DepthStencilView>(state.dsv) : nullptr;
        uint32_t start = state.omStart;
        std::vector<ID3D11UnorderedAccessView *> uavs;
        if (viewport || (isolated && state.omStart == 0)) {
            start = 1;
            if (!moved.empty())
                for (uint32_t i = 1; i <= moved.rbegin()->first; ++i)
                    uavs.push_back(r.get<ID3D11UnorderedAccessView>(moved.contains(i) ? moved.at(i) : 0));
        } else {
            const auto ids = outputIds();
            for (uint32_t i = start; i < state.rtCount; ++i)
                uavs.push_back(r.get<ID3D11UnorderedAccessView>(ids.at(i)));
        }
        if (!uavs.empty())
            r.context_->OMSetRenderTargetsAndUnorderedAccessViews(
                UINT(targets.size()), targets.data(), dsv, start, UINT(uavs.size()), uavs.data(), nullptr);
        else
            r.context_->OMSetRenderTargets(UINT(targets.size()), targets.data(), dsv);
        depthState(viewport);
        r.context_->OMSetBlendState(diagnosticBlend.Get(), factors.data(), sampleMask);
        bindShaders();
    }
    void before() {
        state = r.prepareState(event);
        r.validateRasterizer(state);
        active = !r.options_.disabled.contains(eventId) && !r.options_.suppressDraws;
        Com<ID3D11RasterizerState> rs;
        r.context_->RSGetState(&rs);
        Com<ID3D11RasterizerState2> rs2;
        Com<ID3D11RasterizerState1> rs1;
        if (rs && SUCCEEDED(rs.As(&rs2))) {
            D3D11_RASTERIZER_DESC2 d{};
            rs2->GetDesc2(&d);
            forced = d.ForcedSampleCount;
            conservative = d.ConservativeRaster;
        } else if (rs && SUCCEEDED(rs.As(&rs1))) {
            D3D11_RASTERIZER_DESC1 d{};
            rs1->GetDesc1(&d);
            forced = d.ForcedSampleCount;
        }
        Com<ID3D11BlendState> current;
        r.context_->OMGetBlendState(&current, factors.data(), &sampleMask);
        auto blend = defaultBlend();
        if (current) {
            Com<ID3D11BlendState1> extended;
            if (SUCCEEDED(current.As(&extended)))
                extended->GetDesc1(&blend);
            else {
                D3D11_BLEND_DESC basic{};
                current->GetDesc(&basic);
                blend = decodeBlend(bytes(basic), false);
            }
        }
        selectTarget();
        markerTexture();
        const auto shader = state.stages[4].shader;
        if (!shader && !depth && !geometry)
            throw std::runtime_error("Null PS coverage currently requires a depth-only draw");
        auto raw = (geometry || !shader) ? compileMarker(geometry)
                                         : std::vector<uint8_t>(code(shader).begin(), code(shader).end());
        const auto limit = std::min(state.omStart, 8u);
        std::set<uint32_t> used;
        for (const auto &[tag, payload] : readDxbcParts(raw))
            if (tag == 0x4e47534f)
                for (const auto &s : dxbc_detail::signature(payload))
                    if (s.at("register").get<uint32_t>() < 8)
                        used.insert(s.at("register").get<uint32_t>());
        slot = limit;
        for (uint32_t n = 0; n < limit; ++n)
            if (!used.contains(n) && (n >= count || !state.rtv[n])) {
                slot = n;
                break;
            }
        const bool dual = dualSource(blend);
        isolated = viewport || geometry || buffer || slot == limit || dual || usesLogic(blend);
        if (viewport || (isolated && !limit)) {
            raw = relocate(raw);
            slot = 0;
        } else if (isolated)
            slot = buffer || dual ? 0 : limit - 1;
        CoverageMarkerOptions patch;
        patch.slot = slot;
        patch.keepAlpha = !geometry && slot == 0 && blend.AlphaToCoverageEnable && !viewport;
        if (viewport) {
            patch.arrayIndex = options.layer;
            patch.arrayRouted = !arraySource.is_null();
            patch.arraySource = arraySource;
        }
        auto patched = isolated ? replaceCoverageMarker(raw, patch) : addCoverageMarker(raw, slot);
        createShader(4, patched, geometry);
        auto diagnostic = blend;
        if (isolated) {
            diagnostic = defaultBlend();
            diagnostic.IndependentBlendEnable = TRUE;
            diagnostic.AlphaToCoverageEnable = viewport || geometry ? FALSE : blend.AlphaToCoverageEnable;
            if (viewport && options.layer) {
                diagnostic.IndependentBlendEnable = FALSE;
                for (auto &rt : diagnostic.RenderTarget) {
                    rt.BlendEnable = TRUE;
                    rt.SrcBlend = rt.DestBlend = rt.SrcBlendAlpha = rt.DestBlendAlpha = D3D11_BLEND_ONE;
                    rt.BlendOp = rt.BlendOpAlpha = D3D11_BLEND_OP_MAX;
                }
            }
        } else {
            if (!diagnostic.IndependentBlendEnable)
                for (unsigned i = 1; i < 8; ++i)
                    diagnostic.RenderTarget[i] = diagnostic.RenderTarget[0];
            diagnostic.IndependentBlendEnable = TRUE;
            diagnostic.RenderTarget[slot] = defaultBlend().RenderTarget[0];
        }
        diagnosticBlend = r.createBlend(diagnostic);
        if (isolated) {
            ExtraUavs restoreHigh(r.context_.Get(), mapping);
            r.withPrivateOutputs(
                event, state,
                [&] {
                    bind();
                    r.unbindStreamOutput();
                    bindMarker();
                    if (active)
                        submitDiagnostic();
                },
                false);
            if (!geometry && !options.depthTest)
                depthState();
        } else
            bindMarker();
    }
    std::vector<uint8_t> selectedBytes(ID3D11Resource *object, const Resource &resource, uint32_t mip,
                                       uint32_t layer, uint32_t format, const Json &bufferSelection = {}) {
        if (resource.type == 0x83) {
            const auto offset = bufferSelection.at("byte_offset").get<uint32_t>(),
                       size = bufferSelection.at("byte_length").get<uint32_t>();
            D3D11_BUFFER_DESC desc{size, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0};
            Com<ID3D11Buffer> staging;
            check(r.device_->CreateBuffer(&desc, nullptr, &staging), "Create coverage buffer staging");
            const D3D11_BOX box{offset, 0, 0, offset + size, 1, 1};
            {
                Unpredicated guard(r.context_.Get());
                InspectionCopyBindings bindings(r.context_.Get(), r.options_.warp);
                r.context_->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, object, 0, &box);
            }
            D3D11_MAPPED_SUBRESOURCE mapped{};
            check(r.context_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Read coverage buffer");
            std::vector<uint8_t> storage(size);
            std::memcpy(storage.data(), mapped.pData, size);
            r.context_->Unmap(staging.Get(), 0);
            return storage;
        }
        auto info = textureInfo(resource);
        std::vector<uint8_t> storage;
        if (info.samples > 1) {
            auto resolved = r.readMsaaStorage(object, resource, {}, format);
            info = textureInfo(resolved.resource);
            storage = std::move(resolved.bytes);
        } else
            storage = r.readTextureStorage(object, resource);
        uint64_t offset = 0;
        for (uint32_t l = 0; l < info.layers; ++l)
            for (uint32_t m = 0; m < info.mips; ++m) {
                const auto [pitch, rows] =
                    pitches(std::max(1u, info.width >> m), std::max(1u, info.height >> m), info.format);
                const auto size = uint64_t(pitch) * rows;
                const auto slices = std::max(1u, info.depth >> m);
                if (offset > storage.size() || size * slices > storage.size() - offset)
                    throw std::runtime_error("Coverage storage bounds");
                if (m == mip && (info.dimension == 4 || l == layer)) {
                    const auto slice = info.dimension == 4 ? layer : 0u;
                    if (slice >= slices)
                        throw std::runtime_error("Coverage slice bounds");
                    const auto begin = offset + slice * size;
                    return {storage.begin() + ptrdiff_t(begin), storage.begin() + ptrdiff_t(begin + size)};
                }
                offset += size * slices;
            }
        throw std::runtime_error("Coverage subresource is missing");
    }
    void after() {
        Replay::PredicateIsolation queries(r);
        const auto markerSelection = Json{{"byte_offset", 0}, {"byte_length", width * 4}};
        auto mask =
            selectedBytes(marker.Get(), markerResource, 0,
                          viewport ? 0 : selected.at("relative_layer").get<uint32_t>(), 28, markerSelection);
        std::vector<uint8_t> pixels;
        if (viewport) {
            pixels.resize(size_t(width) * height * 4);
            for (size_t n = 0; n < pixels.size(); n += 4) {
                pixels[n] = pixels[n + 1] = pixels[n + 2] = 24;
                pixels[n + 3] = 255;
            }
        } else {
            const auto format = selected.at("format").get<uint32_t>();
            auto data = selectedBytes(source.Get(), sourceResource, selected.at("mip"), selected.at("layer"),
                                      format, selected);
            const auto sourceFormat = buffer ? format : textureInfo(sourceResource).format;
            if (!depth && (sourceFormat == 27 || sourceFormat == 28 || sourceFormat == 29))
                pixels = std::move(data);
            else {
                // The reference renders its storage preview on a separate hardware
                // device, even for WARP replay. Preserve GPU format conversion and
                // UNORM rounding instead of approximating it with CPU arithmetic.
                Resource preview;
                preview.type = 0x85;
                preview.desc = {width, height, 1, 1, sourceFormat, 1, 0, 0, 8, 0, 0};
                Replay converter(r.frame_);
                pixels =
                    converter
                        .previewTextureStorage(preview, data, 0, 0, 0, 0, 1, depth ? "r" : "rgba", format)
                        .rgba;
            }
        }
        if (mask.size() != uint64_t(width) * height * 4 || pixels.size() != mask.size())
            throw std::runtime_error("Coverage image dimensions mismatch");
        result.mask = {width, height, 28, std::move(mask), targetId};
        result.after = {width, height, 28, std::move(pixels), targetId};
        read = true;
    }
    void report() {
        const auto hasSo =
            std::any_of(state.so.begin(), state.so.begin() + state.soCount, [](auto id) { return id != 0; });
        const auto shader = state.stages[4].shader;
        Json relocation = Json::object();
        for (auto [from, to] : mapping)
            relocation[std::to_string(from)] = to;
        if (viewport) {
            metadata.update({{"target_resource", nullptr},
                             {"target_kind", "viewport"},
                             {"target_subresource", nullptr},
                             {"coordinate_space", "rasterizer_viewport"},
                             {"coordinate_origin", {0, 0}},
                             {"viewports", viewports},
                             {"width", width},
                             {"height", height},
                             {"marker_target_slot", 0},
                             {"samples", 1},
                             {"forced_sample_count", forced},
                             {"conservative_raster", conservative},
                             {"render_target_array_routing", !arraySource.is_null()},
                             {"array_index_selection", options.layer ? Json(*options.layer) : Json(nullptr)},
                             {"array_index_scope", options.layer ? "selected_index" : "all_indices"},
                             {"after_draw_display", "neutral_background_no_color_output"},
                             {"instrumentation", "isolated_viewport_uav_relocation"},
                             {"pixel_shader_present", bool(shader)},
                             {"uav_relocation", relocation},
                             {"submissions", 2 * unsigned(active)},
                             {"original_submissions", unsigned(active)},
                             {"diagnostic_submissions", unsigned(active)},
                             {"geometry_only", geometry},
                             {"original_so_suspended_for_diagnostics", hasSo},
                             {"limitations",
                              {"The draw has no RTV/DSV; coverage uses viewport pixel coordinates and a "
                               "neutral background, not a previous render target.",
                               "Coverage marks rasterized fragments surviving the PS, not the buffer "
                               "elements written by the shader.",
                               "A separate draw on private output copies measures coverage; nondeterministic "
                               "UAV ordering may differ from the original.",
                               "Negative pixel coordinates are clipped by native rasterization; viewport and "
                               "shader coordinates are not translated.",
                               "Array index selection filters the original shader index, not a physical "
                               "texture layer; an empty selection shows their pixel union.",
                               "Eight occupied UAV slots require feature level 11.1."}}});
        } else {
            const auto sourceFormat =
                buffer ? selected.at("format").get<uint32_t>() : textureInfo(sourceResource).format;
            metadata = {
                {"target_resource", targetId},
                {"target_kind", buffer  ? "buffer"
                                : depth ? "depth"
                                        : "color"},
                {"target_view", targetView},
                {"target_slot", colorSlot ? Json(*colorSlot) : Json(nullptr)},
                {"target_subresource", buffer ? Json(nullptr) : selected},
                {"target_buffer_view", buffer ? selected : Json(nullptr)},
                {"coordinate_space", buffer ? "buffer_view_elements" : "texture_subresource"},
                {"marker_target_slot", slot},
                {"samples", samples},
                {"forced_sample_count", forced},
                {"conservative_raster", conservative},
                {"after_draw_display", depth ? "depth_grayscale_0_1"
                                       : (sourceFormat == 27 || sourceFormat == 28 || sourceFormat == 29)
                                           ? "original_rgba8"
                                           : "rgba8_preview_clamped_0_1"},
                {"instrumentation", isolated ? "isolated_output_replacement"
                                    : shader ? "original_dxbc_plus_constant_output"
                                             : "null_ps_constant_marker"},
                {"uav_relocation", relocation},
                {"submissions", unsigned(active) * (1 + unsigned(isolated))},
                {"original_submissions", unsigned(active)},
                {"diagnostic_submissions", unsigned(active && isolated)},
                {"limitations",
                 {"Coverage is the union of samples surviving this draw; later passes may hide it.",
                  "Coverage marks test acceptance even when target write settings leave stored values "
                  "unchanged."}},
                {"geometry_only", geometry},
                {"original_so_suspended_for_diagnostics", isolated && hasSo},
                {"diagnostic_limits", Json::array()}};
            if (!options.depthTest)
                metadata["limitations"].push_back("Ignoring depth/stencil also changes the after_draw image "
                                                  "to show that requested experiment.");
            if (isolated)
                metadata["diagnostic_limits"].push_back(
                    "Coverage is measured in a separate draw on private output copies; nondeterministic "
                    "shader/UAV ordering can differ from the original draw.");
        }
        metadata["requested_target"] = options.target;
        if (samples > 1)
            metadata.update(
                {{"initial_samples_reconstructed", false},
                 {"initialization_note",
                  "Pre-capture per-sample contents are not reconstructed. Regions not written by recorded "
                  "commands or explicit experiment assets may differ from the captured application."}});
        std::string mode = viewport            ? "viewport_fragments"
                           : options.depthTest ? "tested_fragments"
                                               : "fragments_without_depth_stencil";
        if (geometry) {
            metadata["instrumentation"] = "isolated_geometry_replacement";
            metadata["limitations"] = {
                "Replacement PS ignores original PS discard, alpha-to-coverage and SV_Depth output.",
                "Geometry tests use read-only captured depth/stencil; ignoring them affects only the "
                "diagnostic draw.",
                "Coverage is a sample union in the selected target or viewport coordinates; later passes may "
                "hide it."};
            mode = options.depthTest ? "depth_stencil_tested_geometry" : "rasterized_geometry";
        }
        if (buffer) {
            const std::string note =
                "Coordinates and byte ranges are derived from the bound buffer view. Coverage/Quad "
                "diagnostics measure raster positions, not actual GPU memory write addresses. A captured "
                "sparse buffer RTV produced driver-dependent writes; inspect event output bytes separately.";
            metadata["coordinates_are_observed_write_addresses"] = false;
            metadata["buffer_coordinate_note"] = note;
            metadata["limitations"].push_back(note);
        }
        result.overlay = result.after;
        uint64_t covered = 0;
        uint32_t left = width, top = height, right = 0, bottom = 0;
        for (uint32_t y = 0; y < height; ++y)
            for (uint32_t x = 0; x < width; ++x) {
                const auto at = (size_t(y) * width + x) * 4;
                if (!result.mask.rgba[at])
                    continue;
                ++covered;
                left = std::min(left, x);
                right = std::max(right, x);
                top = std::min(top, y);
                bottom = std::max(bottom, y);
                auto &rgba = result.overlay.rgba;
                rgba[at] = uint8_t((unsigned(rgba[at]) + 255) / 2);
                rgba[at + 1] /= 2;
                rgba[at + 2] = uint8_t((unsigned(rgba[at + 2]) + 255) / 2);
                rgba[at + 3] = 255;
            }
        result.report = metadata;
        result.report.update(
            {{"event", inspectionEvent(r.frame_, eventId)},
             {"mode", mode},
             {"covered_pixels", covered},
             {"bounds_inclusive", covered ? Json::array({left, top, right, bottom}) : Json(nullptr)}});
    }

  public:
    CoverageCapture(Replay &replay, Id id, const CoverageOptions &o)
        : r(replay), eventId(id), options(o), geometry(o.mode == "geometry") {}
    CoverageResult run() {
        if (options.mode != "fragment" && options.mode != "geometry")
            throw std::runtime_error("Unknown coverage mode");
        if (options.target != "auto" && options.target != "depth" &&
            !(options.target.size() == 3 && options.target.starts_with("rt") && options.target[2] >= '0' &&
              options.target[2] <= '7'))
            throw std::runtime_error("Unknown coverage target");
        event = r.frame_.event(eventId);
        if (event.type < 0x37 || event.type > 0x3d)
            throw std::runtime_error("Coverage requires a draw event");
        if (r.options_.until != eventId || r.options_.before || r.options_.timings || r.options_.measurement)
            throw std::runtime_error("Coverage requires its own inclusive replay boundary");
        // Reject an unavailable explicit target before any recorded command runs,
        // using the same edited binding history as the eventual draw.
        if (options.target != "auto") {
            const auto initial =
                effectiveBindings(r.frame_, eventId, r.frame_.state(event.state), r.options_);
            const auto colors = std::min({initial.rtCount, initial.omStart, 8u});
            const bool valid = options.target == "depth" ? initial.dsv != 0
                                                         : unsigned(options.target[2] - '0') < colors &&
                                                               initial.rtv[options.target[2] - '0'] != 0;
            if (!valid)
                throw std::runtime_error("Coverage target must name a bound RTV slot or depth/stencil view");
        }
        r.run({}, [&](Id id, bool afterDraw, auto *, const auto &) {
            if (id == eventId) {
                if (afterDraw)
                    after();
                else
                    before();
            }
        });
        if (!read)
            throw std::runtime_error("Coverage draw boundary was not executed");
        report();
        return std::move(result);
    }
};
CoverageResult captureCoverage(Replay &replay, Id event, const CoverageOptions &options) {
    return CoverageCapture(replay, event, options).run();
}
void exportCoverage(const CoverageResult &result, const std::filesystem::path &directory) {
    const auto folder = QString::fromStdWString(directory.wstring());
    QDir dir(folder);
    if (!dir.mkpath("."))
        throw std::runtime_error("Cannot create coverage output directory");
    for (const auto &[name, image] :
         std::array<std::pair<const char *, const Image *>, 3>{{{"coverage.png", &result.mask},
                                                                {"after_draw.png", &result.after},
                                                                {"overlay.png", &result.overlay}}}) {
        QImage pixels(image->rgba.data(), int(image->width), int(image->height), int(image->width * 4),
                      QImage::Format_RGBA8888);
        if (!pixels.save(dir.filePath(name)))
            throw std::runtime_error("Cannot save coverage image");
    }
    QSaveFile file(dir.filePath("coverage.json"));
    const auto data = QByteArray::fromStdString(result.report.dump(2));
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit())
        throw std::runtime_error("Cannot save coverage report");
}
} // namespace flora
