#include "Replay.h"
#include "BlendState.h"
#include "Device.h"
#include "NativeSample.h"
#include "RenderDocCapture.h"
#include "ReplayAnnotation.h"
#include "Unpredicated.h"
#include "core/ClassLinkage.h"
#include "core/ClearView.h"
#include "core/Commands.h"
#include "core/ContextStateRecords.h"
#include "core/CopyCommands.h"
#include "core/DiscardRecords.h"
#include "core/Dxbc.h"
#include "core/InspectionRecords.h"
#include "core/OutputBindings.h"
#include "core/PipelineGetters.h"
#include "core/Predication.h"
#include "core/PresentRecords.h"
#include "core/ReplayCapabilities.h"
#include "core/StreamOutput.h"
#include "core/TextureStorage.h"
#include "core/UavCounters.h"
#include <algorithm>
#include <chrono>
#include <d3d11sdklayers.h>
#include <d3d11shader.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <iostream>
#include <sstream>
#include <thread>

namespace flora {
void check(HRESULT hr, const char *op) {
    if (FAILED(hr)) {
        std::ostringstream s;
        s << op << ": HRESULT 0x" << std::hex << uint32_t(hr);
        throw std::runtime_error(s.str());
    }
}
template <class T> T descriptor(Bytes bytes) {
    if (bytes.size() != sizeof(T))
        throw std::runtime_error("Descriptor size mismatch");
    T out{};
    std::memcpy(&out, bytes.data(), sizeof out);
    return out;
}
template <class T> std::vector<T> optional(Reader &r, uint32_t count, uint32_t limit = 64) {
    if (!r.flag())
        return {};
    if (count > limit)
        throw std::runtime_error("Array count exceeds API limit");
    std::vector<T> out;
    out.reserve(count);
    for (uint32_t i = 0; i < count; ++i)
        out.push_back(r.read<T>());
    return out;
}
Replay::Replay(const Frame &frame, ReplayOptions options)
    : frame_(effectiveFrame(frame, options)), options_(std::move(options)) {
    if (options_.viewFrame && frame_.sha256() != frame.sha256())
        throw std::runtime_error("View overlay belongs to a different capture");
    if (options_.renderdocLibrary.empty() != options_.renderdocOutput.empty())
        throw std::runtime_error("RenderDoc library and output must be specified together");
    if (!options_.renderdocLibrary.empty())
        renderdoc_ = std::make_unique<RenderDocCapture>(options_.renderdocLibrary, options_.renderdocOutput);
    auto selected = createDx11Device(options_.warp, options_.debug, options_.vendor);
    device_ = std::move(selected.device);
    context_ = std::move(selected.context);
    check(context_.As(&context1_), "Query DX11.1 context");
    uavLimit_ = selected.level >= D3D_FEATURE_LEVEL_11_1 ? 64 : 8;
    if (renderdoc_) {
        check(context_.As(&captureAnnotation_), "Query capture annotations");
        renderdoc_->start(device_.Get());
    }
}
Replay::~Replay() {
    if (renderdoc_)
        renderdoc_->discard();
    resetPredicates();
    if (context_) {
        context_->ClearState();
        context_->Flush();
    }
}
std::optional<std::filesystem::path> Replay::finishCapture() {
    return renderdoc_ ? std::optional(renderdoc_->finish()) : std::nullopt;
}
DXGI_ADAPTER_DESC Replay::adapterDescription() const {
    Com<IDXGIDevice> dxgi;
    Com<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC desc{};
    check(device_.As(&dxgi), "Query DXGI device");
    check(dxgi->GetAdapter(&adapter), "Get adapter");
    check(adapter->GetDesc(&desc), "Get adapter description");
    return desc;
}
std::string Replay::adapter() const {
    auto desc = adapterDescription();
    int size = WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, nullptr, 0, nullptr, nullptr);
    std::string text(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, text.data(), size, nullptr, nullptr);
    text.pop_back();
    return text;
}
void Replay::immediate(Id id) const { requireImmediateContext(frame_, id); }
IUnknown *Replay::object(Id id) {
    if (!id)
        return nullptr;
    if (!predicateCreationAudit_)
        predicateCreationAudit_ = auditPredicateCreations(frame_);
    if (auto it = predicateCreationAudit_->creationEvents.find(id);
        it != predicateCreationAudit_->creationEvents.end() && !objects_.contains(id)) {
        requirePredicateCreation(*predicateCreationAudit_, it->second);
        throw std::runtime_error("Predicate " + std::to_string(id) + " unavailable before creation event " +
                                 std::to_string(it->second));
    }
    if (!bufferCreationAudit_)
        bufferCreationAudit_ = auditBufferCreations(frame_);
    if (auto it = bufferCreationAudit_->creationEvents.find(id);
        it != bufferCreationAudit_->creationEvents.end() && !objects_.contains(id))
        throw std::runtime_error("Resource " + std::to_string(id) +
                                 " is not available before CreateBuffer event " + std::to_string(it->second));
    if (!textureCreationAudit_)
        textureCreationAudit_ = auditTextureCreations(frame_);
    if (auto it = textureCreationAudit_->creationEvents.find(id);
        it != textureCreationAudit_->creationEvents.end() && !objects_.contains(id))
        throw std::runtime_error("Resource " + std::to_string(id) +
                                 " is not available before texture/view creation event " +
                                 std::to_string(it->second));
    if (!classCreationAudit_)
        classCreationAudit_ = auditClassCreations(frame_);
    id = canonicalClassLinkage(classCreationAudit_->identities, id);
    if (auto it = classCreationAudit_->creationEvents.find(id);
        it != classCreationAudit_->creationEvents.end() && !objects_.contains(id)) {
        const auto &c = requireClassCreation(*classCreationAudit_, it->second);
        if (!c.note.empty())
            throw std::runtime_error("Class resource " + std::to_string(id) + ": " + c.note);
        throw std::runtime_error("Class resource " + std::to_string(id) +
                                 " unavailable before creation event " + std::to_string(it->second));
    }
    if (!pipelineCreationAudit_)
        pipelineCreationAudit_ = auditPipelineCreations(frame_);
    if (auto it = pipelineCreationAudit_->creationEvents.find(id);
        it != pipelineCreationAudit_->creationEvents.end() && !objects_.contains(id)) {
        const auto &creation = requirePipelineCreation(*pipelineCreationAudit_, it->second);
        if (!creation.note.empty())
            throw std::runtime_error("Pipeline resource " + std::to_string(id) + ": " + creation.note);
        if (!isStateCreation(creation.type))
            throw std::runtime_error("Pipeline resource " + std::to_string(id) +
                                     " is unavailable before creation event " + std::to_string(it->second));
    }
    if (auto it = objects_.find(id); it != objects_.end())
        return it->second.Get();
    const auto resource = frame_.resource(id);
    const auto p = frame_.payload(id);
    Reader r(p);
    r.skip(16);
    const auto t = resource.type;
    Com<IUnknown> result;
    try {
        if (t == 0x83) {
            auto desc = r.read<D3D11_BUFFER_DESC>();
            auto dataId = r.read<Id>();
            r.end();
            auto data = dataId ? frame_.data(dataId) : Bytes{};
            if (dataId && data.size() != desc.ByteWidth)
                throw std::runtime_error("Buffer initial size mismatch");
            D3D11_SUBRESOURCE_DATA initial{data.data(), 0, 0};
            Com<ID3D11Buffer> obj;
            check(device_->CreateBuffer(&desc, dataId ? &initial : nullptr, &obj), "CreateBuffer");
            result = obj;
        } else if (t >= 0x84 && t <= 0x87) {
            auto d = resource.desc;
            auto info = textureInfo(resource);
            if (!info.mips || info.mips > 32 || !info.layers)
                throw std::runtime_error("Invalid mip/layer count");
            if (t == 0x87)
                throw std::runtime_error("Capture-end reference pixels are not replay resources");
            if ((info.format == 104 || info.format == 105) && resource.data &&
                !options_.textures.contains(id))
                throw std::runtime_error("Legacy GPA P010/P016 GenData is not standard DXGI storage; use "
                                         "captured Y export or a verified complete texture replacement");
            if (auto it = frame_.entries().find(resource.device);
                it != frame_.entries().end() && it->second.type == 0x38) {
                if (d[4] == 28)
                    d[4] = 27;
                else if (d[4] == 87)
                    d[4] = 90;
                else if (d[4] == 88)
                    d[4] = 92;
            }
            auto data = resource.data && info.samples == 1 ? frame_.data(resource.data) : Bytes{};
            if (resource.data && info.samples > 1)
                ignoredMsaaInitial_.push_back(id);
            if (auto it = options_.textures.find(id); it != options_.textures.end())
                data = it->second;
            if (uint64_t(info.mips) * info.layers > 30720)
                throw std::runtime_error("Too many texture subresources");
            std::vector<D3D11_SUBRESOURCE_DATA> initial;
            if ((resource.data && info.samples == 1) || options_.textures.contains(id)) {
                try {
                    for (const auto &sub : textureInitialSubresources(resource, data))
                        initial.push_back({data.data() + size_t(sub.offset), sub.rowPitch, UINT(sub.slicePitch)});
                } catch (const std::exception &error) {
                    throw std::runtime_error("Texture initial data " + std::to_string(resource.data) +
                                             ": " + error.what());
                }
            }
            auto init = initial.empty() ? nullptr : initial.data();
            Bytes descBytes(reinterpret_cast<const uint8_t *>(d.data()), d.size() * 4);
            if (t == 0x84) {
                auto desc = descriptor<D3D11_TEXTURE1D_DESC>(descBytes);
                Com<ID3D11Texture1D> obj;
                check(device_->CreateTexture1D(&desc, init, &obj), "CreateTexture1D");
                result = obj;
            } else if (t == 0x86) {
                auto desc = descriptor<D3D11_TEXTURE3D_DESC>(descBytes);
                Com<ID3D11Texture3D> obj;
                check(device_->CreateTexture3D(&desc, init, &obj), "CreateTexture3D");
                result = obj;
            } else {
                auto desc = descriptor<D3D11_TEXTURE2D_DESC>(descBytes);
                Com<ID3D11Texture2D> obj;
                check(device_->CreateTexture2D(&desc, init, &obj), "CreateTexture2D");
                result = obj;
            }
        } else if (t >= 0x8c && t <= 0x8f) {
            auto source = get<ID3D11Resource>(r.read<Id>());
            auto raw = r.take(r.remaining());
            if (t == 0x8c) {
                auto desc = descriptor<D3D11_SHADER_RESOURCE_VIEW_DESC>(raw);
                Com<ID3D11ShaderResourceView> obj;
                check(device_->CreateShaderResourceView(source, &desc, &obj), "CreateSRV");
                result = obj;
            }
            if (t == 0x8d) {
                auto desc = descriptor<D3D11_RENDER_TARGET_VIEW_DESC>(raw);
                Com<ID3D11RenderTargetView> obj;
                check(device_->CreateRenderTargetView(source, &desc, &obj), "CreateRTV");
                result = obj;
            }
            if (t == 0x8e) {
                auto desc = descriptor<D3D11_DEPTH_STENCIL_VIEW_DESC>(raw);
                Com<ID3D11DepthStencilView> obj;
                check(device_->CreateDepthStencilView(source, &desc, &obj), "CreateDSV");
                result = obj;
            }
            if (t == 0x8f) {
                auto desc = descriptor<D3D11_UNORDERED_ACCESS_VIEW_DESC>(raw);
                Com<ID3D11UnorderedAccessView> obj;
                check(device_->CreateUnorderedAccessView(source, &desc, &obj), "CreateUAV");
                if (auto initial = options_.initialUavCounters.find(id);
                    initial != options_.initialUavCounters.end())
                    writeCounter(obj.Get(), initial->second);
                else if (describeCounter(frame_, id))
                    missingInitialCounters_.insert(id);
                result = obj;
            }
        } else if (t == 0x96) {
            auto predicate = createPredicate(id);
            Unpredicated guard(context_.Get());
            context_->Begin(predicate.Get());
            context_->End(predicate.Get());
            baselinePredicates_.insert(id);
            result = predicate;
        } else if (t == 0x97) {
            readClassRecord(frame_, id);
            Com<ID3D11ClassLinkage> obj;
            check(device_->CreateClassLinkage(&obj), "CreateClassLinkage");
            result = obj;
        } else if (t == 0x98) {
            const auto record = readClassRecord(frame_, id);
            auto linkage = get<ID3D11ClassLinkage>(record.linkage);
            Com<ID3D11ClassInstance> obj;
            const auto &d = record.desc;
            if (d[7])
                check(linkage->CreateClassInstance(record.typeName.c_str(), d[3], d[4], d[5], d[6], &obj),
                      "CreateClassInstance");
            else
                check(linkage->GetClassInstance(record.instanceName.c_str(), d[1], &obj), "GetClassInstance");
            result = obj;
        } else if (t >= 0x90 && t <= 0x95) {
            auto data = frame_.shader(resource.data);
            if (auto it = options_.shaders.find(id); it != options_.shaders.end())
                data = it->second;
            Reader head(p);
            head.skip(32);
            auto linkage = shaderClassLinkage(frame_, id);
            head.skip(8);
            auto so = head.read<Id>();
            result = createCapturedShader(id, t, data, linkage, so);
        } else if (t == 0x82) {
            auto dataId = r.read<Id>();
            r.end();
            Reader layout(frame_.payload(dataId, 9, 0x84));
            auto n = layout.read<uint32_t>();
            if (n > 32)
                throw std::runtime_error("Input layout element limit");
            std::vector<D3D11_INPUT_ELEMENT_DESC> elements;
            std::vector<std::string> names;
            names.reserve(n);
            elements.reserve(n);
            for (UINT i = 0; i < n; ++i) {
                auto name = frame_.data(layout.read<Id>());
                if (name.empty() || name.back() != 0)
                    throw std::runtime_error("Input semantic is not terminated");
                names.emplace_back(reinterpret_cast<const char *>(name.data()), name.size() - 1);
                D3D11_INPUT_ELEMENT_DESC e{};
                e.SemanticName = names.back().c_str();
                e.SemanticIndex = layout.read<UINT>();
                e.Format = DXGI_FORMAT(layout.read<UINT>());
                e.InputSlot = layout.read<UINT>();
                e.AlignedByteOffset = layout.read<UINT>();
                e.InputSlotClass = D3D11_INPUT_CLASSIFICATION(layout.read<UINT>());
                e.InstanceDataStepRate = layout.read<UINT>();
                elements.push_back(e);
            }
            auto signature = layout.take(layout.read<UINT>());
            layout.end();
            Com<ID3D11InputLayout> obj;
            check(device_->CreateInputLayout(elements.data(), n, signature.data(), signature.size(), &obj),
                  "CreateInputLayout");
            result = obj;
        } else if (t == 0x88) {
            auto desc = descriptor<D3D11_SAMPLER_DESC>(r.take(r.remaining()));
            Com<ID3D11SamplerState> obj;
            check(device_->CreateSamplerState(&desc, &obj), "CreateSamplerState");
            result = obj;
        } else if (t == 0x8b) {
            auto desc = descriptor<D3D11_DEPTH_STENCIL_DESC>(r.take(r.remaining()));
            Com<ID3D11DepthStencilState> obj;
            check(device_->CreateDepthStencilState(&desc, &obj), "CreateDepthStencilState");
            result = obj;
        } else if (t == 0x89 || t == 0x10e || t == 0x10f) {
            auto raw = r.take(r.remaining());
            auto size = t == 0x89 ? 40 : t == 0x10e ? 44 : 48;
            if (raw.size() != size)
                throw std::runtime_error("Rasterizer size mismatch");
            D3D11_RASTERIZER_DESC2 desc{};
            std::memcpy(&desc, raw.data(), raw.size());
            result = createRasterizer(desc);
        } else if (t == 0x8a || t == 0x10d) {
            result = createBlend(decodeBlend(r.take(r.remaining()), t == 0x10d));
        } else
            throw std::runtime_error("Resource migration pending: " + resourceName(t) + " type " +
                                     std::to_string(t));
    } catch (const std::exception &e) {
        throw std::runtime_error("Resource " + std::to_string(id) + ": " + e.what());
    }
    if (hasResourceLodClamp(resource) && resourceLods_.contains(id)) {
        Com<ID3D11Resource> texture;
        check(result.As(&texture), "Query initial LOD resource");
        context_->SetResourceMinLOD(texture.Get(), resourceLods_.at(id));
        ++counts["resource_lod_initial_restores"];
    }
    if (renderdoc_ && result) {
        Com<ID3D11DeviceChild> child;
        if (SUCCEEDED(result.As(&child))) {
            std::ostringstream name;
            name << "GPA resource " << id << " (type 0x" << std::hex << frame_.entry(id).type << ')';
            const auto value = name.str();
            check(child->SetPrivateData(WKPDID_D3DDebugObjectName, UINT(value.size()), value.data()),
                  "Name captured resource");
        }
    }
    objects_.emplace(id, result);
    return result.Get();
}
void Replay::setRange(int stage, UINT slot, ID3D11Buffer *buffer, const Range &range) {
    using Fn = void (STDMETHODCALLTYPE ID3D11DeviceContext1::*)(UINT, UINT, ID3D11Buffer *const *,
                                                                const UINT *, const UINT *);
    static constexpr Fn fns[] = {
        &ID3D11DeviceContext1::VSSetConstantBuffers1, &ID3D11DeviceContext1::HSSetConstantBuffers1,
        &ID3D11DeviceContext1::DSSetConstantBuffers1, &ID3D11DeviceContext1::GSSetConstantBuffers1,
        &ID3D11DeviceContext1::PSSetConstantBuffers1, &ID3D11DeviceContext1::CSSetConstantBuffers1};
    if (!range.window) {
        using Basic = void (STDMETHODCALLTYPE ID3D11DeviceContext::*)(UINT, UINT, ID3D11Buffer *const *);
        static constexpr Basic basic[] = {
            &ID3D11DeviceContext::VSSetConstantBuffers, &ID3D11DeviceContext::HSSetConstantBuffers,
            &ID3D11DeviceContext::DSSetConstantBuffers, &ID3D11DeviceContext::GSSetConstantBuffers,
            &ID3D11DeviceContext::PSSetConstantBuffers, &ID3D11DeviceContext::CSSetConstantBuffers};
        (context_.Get()->*basic[stage])(slot, 1, &buffer);
    } else
        (context1_.Get()->*fns[stage])(slot, 1, &buffer, &range.first, &range.count);
}
void Replay::bind(const State &s, bool compute) {
    const bool extended =
        std::any_of(s.omExtended.begin(), s.omExtended.end(), [](Id x) { return x != 0; }) ||
        std::any_of(s.csExtended.begin(), s.csExtended.end(), [](Id x) { return x != 0; });
    if (extended || outputHistory_)
        validateOutputSnapshot(s);
    if (options_.debug)
        std::cerr << "state " << s.ib << " " << s.layout << " " << s.topology << " vp " << s.viewports
                  << " sc " << s.scissors << " mask " << s.sampleMask << " shader " << s.stages[0].shader
                  << " " << s.stages[4].shader << " cb " << s.stages[0].cb[0] << " " << s.stages[4].cb[0]
                  << " vb " << s.vb[0] << " " << s.strides[0] << " " << s.offsets[0] << "\n";
    std::array<ID3D11UnorderedAccessView *, 64> empty{};
    context_->OMSetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, uavLimit_, empty.data(),
                                                        nullptr);
    context_->CSSetUnorderedAccessViews(0, uavLimit_, empty.data(), nullptr);
    unbindStreamOutput();
    context_->IASetInputLayout(get<ID3D11InputLayout>(s.layout));
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY(s.topology));
    context_->IASetIndexBuffer(get<ID3D11Buffer>(s.ib), DXGI_FORMAT(s.ibFormat), s.ibOffset);
    std::array<ID3D11Buffer *, 32> vb{};
    for (size_t i = 0; i < 32; ++i)
        vb[i] = get<ID3D11Buffer>(s.vb[i]);
    context_->IASetVertexBuffers(0, 32, vb.data(), s.strides.data(), s.offsets.data());
    using CB = void (STDMETHODCALLTYPE ID3D11DeviceContext::*)(UINT, UINT, ID3D11Buffer *const *);
    using SRV =
        void (STDMETHODCALLTYPE ID3D11DeviceContext::*)(UINT, UINT, ID3D11ShaderResourceView *const *);
    using SAM = void (STDMETHODCALLTYPE ID3D11DeviceContext::*)(UINT, UINT, ID3D11SamplerState *const *);
    static constexpr CB cbFns[] = {
        &ID3D11DeviceContext::VSSetConstantBuffers, &ID3D11DeviceContext::HSSetConstantBuffers,
        &ID3D11DeviceContext::DSSetConstantBuffers, &ID3D11DeviceContext::GSSetConstantBuffers,
        &ID3D11DeviceContext::PSSetConstantBuffers, &ID3D11DeviceContext::CSSetConstantBuffers};
    static constexpr SRV srvFns[] = {
        &ID3D11DeviceContext::VSSetShaderResources, &ID3D11DeviceContext::HSSetShaderResources,
        &ID3D11DeviceContext::DSSetShaderResources, &ID3D11DeviceContext::GSSetShaderResources,
        &ID3D11DeviceContext::PSSetShaderResources, &ID3D11DeviceContext::CSSetShaderResources};
    static constexpr SAM samFns[] = {
        &ID3D11DeviceContext::VSSetSamplers, &ID3D11DeviceContext::HSSetSamplers,
        &ID3D11DeviceContext::DSSetSamplers, &ID3D11DeviceContext::GSSetSamplers,
        &ID3D11DeviceContext::PSSetSamplers, &ID3D11DeviceContext::CSSetSamplers};
    for (int stage = 0; stage < 6; ++stage) {
        auto &x = s.stages[stage];
        if (x.classCount > x.classes.size())
            throw std::runtime_error("Shader class count exceeds 256");
        bindShader(stage, x.shader, std::span(x.classes).first(x.classCount));
        auto shader = object(x.shader);
        std::array<ID3D11Buffer *, 14> cb{};
        std::array<ID3D11SamplerState *, 16> sam{};
        std::array<ID3D11ShaderResourceView *, 128> srv{};
        for (size_t i = 0; i < cb.size(); ++i)
            cb[i] = get<ID3D11Buffer>(x.cb[i]);
        for (size_t i = 0; i < sam.size(); ++i)
            sam[i] = get<ID3D11SamplerState>(x.samplers[i]);
        for (size_t i = 0; i < srv.size(); ++i)
            if (srvBindings_.overrides(unsigned(stage)).contains(unsigned(i)) ||
                (outputHistory_ &&
                 outputHistory_->delta(bindingEvent_)
                     .contains(std::string(samplerStageNames[stage]) + ".srv." + std::to_string(i))) ||
                (shader && !passthroughShaders_.contains(x.shader) &&
                 (x.classCount || usedSrvs_.at(x.shader)[i])))
                srv[i] = get<ID3D11ShaderResourceView>(x.srv[i]);
        (context_.Get()->*cbFns[stage])(0, 14, cb.data());
        (context_.Get()->*srvFns[stage])(0, 128, srv.data());
        (context_.Get()->*samFns[stage])(0, 16, sam.data());
        for (auto &[slot, range] : ranges_[stage]) {
            if (x.cb[slot] != range.buffer)
                throw std::runtime_error("CB1 range disagrees with snapshot");
            setRange(stage, slot, cb[slot], range);
        }
    }
    context_->RSSetState(get<ID3D11RasterizerState>(s.rasterizer));
    if (s.viewportValues) {
        if (device_->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0)
            throw std::runtime_error("Viewport editing requires feature level 11_0");
        std::vector<D3D11_VIEWPORT> rows;
        for (const auto &v : *s.viewportValues)
            rows.push_back({v[0], v[1], v[2], v[3], v[4], v[5]});
        context_->RSSetViewports(UINT(rows.size()), rows.empty() ? nullptr : rows.data());
    } else if (s.viewports) {
        Reader r(frame_.payload(s.viewports, 9, 0x87));
        auto n = r.read<UINT>();
        if (n > 16)
            throw std::runtime_error("Viewport limit");
        std::vector<D3D11_VIEWPORT> v;
        for (UINT i = 0; i < n; ++i)
            v.push_back(r.read<D3D11_VIEWPORT>());
        r.end();
        context_->RSSetViewports(n, v.data());
    } else
        context_->RSSetViewports(0, nullptr);
    if (s.scissorValues) {
        std::vector<D3D11_RECT> rows;
        for (const auto &v : *s.scissorValues)
            rows.push_back({v[0], v[1], v[2], v[3]});
        context_->RSSetScissorRects(UINT(rows.size()), rows.empty() ? nullptr : rows.data());
    } else if (s.scissors) {
        Reader r(frame_.payload(s.scissors, 9, 0x86));
        auto n = r.read<UINT>();
        if (n > 16)
            throw std::runtime_error("Scissor limit");
        std::vector<D3D11_RECT> v;
        for (UINT i = 0; i < n; ++i)
            v.push_back(r.read<D3D11_RECT>());
        r.end();
        context_->RSSetScissorRects(n, v.data());
    } else
        context_->RSSetScissorRects(0, nullptr);
    context_->OMSetBlendState(get<ID3D11BlendState>(s.blend), s.blendFactor.data(), s.sampleMask);
    context_->OMSetDepthStencilState(get<ID3D11DepthStencilState>(s.depthState), s.stencilRef);
    const auto predicate = predicateBinding_.value_or(PredicateBinding{s.predicate, s.predicateValue});
    if (conditionPredicates_.contains(predicate.resource) && !predicateBinding_)
        throw std::runtime_error("Missing Predicate snapshot has no established normalized setter");
    bindPredicate(predicate.resource, predicate.value);
    if (compute || extended || outputHistory_) {
        std::array<ID3D11UnorderedAccessView *, 64> uavs{};
        for (unsigned i = 0; i < uavLimit_; ++i)
            uavs[i] = get<ID3D11UnorderedAccessView>(i < 8 ? s.csUav[i] : s.csExtended[i - 8]);
        context_->CSSetUnorderedAccessViews(0, uavLimit_, uavs.data(), nullptr);
        if (compute && !extended && !outputHistory_)
            return;
    }
    if (s.rtCount > uavLimit_ || s.omStart > 64)
        throw std::runtime_error("Output snapshot limit");
    bindStreamOutput(s);
    auto rtCount = std::min({s.rtCount, s.omStart, 8u});
    std::array<ID3D11RenderTargetView *, 8> rt{};
    for (UINT i = 0; i < rtCount; ++i)
        rt[i] = get<ID3D11RenderTargetView>(s.rtv[i]);
    auto depth = get<ID3D11DepthStencilView>(s.dsv);
    if (s.omStart < s.rtCount) {
        std::array<ID3D11UnorderedAccessView *, 64> uav{};
        for (UINT i = s.omStart; i < s.rtCount; ++i)
            uav[i - s.omStart] = get<ID3D11UnorderedAccessView>(i < 8 ? s.rtv[i] : s.omExtended[i - 8]);
        context_->OMSetRenderTargetsAndUnorderedAccessViews(rtCount, rt.data(), depth, s.omStart,
                                                            s.rtCount - s.omStart, uav.data(), nullptr);
    } else
        context_->OMSetRenderTargets(rtCount, rt.data(), depth);
    validateBlendOutputs(s, get<ID3D11BlendState>(s.blend));
    if (!compute && rtCount && s.rtv[0]) {
        Reader r(frame_.payload(s.rtv[0]));
        r.skip(16);
        lastTarget_ = r.read<Id>();
        lastTargetView_ = s.rtv[0];
    }
    if (extended || outputHistory_)
        verifyOutputBindings(OutputBindingModel::snapshot(s));
    observeIaBindings(bindingEvent_);
}
Replay::ConstantRange Replay::constantRange(unsigned stage, uint32_t slot, Id buffer) const {
    const auto &ranges = ranges_.at(stage);
    auto it = ranges.find(slot);
    if (it == ranges.end())
        return {};
    if (it->second.buffer != buffer)
        throw std::runtime_error("CB1 range disagrees with snapshot");
    return {it->second.first, it->second.window ? std::optional<uint32_t>(it->second.count) : std::nullopt};
}
void Replay::constantBuffers(const Entry &e) {
    const auto original = readConstantBufferSetter(e.type, frame_.payload(e.id));
    const auto edited = options_.constantBufferSetters.find(e.id);
    auto b = edited == options_.constantBufferSetters.end() ? original : edited->second;
    if (b.type != e.type || b.context != original.context)
        throw std::runtime_error("CB setter type or context mismatch");
    if (std::any_of(b.buffers.begin(), b.buffers.end(),
                    [&](Id id) { return id && !frame_.entries().contains(id); })) {
        if (!allowUnusedCbLifetime_)
            throw std::runtime_error(
                "Absent constant buffers cannot supply native command-state observations");
        auto proof = proveUnusedConstantBufferLifetime(
            frame_, e.id, options_.disabled, options_.commandPayloads, options_.constantBufferSetters,
            options_.until, options_.before);
        // These slots cannot reach GPU work or an observer before their explicit close.
        // Null is temporary native bookkeeping, not recovered captured binding state.
        for (auto &id : b.buffers)
            if (id && !frame_.entries().contains(id))
                id = 0;
        unusedCbLifetimes_.push_back(std::move(proof));
        ++counts["unmaterialized_constant_buffer_setters"];
    }
    validateConstantBufferBinding(frame_, b);
    auto next = constantBufferBindings_;
    ConstantBufferObservations previous{};
    if (edited != options_.constantBufferSetters.end() && displacedConstantBuffers(original, b)) {
        if (!constantBufferHistory_)
            constantBufferHistory_ = std::make_unique<ConstantBufferHistory>(frame_);
        previous = constantBufferHistory_->advance(e.id);
    }
    next.transition(original, edited == options_.constantBufferSetters.end() ? nullptr : &b, previous);
    if (b.first) {
        D3D11_FEATURE_DATA_D3D11_OPTIONS support{};
        check(device_->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &support, sizeof support),
              "Query constant-buffer offsetting support");
        if (!support.ConstantBufferOffsetting)
            throw std::runtime_error("Driver does not support constant-buffer offsetting");
    }
    // Resolve every resource before mutating native bindings.
    std::vector<ID3D11Buffer *> buffers;
    for (auto id : b.buffers)
        buffers.push_back(get<ID3D11Buffer>(id));
    const auto stage = constantBufferStage(e.type).value();
    for (unsigned i = 0; i < b.buffers.size(); ++i) {
        auto range = constantBufferRow(b, i);
        setRange(stage, b.start + i, buffers[i], range);
        if (e.type >= 0x24f && e.type <= 0x254)
            ranges_[stage][b.start + i] = range;
        else
            ranges_[stage].erase(b.start + i);
    }
    constantBufferBindings_ = std::move(next);
}
bool Replay::outputs(const Entry &e, Bytes payload) {
    const auto command = readOutputCommand(e.type, payload);
    immediate(command.context);
    validateOutputRange(e.type, command, uavLimit_);
    const auto rtCount = command.rtvCount, start = command.start, count = command.uavCount;
    const auto dsv = command.dsv;
    const auto &rtvs = command.rtvs.value_or(std::vector<Id>{});
    const auto &uavs = command.uavs.value_or(std::vector<Id>{});
    const auto &initial = command.initialCounts.value_or(std::vector<uint32_t>{});
    const bool om = e.type == 0x34ff || e.type == 0x3500;
    // Captures may omit unused placeholder views. A later full snapshot resolves them.
    bool missing = false;
    for (auto id : rtvs)
        if (id && !frame_.entries().contains(id))
            missing = true;
    for (auto id : uavs)
        if (id && !frame_.entries().contains(id))
            missing = true;
    if (rtCount != UINT_MAX && dsv && !frame_.entries().contains(dsv))
        missing = true;
    if (missing) {
        if (options_.outputSetters.contains(e.id))
            throw std::runtime_error("Edited output binding references a missing view");
        counts["unresolved_output_setters"]++;
        outputGap_ = e.id;
        return false;
    }
    auto viewType = [&](Id id, uint16_t expected) {
        if (id) {
            const auto &entry = frame_.entry(id);
            if (entry.category != 5 || entry.type != expected)
                throw std::runtime_error("Output binding view has the wrong resource type");
        }
    };
    for (auto id : rtvs)
        viewType(id, 0x8d);
    for (auto id : uavs)
        viewType(id, 0x8f);
    if (rtCount != UINT_MAX)
        viewType(dsv, 0x8e);
    std::vector<ID3D11RenderTargetView *> rt;
    for (auto id : rtvs)
        rt.push_back(get<ID3D11RenderTargetView>(id));
    std::vector<ID3D11UnorderedAccessView *> ua;
    for (auto id : uavs)
        ua.push_back(get<ID3D11UnorderedAccessView>(id));
    auto init = initial.empty() ? nullptr : initial.data();
    if (e.type == 0x34ff)
        context_->OMSetRenderTargets(rtCount, rt.data(), get<ID3D11DepthStencilView>(dsv));
    else if (om)
        context_->OMSetRenderTargetsAndUnorderedAccessViews(
            rtCount, rt.data(), rtCount == UINT_MAX ? nullptr : get<ID3D11DepthStencilView>(dsv), start,
            count, ua.data(), init);
    else
        context_->CSSetUnorderedAccessViews(start, count, ua.data(), init);
    for (size_t i = 0; i < uavs.size() && i < initial.size(); ++i)
        if (initial[i] != UINT_MAX) {
            undefinedCreatedCounters_.erase(uavs[i]);
            missingInitialCounters_.erase(uavs[i]);
        }
    return true;
}
void Replay::mappedWrites(const Entry &e) {
    if (!mapRecordAudit_)
        mapRecordAudit_ = auditMapRecords(frame_);
    const auto &record = requireMapRecord(*mapRecordAudit_, e.id);
    if (record.result < 0)
        return;
    const auto layout = mappedWriteLayout(frame_, record);
    const auto id = record.resource;
    const auto sub = record.subresource, kind = record.kind, flags = record.flags;
    const auto row = layout.row, rows = layout.rows, depth = layout.depth;
    const Bytes tight = layout.recoveredLuma.empty() ? layout.tight : Bytes(layout.recoveredLuma);
    auto obj = get<ID3D11Resource>(id);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    ReplayAnnotation marker(captureAnnotation_.Get(), e.id, "MapCapturedWrites");
    check(context_->Map(obj, sub, D3D11_MAP(kind), flags, &mapped), "Map captured writes");
    try {
        if (!mapped.pData)
            throw std::runtime_error("Map returned a null data pointer");
        if (layout.texture && layout.full) {
            if (mapped.RowPitch < row || (depth > 1 && mapped.DepthPitch < uint64_t(mapped.RowPitch) * rows))
                throw std::runtime_error("Mapped pitch too small");
            for (UINT z = 0; z < depth; ++z)
                for (UINT y = 0; y < rows; ++y)
                    std::memcpy(static_cast<uint8_t *>(mapped.pData) + size_t(z) * mapped.DepthPitch +
                                    size_t(y) * mapped.RowPitch,
                                tight.data() + (size_t(z) * rows + y) * row, row);
        } else
            for (auto [offset, data] : layout.updates)
                std::memcpy(static_cast<uint8_t *>(mapped.pData) + offset, data.data(), data.size());
    } catch (...) {
        context_->Unmap(obj, sub);
        throw;
    }
    context_->Unmap(obj, sub);
    if (layout.planarFormat) {
        PlanarWrite write;
        write.event = e.id;
        write.resource = id;
        write.subresource = sub;
        write.format = layout.planarFormat;
        write.mapType = kind;
        write.sourceRowPitch = layout.sourceRowPitch;
        write.writtenRowBytes = row;
        write.nativeRowPitch = mapped.RowPitch;
        planarWrites_.push_back(write);
    }
    counts["Map"]++;
    lastWorkEvent_ = e.id;
}
State Replay::prepareState(const Event &event) {
    immediate(event.context);
    auto state = frame_.state(event.state);
    if (predicateBinding_) {
        state.predicate = predicateBinding_->resource;
        state.predicateValue = predicateBinding_->value;
    }
    for (const auto &[type, binding] : activePipelineBindings_)
        overlayPipelineBinding(state, binding);
    if (!outputHistory_)
        iaBindings_.hazards(frame_, srvOutputs(state));
    iaBindings_.apply(state, !outputHistory_);
    constantBufferBindings_.apply(state);
    samplerBindings_.observe(state);
    samplerBindings_.apply(state);
    bindingEvent_ = event.id;
    retainedSo_.fill(false);
    if (outputHistory_) {
        auto effective = outputHistory_->state(event.id, state);
        state = effective.state;
        retainedSo_ = effective.retainedSo;
    } else if (srvBindings_.active()) {
        srvBindings_.hazards(srvHazards_, srvOutputs(state));
        srvBindings_.apply(state);
    }
    return state;
}
void Replay::applyGraphicsEdits(const Event &event, const State &state) {
    const auto t = event.type;
    const auto &e = event;
    if ((t == 0x35 || t == 0x36) &&
        (options_.rasterizerEdits.contains(e.id) || options_.blendEdits.contains(e.id)))
        throw std::runtime_error("Graphics pipeline experiment on a dispatch");
    applyRasterizerEdit(e.id);
    if (auto edit = options_.depthStencilEdits.find(e.id); edit != options_.depthStencilEdits.end()) {
        if (t == 0x35 || t == 0x36)
            throw std::runtime_error("Graphics pipeline experiment on a dispatch");
        Com<ID3D11DepthStencilState> depth;
        if (edit->second.descriptor)
            check(device_->CreateDepthStencilState(&*edit->second.descriptor, &depth),
                  "Create edited depth/stencil state");
        else
            depth = get<ID3D11DepthStencilState>(state.depthState);
        context_->OMSetDepthStencilState(depth.Get(), edit->second.reference.value_or(state.stencilRef));
    }
    applyBlendEdit(e.id, state);
    applySamplerEdits(e.id);
    applySrvEdits(e.id, state);
}
void Replay::command(const Entry &e) {
    auto t = e.type;
    if (!isDraw(t) && options_.experiment && options_.experiment->events.contains(e.id))
        appliedExperimentEvents_.push_back(e.id);
    Bytes payload = frame_.payload(e.id);
    if (auto it = options_.commandPayloads.find(e.id); it != options_.commandPayloads.end())
        payload = it->second;
    if (auto it = options_.outputSetters.find(e.id); it != options_.outputSetters.end())
        payload = it->second;
    if (outputHistory_ && OutputBindingModel::models(e.type))
        outputHistory_->advance(e.id);
    Reader r(payload);
    if (finishCommandListVersion(t)) {
        if (!std::ranges::equal(payload, frame_.payload(e.id)))
            throw std::runtime_error("FinishCommandList payload experiments are not supported");
        acceptFinishCommandList(frame_, readFinishCommandList(t, payload));
        if (!options_.disabled.contains(e.id))
            ++counts["finish_command_list_metadata"];
        return;
    }
    if (isDiscardRecord(t)) {
        if (!std::ranges::equal(payload, frame_.payload(e.id)))
            throw std::runtime_error("Discard payload experiments are not supported");
        const auto record = readDiscardRecord(t, payload);
        validateDiscardRecord(frame_, record);
        if (!context1_)
            throw std::runtime_error("Native resource discard requires ID3D11DeviceContext1");
        if (options_.disabled.contains(e.id))
            return;
        ReplayAnnotation marker(captureAnnotation_.Get(), e.id, commandName(t).c_str());
        if (t == 0x3553)
            context1_->DiscardResource(get<ID3D11Resource>(record.target));
        else if (t == 0x3554)
            context1_->DiscardView(get<ID3D11View>(record.target));
        else {
            std::vector<D3D11_RECT> rects;
            for (const auto &rect : record.rectangles)
                rects.push_back({rect[0], rect[1], rect[2], rect[3]});
            context1_->DiscardView1(get<ID3D11View>(record.target),
                                    record.hasRectangles ? rects.data() : nullptr, record.count);
        }
        discardHistory_.emplace_back(e.id, record);
        ++counts[commandName(t)];
        return;
    }
    if (isContextStateRecord(t)) {
        if (!std::ranges::equal(payload, frame_.payload(e.id)))
            throw std::runtime_error("Context-state payload experiments are not supported");
        const auto record = readContextStateRecord(t, payload);
        validateContextStateOwner(frame_, e.id, record);
        if (const auto gap = contextStateReplayGap(record); !gap.empty())
            throw std::runtime_error("Event " + std::to_string(e.id) + ": " + gap);
        ++counts["device_creation_flags_observations"];
        return;
    }
    if (isPipelineGetter(t)) {
        if (!std::ranges::equal(payload, frame_.payload(e.id)))
            throw std::runtime_error("Pipeline getter payload experiments are not supported");
        validatePipelineGetter(frame_, readPipelineGetter(t, payload));
        if (!options_.disabled.contains(e.id))
            ++counts["pipeline_getter_observations"];
        return;
    }
    if (isResourceLodRecord(t)) {
        if (!std::ranges::equal(payload, frame_.payload(e.id)))
            throw std::runtime_error("Resource LOD payload experiments are not supported");
        const auto record = readResourceLod(t, payload);
        validateResourceLod(frame_, record);
        for (const auto &issue : resourceLodAudit_->issues)
            if (issue.event == e.id)
                throw std::runtime_error(issue.reason);
        if (options_.disabled.contains(e.id))
            return;
        if (record.setter) {
            resourceLods_[record.resource] = record.value;
            context_->SetResourceMinLOD(get<ID3D11Resource>(record.resource), record.value);
        }
        ++counts[commandName(t)];
        return;
    }
    if (t >= 0x3278 && t <= 0x327e) {
        validateAnnotationCommand(t, payload);
        ++counts["annotation_records"];
        return;
    }
    if (isMapObservation(t)) {
        if (!std::ranges::equal(payload, frame_.payload(e.id)))
            throw std::runtime_error("Map observation payload edits are not supported");
        if (!mapRecordAudit_)
            mapRecordAudit_ = auditMapRecords(frame_);
        requireMapRecord(*mapRecordAudit_, e.id);
        // CPU READ results cannot change GPU storage. Writable 0x246 playback
        // already maps, copies saved bytes and unmaps at its own event boundary.
        ++counts[t == 0x34ec ? "map_read_observations" : "unmap_observations"];
        return;
    }
    if (pipelineSetter(e, payload))
        return;
    if (isDraw(t)) {
        auto event = frame_.event(e.id);
        auto state = prepareState(event);
        withEventEdits(event, state, [&] {
            bind(state, t == 0x35 || t == 0x36);
            applyGraphicsEdits(event, state);
            clearBindingGaps();
            auto observe = [&](bool after) {
                if (boundaryObserver_)
                    boundaryObserver_(e.id, after, context_.Get(), objects_);
            };
            observe(false);
            const bool measureEvent =
                activeSample_ && options_.measurement->singleEvent && e.id == options_.measurement->start;
            if (measureEvent) {
                measuredPredicate_ = {state.predicate, state.predicateValue};
                activeSample_->begin();
            }
            if (!(options_.before && e.id == options_.until) && options_.experiment &&
                options_.experiment->events.contains(e.id))
                appliedExperimentEvents_.push_back(e.id);
            if ((options_.before && e.id == options_.until) || options_.disabled.contains(e.id) ||
                (options_.suppressDraws && t != 0x35 && t != 0x36)) {
                if (measureEvent)
                    activeSample_->end();
                if (!(options_.before && e.id == options_.until))
                    observe(true);
                return false;
            }
            requireBoundResourceLods(t == 0x35 || t == 0x36);
            if (!undefinedCreatedCounters_.empty() || !missingInitialCounters_.empty())
                for (const auto &counter : boundCounters(frame_, event, state))
                    requireCreatedCounter(counter.view);
            const auto &a = event.args;
            if (t != 0x35 && t != 0x36)
                validateRasterizer(state);
            auto autoCount = t == 0x38 ? drawAutoParameters(e.id).vertexCount : 0;
            auto arguments = event.argumentBuffer ? get<ID3D11Buffer>(event.argumentBuffer) : nullptr;
            auto activeStreams =
                t != 0x35 && t != 0x36 ? beginStreamOutput(state) : std::vector<ActiveStream>{};
            if (options_.timings) {
                Timestamp timestamp{e.id};
                D3D11_QUERY_DESC desc{D3D11_QUERY_TIMESTAMP, 0};
                check(device_->CreateQuery(&desc, &timestamp.begin), "Create timestamp");
                check(device_->CreateQuery(&desc, &timestamp.end), "Create timestamp");
                context_->End(timestamp.begin.Get());
                timestamps_.push_back(std::move(timestamp));
            }
            {
                ReplayAnnotation marker(captureAnnotation_.Get(), e.id, commandName(t), false);
                switch (t) {
                case 0x35:
                    context_->Dispatch(a[0], a[1], a[2]);
                    break;
                case 0x36:
                    context_->DispatchIndirect(arguments, a[0]);
                    break;
                case 0x37:
                    context_->Draw(a[0], a[1]);
                    break;
                case 0x38:
                    context_->Draw(autoCount, 0);
                    break;
                case 0x39:
                    context_->DrawIndexed(a[0], a[1], int32_t(a[2]));
                    break;
                case 0x3a:
                    context_->DrawIndexedInstanced(a[0], a[1], a[2], int32_t(a[3]), a[4]);
                    break;
                case 0x3b:
                    context_->DrawIndexedInstancedIndirect(arguments, a[0]);
                    break;
                case 0x3c:
                    context_->DrawInstanced(a[0], a[1], a[2], a[3]);
                    break;
                case 0x3d:
                    context_->DrawInstancedIndirect(arguments, a[0]);
                    break;
                }
            }
            // Queue the timestamp before SO readback can wait on the CPU.
            if (options_.timings)
                context_->End(timestamps_.back().end.Get());
            endStreamOutput(e.id, activeStreams);
            counts[commandName(t)]++;
            lastWorkEvent_ = e.id;
            if (measureEvent)
                activeSample_->end();
            observe(true);
            return true;
        });
        return;
    }
    if (isWritableCommand(t))
        validateWritableCommand(frame_, e.id);
    if (isCopyCommand(t))
        validateCopyCommand(frame_, readCopyCommand(t, payload));
    if (options_.disabled.contains(e.id))
        return;
    if (inputBindings(e, payload)) {
        counts["state_or_auxiliary_records"]++;
        return;
    }
    if (isStreamOutputTargets(t)) {
        if (!applyStreamOutput(e.id, payload))
            return;
        if (outputHistory_)
            verifyOutputBindings(outputHistory_->delta(e.id));
        observeSrvBindings(e.id);
        observeIaBindings(e.id);
        counts["SOSetTargets"]++;
        return;
    }
    if (predicateOperation(t)) {
        if (predicateOperation(t) == PredicateOperation::Set) {
            auto captured = readPredicateCommand(t, payload);
            immediate(captured.context);
            auto edit = options_.predicateSetters.find(e.id);
            const auto binding = edit == options_.predicateSetters.end()
                                     ? PredicateBinding{captured.resource, captured.value}
                                     : edit->second;
            // Explicit setters are authoritative until the next setter/ClearState.
            // Original captures can omit frame-created predicates from Draw snapshots.
            prepareNormalizedPredicate(e.id, binding.resource);
            bindPredicate(binding.resource, binding.value);
            predicateBinding_ = binding;
            counts["SetPredication"]++;
        } else
            applyPredicate(t, payload);
        return;
    }
    if ((t >= 0x249 && t <= 0x254) || t == 0x34e5 || t == 0x34ee || t == 0x34f4 || t == 0x351c ||
        t == 0x3520 || t == 0x3525) {
        constantBuffers(e);
        return;
    }
    if (t == 0x34ff || t == 0x3500 || t == 0x3522 || t == 0x25e) {
        const bool applied = outputs(e, payload);
        if (applied && outputHistory_)
            verifyOutputBindings(outputHistory_->delta(e.id));
        observeSrvBindings(e.id);
        observeIaBindings(e.id);
        return;
    }
    if (t == 0x246) {
        mappedWrites(e);
        return;
    }
    if (acceptQueryMetadata(t, payload)) {
        counts["query_metadata_records"]++;
        return;
    }
    if (acceptInspectionRecord(t, payload)) {
        counts["inspection_records"]++;
        return;
    }
    if (t == 0x3578) {
        if (!bufferCreationAudit_)
            bufferCreationAudit_ = auditBufferCreations(frame_);
        const auto &creation = requireBufferCreation(*bufferCreationAudit_, e.id);
        if (creation.result == 0) {
            if (objects_.contains(creation.resource))
                throw std::runtime_error("CreateBuffer identity already has materialized replay storage");
            D3D11_BUFFER_DESC desc{};
            static_assert(sizeof(desc) == sizeof(creation.descriptor));
            std::memcpy(&desc, creation.descriptor.data(), sizeof(desc));
            const auto data = creation.data ? frame_.data(creation.data) : Bytes{};
            D3D11_SUBRESOURCE_DATA initial{data.data(), 0, 0};
            Com<ID3D11Buffer> buffer;
            check(device_->CreateBuffer(&desc, creation.hasInitial ? &initial : nullptr, &buffer),
                  "Captured CreateBuffer");
            if (renderdoc_) {
                const auto name = "GPA resource " + std::to_string(creation.resource) + " (type 0x83)";
                check(buffer->SetPrivateData(WKPDID_D3DDebugObjectName, UINT(name.size()), name.data()),
                      "Name captured resource");
            }
            objects_.emplace(creation.resource, buffer);
            ++counts["CreateBuffer"];
            if (!creation.hasInitial)
                ++counts["buffers_created_without_initial_data"];
        } else
            ++counts["buffer_creation_observations"];
        return;
    }
    if (acceptTextureCreationObservation(t, payload)) {
        ++counts["texture_creation_observations"];
        return;
    }
    if (t == 0x358e) {
        predicateCreation(e);
        return;
    }
    if (isClassCreation(t)) {
        classCreation(e);
        return;
    }
    if (isPipelineCreation(t)) {
        pipelineCreation(e);
        return;
    }
    if (isTextureCreation(t)) {
        if (!textureCreationAudit_)
            textureCreationAudit_ = auditTextureCreations(frame_);
        const auto &creation = requireTextureCreation(*textureCreationAudit_, e.id);
        if (creation.result != 0) {
            ++counts["texture_creation_observations"];
            return;
        }
        if (objects_.contains(creation.resource))
            throw std::runtime_error("Texture/view identity already has replay storage");
        Com<IUnknown> created;
        if (isViewCreation(t)) {
            auto makeTyped = [&]<class View, class Desc>(auto method, const std::vector<uint32_t> *words) {
                Desc desc{};
                if (words)
                    std::memcpy(&desc, words->data(), sizeof(desc));
                Com<View> view;
                check((device_.Get()->*method)(get<ID3D11Resource>(creation.source), words ? &desc : nullptr,
                                               view.GetAddressOf()),
                      "Captured view creation");
                Desc actual{};
                view->GetDesc(&actual);
                std::vector<uint32_t> actualWords(sizeof(actual) / sizeof(uint32_t));
                std::memcpy(actualWords.data(), &actual, sizeof(actual));
                return std::pair<Com<IUnknown>, std::vector<uint32_t>>{view, std::move(actualWords)};
            };
            auto makeView = [&](const std::vector<uint32_t> *words) {
                switch (t) {
                case 0x357c:
                    return makeTyped
                        .template operator()<ID3D11ShaderResourceView, D3D11_SHADER_RESOURCE_VIEW_DESC>(
                            &ID3D11Device::CreateShaderResourceView, words);
                case 0x357d:
                    return makeTyped
                        .template operator()<ID3D11UnorderedAccessView, D3D11_UNORDERED_ACCESS_VIEW_DESC>(
                            &ID3D11Device::CreateUnorderedAccessView, words);
                case 0x357e:
                    return makeTyped
                        .template operator()<ID3D11RenderTargetView, D3D11_RENDER_TARGET_VIEW_DESC>(
                            &ID3D11Device::CreateRenderTargetView, words);
                case 0x357f:
                    return makeTyped
                        .template operator()<ID3D11DepthStencilView, D3D11_DEPTH_STENCIL_VIEW_DESC>(
                            &ID3D11Device::CreateDepthStencilView, words);
                default:
                    throw std::runtime_error("Unknown captured view creation");
                }
            };
            auto [view, actualWords] = makeView(creation.hasDescriptor ? &creation.descriptor : nullptr);
            auto readWords = [&](Bytes bytes) {
                Reader r(bytes);
                r.skip(24);
                std::vector<uint32_t> words;
                for (unsigned i = 0; i < viewCreationDescriptorWords(t); ++i)
                    words.push_back(r.read<uint32_t>());
                r.end();
                return words;
            };
            const auto kind = viewCreationResourceType(t);
            const auto savedWords = readWords(frame_.capturedPayload(creation.resource, 5, kind));
            if (!createdViewDescriptorEqual(t, savedWords, actualWords))
                throw std::runtime_error("Created view descriptor does not reproduce the saved view");
            const auto effectiveWords = readWords(frame_.payload(creation.resource, 5, kind));
            if (!createdViewDescriptorEqual(t, savedWords, effectiveWords))
                view = makeView(&effectiveWords).first;
            created = view;
        } else {
            const auto resource = frame_.resource(creation.resource);
            auto data = creation.data ? std::optional<Bytes>(frame_.data(creation.data)) : std::nullopt;
            if (auto it = options_.textures.find(creation.resource); it != options_.textures.end())
                data = Bytes(it->second);
            created = createEditTexture(resource, data);
            if (!creation.hasInitial)
                ++counts["textures_created_without_initial_data"];
        }
        if (renderdoc_) {
            Com<ID3D11DeviceChild> child;
            check(created.As(&child), "Captured object is a device child");
            const auto name = "GPA resource " + std::to_string(creation.resource) + " (captured creation)";
            check(child->SetPrivateData(WKPDID_D3DDebugObjectName, UINT(name.size()), name.data()),
                  "Name captured texture/view");
        }
        objects_.emplace(creation.resource, created);
        if (!isViewCreation(t) && hasResourceLodClamp(frame_.resource(creation.resource)))
            resourceLods_[creation.resource] = 0; // A successful native creation establishes the default.
        if (t == 0x357d && describeCounter(frame_, creation.resource)) {
            if (auto it = options_.initialUavCounters.find(creation.resource);
                it != options_.initialUavCounters.end())
                writeCounter(creation.resource, it->second);
            else
                undefinedCreatedCounters_.insert(creation.resource);
        }
        ++counts[commandName(t)];
        return;
    }
    if (t == 0x3017 || t == 0x3167) {
        readPrivateDataObservation(payload);
        ++counts["private_data_observations"];
        return;
    }
    if (t == 0x3257) {
        const auto present = validatePresentRecord(frame_, e.id);
        if (present.unbindRtv && objects_.contains(present.backbuffer)) {
            // Captured Texture2D storage represents the submitted single-frame image.
            // Preserve it for readback; later buffer rotation is rejected by the validator.
            auto backbuffer = get<ID3D11Resource>(present.backbuffer);
            auto isBackbuffer = [&](ID3D11View *view) {
                if (!view)
                    return false;
                Com<ID3D11Resource> resource;
                view->GetResource(&resource);
                return resource.Get() == backbuffer;
            };
            std::array<ID3D11RenderTargetView *, 8> targets{};
            std::array<Com<ID3D11RenderTargetView>, 8> owned;
            Com<ID3D11DepthStencilView> depth;
            context_->OMGetRenderTargets(8, targets.data(), &depth);
            bool changed = false;
            UINT count = 0;
            for (size_t slot = 0; slot < targets.size(); ++slot) {
                owned[slot].Attach(targets[slot]);
                if (targets[slot]) {
                    if (isBackbuffer(targets[slot])) {
                        targets[slot] = nullptr;
                        changed = true;
                    } else
                        count = UINT(slot + 1);
                }
            }
            std::array<ID3D11UnorderedAccessView *, 64> om{}, cs{};
            std::array<Com<ID3D11UnorderedAccessView>, 64> ownedOm, ownedCs;
            context_->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, uavLimit_, om.data());
            context_->CSGetUnorderedAccessViews(0, uavLimit_, cs.data());
            bool changedOm = false;
            for (UINT slot = 0; slot < uavLimit_; ++slot) {
                ownedOm[slot].Attach(om[slot]);
                ownedCs[slot].Attach(cs[slot]);
                if (isBackbuffer(om[slot])) {
                    om[slot] = nullptr;
                    changedOm = true;
                    ++counts["present_om_uav_unbinds"];
                }
                if (isBackbuffer(cs[slot])) {
                    ID3D11UnorderedAccessView *empty = nullptr;
                    context_->CSSetUnorderedAccessViews(slot, 1, &empty, nullptr);
                    ++counts["present_cs_uav_unbinds"];
                }
            }
            if (changedOm) {
                std::array<UINT, 64> initial;
                initial.fill(UINT_MAX);
                context_->OMSetRenderTargetsAndUnorderedAccessViews(count, targets.data(), depth.Get(), count,
                                                                    uavLimit_ - count, om.data() + count,
                                                                    initial.data() + count);
            }
            if (changed) {
                if (!changedOm)
                    context_->OMSetRenderTargetsAndUnorderedAccessViews(count, targets.data(), depth.Get(), 0,
                                                                        D3D11_KEEP_UNORDERED_ACCESS_VIEWS,
                                                                        nullptr, nullptr);
                ++counts["present_rtv_unbinds"];
            }
        }
        ++counts[present.test ? "present_tests" : "Present"];
        return;
    }
    if (acceptPassiveObjectRecord(t, payload)) {
        ++counts["object_observation_records"];
        return;
    }
    if (resourceLodAudit_ && !resourceLodAudit_->clamped.empty())
        for (auto resource : resourceLodAccesses(frame_, e, payload))
            requireResourceLod(resource);
    r.skip(16);
    if (t == 0x257) {
        const auto command = readClearView(payload);
        const auto target = validateClearView(frame_, command);
        D3D11_FEATURE_DATA_D3D11_OPTIONS support{};
        check(device_->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &support, sizeof support),
              "Check ClearView support");
        if (!context1_ || !support.ClearView)
            throw std::runtime_error("Replay device does not support ClearView");
        if (target.viewType == 0x8e) {
            D3D11_FEATURE_DATA_D3D11_OPTIONS1 depth{};
            check(device_->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS1, &depth, sizeof depth),
                  "Check depth ClearView support");
            if (!depth.ClearViewAlsoSupportsDepthOnlyFormats)
                throw std::runtime_error("Replay device does not support depth-only ClearView");
        }
        std::vector<D3D11_RECT> rects;
        for (const auto &rect : command.rectangles)
            rects.push_back({rect[0], rect[1], rect[2], rect[3]});
        const auto view = get<ID3D11View>(command.view);
        ReplayAnnotation marker(captureAnnotation_.Get(), e.id, "ClearView");
        context1_->ClearView(view, command.color.data(), command.hasRectangles ? rects.data() : nullptr,
                             command.count);
    } else if (t == 0x32 || t == 0x33 || t == 0x34) {
        auto view = r.read<Id>();
        if (!r.flag())
            throw std::runtime_error("Missing clear values");
        auto values = r.array<UINT, 4>();
        r.end();
        float floats[4];
        std::memcpy(floats, values.data(), 16);
        if (t == 0x32) {
            const auto target = get<ID3D11RenderTargetView>(view);
            ReplayAnnotation marker(captureAnnotation_.Get(), e.id, commandName(t));
            context_->ClearRenderTargetView(target, floats);
        } else {
            const auto target = get<ID3D11UnorderedAccessView>(view);
            ReplayAnnotation marker(captureAnnotation_.Get(), e.id, commandName(t));
            if (t == 0x33)
                context_->ClearUnorderedAccessViewUint(target, values.data());
            else
                context_->ClearUnorderedAccessViewFloat(target, floats);
        }
    } else if (t == 0x31) {
        auto view = r.read<Id>();
        auto flags = r.read<UINT>();
        auto depth = r.read<float>();
        auto stencil = r.read<uint8_t>();
        r.end();
        const auto target = get<ID3D11DepthStencilView>(view);
        ReplayAnnotation marker(captureAnnotation_.Get(), e.id, commandName(t));
        context_->ClearDepthStencilView(target, flags, depth, stencil);
    } else if (t == 0x3e) {
        auto dst = r.read<Id>(), src = r.read<Id>();
        r.end();
        const auto destination = get<ID3D11Resource>(dst), source = get<ID3D11Resource>(src);
        ReplayAnnotation marker(captureAnnotation_.Get(), e.id, commandName(t));
        context_->CopyResource(destination, source);
    } else if (t == 0x3f) {
        auto dst = r.read<Id>();
        auto offset = r.read<UINT>();
        auto src = r.read<Id>();
        r.end();
        const auto destination = get<ID3D11Buffer>(dst);
        const auto source = get<ID3D11UnorderedAccessView>(src);
        requireCreatedCounter(src);
        ReplayAnnotation marker(captureAnnotation_.Get(), e.id, commandName(t));
        context_->CopyStructureCount(destination, offset, source);
    } else if (t == 0x40 || t == 0x256) {
        auto dst = r.read<Id>();
        auto sub = r.read<UINT>(), x = r.read<UINT>(), y = r.read<UINT>(), z = r.read<UINT>();
        auto src = r.read<Id>();
        auto srcSub = r.read<UINT>();
        bool has = r.flag();
        D3D11_BOX box{};
        if (has)
            box = r.read<D3D11_BOX>();
        const auto flags = t == 0x256 ? r.read<UINT>() : 0;
        r.end();
        const auto destination = get<ID3D11Resource>(dst), source = get<ID3D11Resource>(src);
        ReplayAnnotation marker(captureAnnotation_.Get(), e.id, commandName(t));
        if (has && (box.left >= box.right || box.top >= box.bottom || box.front >= box.back))
            ++counts["empty_copy_regions"];
        else if (t == 0x256) {
            if (!context1_)
                throw std::runtime_error("Replay device does not support Context1 transfers");
            if (dst == src && sub == srcSub) {
                D3D11_FEATURE_DATA_D3D11_OPTIONS support{};
                check(device_->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &support, sizeof support),
                      "Check overlapping copy support");
                if (!support.CopyWithOverlap)
                    throw std::runtime_error("Replay device does not support same-subresource copying");
            }
            context1_->CopySubresourceRegion1(destination, sub, x, y, z, source, srcSub, has ? &box : nullptr,
                                              flags);
        } else
            context_->CopySubresourceRegion(destination, sub, x, y, z, source, srcSub, has ? &box : nullptr);
    } else if (t == 0x42) {
        auto dst = r.read<Id>();
        auto sub = r.read<UINT>();
        auto src = r.read<Id>();
        auto srcSub = r.read<UINT>(), format = r.read<UINT>();
        r.end();
        const auto destination = get<ID3D11Resource>(dst), source = get<ID3D11Resource>(src);
        ReplayAnnotation marker(captureAnnotation_.Get(), e.id, commandName(t));
        UINT support = 0;
        check(device_->CheckFormatSupport(DXGI_FORMAT(format), &support), "Check resolve format support");
        if (!(support & D3D11_FORMAT_SUPPORT_MULTISAMPLE_RESOLVE))
            throw std::runtime_error("Resolve format is unsupported by the replay device");
        context_->ResolveSubresource(destination, sub, source, srcSub, DXGI_FORMAT(format));
    } else if (t == 0x245) {
        auto id = r.read<Id>();
        r.end();
        const auto view = get<ID3D11ShaderResourceView>(id);
        ReplayAnnotation marker(captureAnnotation_.Get(), e.id, commandName(t));
        context_->GenerateMips(view);
    } else if (t == 0x247 || t == 0x255) {
        auto layout = updateSourceLayout(frame_, e.id);
        const auto destination = frame_.resource(layout.destination);
        const auto format = destination.type == 0x83 ? 0 : textureInfo(destination).format;
        const bool explicitSource = options_.updateSources.contains(e.id);
        if ((format == 104 || format == 105) && !explicitSource)
            throw std::runtime_error("Legacy GPA P010/P016 Update chroma reads beyond saved GenData; "
                                     "use an explicit complete Update source replacement");
        D3D11_BOX box{};
        if (layout.hasBox)
            std::memcpy(&box, layout.box.data(), sizeof box);
        Bytes data;
        if (auto it = options_.updateSources.find(e.id); it != options_.updateSources.end())
            data = it->second;
        else
            data = frame_.data(layout.data);
        if (!layout.empty && data.size() != layout.size)
            throw std::runtime_error("Packed UpdateSubresource size mismatch");
        const auto destinationObject = get<ID3D11Resource>(layout.destination);
        ReplayAnnotation marker(captureAnnotation_.Get(), e.id, commandName(t));
        if (layout.empty)
            ++counts["empty_update_regions"];
        else if (t == 0x255) {
            if (!context1_)
                throw std::runtime_error("Replay device does not support Context1 transfers");
            if (layout.hasBox && destination.type == 0x83 && (destination.desc.at(2) & 4)) {
                D3D11_FEATURE_DATA_D3D11_OPTIONS support{};
                check(device_->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &support, sizeof support),
                      "Check partial constant-buffer update support");
                if (!support.ConstantBufferPartialUpdate)
                    throw std::runtime_error(
                        "Replay device does not support partial constant-buffer updates");
            }
            context1_->UpdateSubresource1(destinationObject, layout.subresource,
                                          layout.hasBox ? &box : nullptr, data.data(), layout.rowPitch,
                                          layout.slicePitch, layout.flags);
        } else
            context_->UpdateSubresource(destinationObject, layout.subresource, layout.hasBox ? &box : nullptr,
                                        data.data(), layout.rowPitch, layout.slicePitch);
        if (format >= 103 && format <= 105) {
            PlanarWrite write;
            write.event = e.id;
            write.resource = layout.destination;
            write.subresource = layout.subresource;
            write.format = format;
            write.explicitSource = explicitSource;
            write.rowPitch = layout.rowPitch;
            write.slicePitch = layout.slicePitch;
            if (layout.hasBox)
                write.box = layout.box;
            planarWrites_.push_back(write);
        }
    } else if (t == 0x242) {
        r.end();
        Reader owner(payload);
        owner.skip(8);
        immediate(owner.read<Id>());
        context_->ClearState();
        boundPredicate_ = 0;
        predicateValue_ = 0;
        predicateBinding_.reset();
        samplerBindings_.clear();
        activePipelineBindings_.clear();
        srvBindings_.clear();
        iaBindings_.clear();
        constantBufferBindings_.clear();
        resetStreamOutputBindings();
        clearBindingGaps();
        for (auto &ranges : ranges_)
            ranges.clear();
    } else if (t == 0x244) {
        r.end();
        Reader owner(payload);
        owner.skip(8);
        immediate(owner.read<Id>());
        context_->Flush();
    } else {
        throw std::runtime_error("Command migration pending: " + commandName(t));
    }
    counts[commandName(t)]++;
    if (isWritableCommand(t))
        lastWorkEvent_ = e.id;
}
void Replay::run(const std::function<void(Id, size_t, size_t)> &progress,
                 const ReplayBoundaryObserver &observer, const ReplayBoundaryObserver &commandObserver,
                 const ReplayCommandScope &commandScope) {
    boundaryObserver_ = observer;
    struct ResetObserver {
        ReplayBoundaryObserver &value;
        ~ResetObserver() { value = {}; }
    } reset{boundaryObserver_};
    replayComplete_ = false;
    allowUnusedSoLifetime_ = !commandObserver && !commandScope;
    allowUnusedCbLifetime_ = !commandObserver && !commandScope;
    unusedCbLifetimes_.clear();
    unusedSoLifetimes_.clear();
    measurementResult_.reset();
    if (options_.measurement) {
        const auto &m = *options_.measurement;
        for (auto id : {m.start, m.end})
            if (!id || !frame_.entries().contains(id) || frame_.entry(id).category != 7)
                throw std::runtime_error("Statistics range endpoints must be captured API command IDs");
        if (m.start > m.end)
            throw std::runtime_error("Statistics range start must not exceed end");
        if (m.singleEvent && (m.start != m.end || !isDraw(frame_.entry(m.start).type)))
            throw std::runtime_error("Statistics requires a Draw or Dispatch event");
        if (options_.before || options_.until != m.end || options_.timings)
            throw std::runtime_error("Statistics requires its own complete replay boundary");
    }
    ++generation_;
    if (options_.until && frame_.entry(options_.until).category != 7)
        throw std::runtime_error("Stop event is not an API command");
    context_->ClearState();
    resetPredicates();
    predicateBinding_.reset();
    samplerBindings_ = SamplerBindings{};
    activePipelineBindings_.clear();
    srvBindings_.clear();
    srvHistories_.clear();
    iaBindings_.clear();
    iaHistories_.clear();
    constantBufferBindings_.clear();
    constantBufferHistory_.reset();
    outputHistory_ = makeOutputHistory(frame_, options_);
    retainedSo_.fill(false);
    bindingEvent_ = 0;
    clearBindingGaps();
    objects_.clear();
    if (!resourceLodAudit_)
        resourceLodAudit_ = auditResourceLod(frame_);
    resourceLods_.clear();
    resourceLodShaderSrvs_.clear();
    for (const auto &[resource, initial] : resourceLodAudit_->initial)
        resourceLods_[resource] = initial.value;
    unissuedPredicates_.clear();
    baselinePredicates_.clear();
    conditionPredicates_.clear();
    undefinedCreatedCounters_.clear();
    missingInitialCounters_.clear();
    ignoredMsaaInitial_.clear();
    planarWrites_.clear();
    appliedExperimentEvents_.clear();
    editedSamplers_.clear();
    rasterizerExtensions_.clear();
    logicBlendStates_.clear();
    usedSrvs_.clear();
    interfaceSlots_.clear();
    passthroughShaders_.clear();
    resetStreamOutputBindings();
    soVertexCounts_.clear();
    soByteCursors_.clear();
    soQueries_.clear();
    soAutoResults_.clear();
    streamOutputHistory.clear();
    soCountRequiresKnown_ = options_.editedEvents || !options_.shaders.empty() ||
                            !options_.textures.empty() || !options_.disabled.empty() ||
                            !options_.buffers.empty() || !options_.commandPayloads.empty() ||
                            !options_.updateSources.empty() || !options_.uavCounters.empty() ||
                            !options_.predicateSetters.empty() || !options_.depthStencilEdits.empty() ||
                            !options_.rasterizerEdits.empty() || !options_.blendEdits.empty() ||
                            !options_.pipelineSetters.empty() || !options_.samplerSetters.empty() ||
                            !options_.samplerEdits.empty() || !options_.srvEdits.empty() ||
                            !options_.srvSetters.empty() || !options_.outputSetters.empty() ||
                            !options_.iaSetters.empty() || !options_.constantBufferSetters.empty();
    soCountEnabled_ = false;
    for (const auto &[id, entry] : frame_.entries())
        if ((entry.category == 7 && entry.type == 0x38) ||
            (entry.category == 5 && entry.type == 0x91 && shaderStreamOutput(frame_, id)))
            soCountEnabled_ = true;
    counts.clear();
    discardHistory_.clear();
    for (auto &ranges : ranges_)
        ranges.clear();
    lastTarget_ = lastTargetView_ = lastEvent_ = lastWorkEvent_ = 0;
    timestamps_.clear();
    timings.clear();
    statistics = {};
    Com<ID3D11Query> disjoint, stats;
    if (options_.timings) {
        D3D11_QUERY_DESC desc{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
        check(device_->CreateQuery(&desc, &disjoint), "Create disjoint query");
        desc.Query = D3D11_QUERY_PIPELINE_STATISTICS;
        check(device_->CreateQuery(&desc, &stats), "Create statistics query");
        context_->Begin(disjoint.Get());
        context_->Begin(stats.Get());
    }
    std::unique_ptr<NativeSample> sample;
    if (options_.measurement)
        sample = std::make_unique<NativeSample>(device_.Get(), context_.Get());
    activeSample_ = sample.get();
    struct ResetSample {
        NativeSample *&active;
        ~ResetSample() { active = nullptr; }
    } resetSample{activeSample_};
    std::map<std::string, uint64_t> initialCounts;
    size_t total = 0, done = 0;
    for (auto &[id, e] : frame_.entries())
        if (e.category == 7)
            ++total;
    for (auto &[id, e] : frame_.entries())
        if (e.category == 7) {
            if (options_.until &&
                (id > options_.until ||
                 (options_.before && id == options_.until &&
                  (!isDraw(e.type) || (!options_.prepareBeforeDraw && !boundaryObserver_)))))
                break;
            try {
                lastEvent_ = id;
                if (sample && !options_.measurement->singleEvent && id == options_.measurement->start) {
                    initialCounts = counts;
                    sample->begin();
                }
                const auto execute = [&] {
                    if (commandObserver)
                        commandObserver(id, false, context_.Get(), objects_);
                    command(e);
                    if (commandObserver)
                        commandObserver(id, true, context_.Get(), objects_);
                };
                if (commandScope)
                    commandScope(id, execute);
                else
                    execute();
                if (sample && !options_.measurement->singleEvent && id == options_.measurement->end)
                    sample->end();
            } catch (const std::exception &error) {
                throw std::runtime_error("Event " + std::to_string(id) + " (" + commandName(e.type) +
                                         "): " + error.what());
            }
            if (progress && ((++done % 128) == 0 || isDraw(e.type)))
                progress(id, done, total);
        }
    std::optional<NativeStatistics> measured;
    if (sample) {
        auto result = sample->result();
        result.predicate = measuredPredicate_;
        for (const auto &[name, count] : counts)
            if (count != initialCounts[name])
                result.replayCounts[name] = count - initialCounts[name];
        measured = std::move(result);
    }
    // The reference recovers missing setters from a before-draw snapshot without
    // executing that draw's experiments, recording it as work, or notifying observers.
    const bool gaps = !pipelineGaps_.empty() || outputGap_ || layoutGap_ ||
                      std::any_of(srvGaps_.begin(), srvGaps_.end(), [](const auto &stage) {
                          return std::any_of(stage.begin(), stage.end(), [](Id id) { return id != 0; });
                      });
    if (gaps && options_.before && options_.until && isDraw(frame_.entry(options_.until).type)) {
        const auto &entry = frame_.entry(options_.until);
        bind(prepareState(frame_.event(options_.until)), entry.type == 0x35 || entry.type == 0x36);
        clearBindingGaps();
    }
    if (options_.timings) {
        context_->End(stats.Get());
        context_->End(disjoint.Get());
    }
    if (options_.until)
        requireResolvedBindings();
    context_->Flush();
    check(device_->GetDeviceRemovedReason(), "Replay device status");
    if (options_.timings) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        auto wait = [&](ID3D11Query *q, void *data, UINT bytes) {
            for (;;) {
                auto hr = context_->GetData(q, data, bytes, 0);
                check(hr, "Read GPU query");
                if (hr == S_OK)
                    return;
                if (std::chrono::steady_clock::now() > deadline)
                    throw std::runtime_error("GPU query timeout");
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        };
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT frequency{};
        wait(disjoint.Get(), &frequency, sizeof frequency);
        wait(stats.Get(), &statistics, sizeof statistics);
        if (frequency.Disjoint || !frequency.Frequency)
            throw std::runtime_error("GPU timestamp frequency changed during collection");
        for (auto &timestamp : timestamps_) {
            UINT64 begin = 0, end = 0;
            wait(timestamp.begin.Get(), &begin, sizeof begin);
            wait(timestamp.end.Get(), &end, sizeof end);
            if (end < begin)
                throw std::runtime_error("Non-monotonic GPU timestamp");
            timings.push_back({timestamp.event, double(end - begin) * 1e6 / double(frequency.Frequency)});
        }
    }
    if (options_.debug) {
        Com<ID3D11InfoQueue> queue;
        if (SUCCEEDED(device_.As(&queue)))
            for (UINT64 i = 0; i < queue->GetNumStoredMessages(); ++i) {
                SIZE_T size = 0;
                queue->GetMessage(i, nullptr, &size);
                std::vector<uint8_t> bytes(size);
                auto msg = reinterpret_cast<D3D11_MESSAGE *>(bytes.data());
                if (SUCCEEDED(queue->GetMessage(i, msg, &size)))
                    std::cerr << msg->pDescription << '\n';
            }
    }
    measurementResult_ = std::move(measured);
    replayComplete_ = true;
}
Image Replay::output(Id texture, UINT sub) {
    Unpredicated guard(context_.Get());
    if (!texture && (!options_.until || !lastTarget_)) {
        std::vector<Resource> markers, live;
        for (auto &[id, e] : frame_.entries())
            if (e.category == 5 && (e.type == 0x85 || e.type == 0x87)) {
                auto res = frame_.resource(id);
                auto it = frame_.entries().find(res.device);
                if (it != frame_.entries().end() && it->second.category == 5 && it->second.type == 0x38)
                    (e.type == 0x87 ? markers : live).push_back(res);
            }
        if (markers.size() == 1) {
            std::vector<Id> matches;
            for (auto &res : live)
                if (res.device == markers[0].device &&
                    std::equal(res.desc.begin(), res.desc.begin() + 7, markers[0].desc.begin()))
                    matches.push_back(res.id);
            if (matches.size() == 1 && (objects_.contains(matches[0]) || frame_.resource(matches[0]).data ||
                                        options_.textures.contains(matches[0])))
                texture = matches[0];
        }
    }
    if (!texture)
        texture = lastTarget_;
    if (!texture)
        throw std::runtime_error("No replay output target");
    auto res = frame_.resource(texture);
    auto info = textureInfo(res);
    if (info.dimension != 3 || info.samples != 1 || sub >= info.mips * info.layers)
        throw std::runtime_error("Output conversion migration pending for this texture");
    if (info.format != 27 && info.format != 28 && info.format != 29 && info.format != 87 &&
        info.format != 90 && info.format != 91)
        throw std::runtime_error("Output format conversion migration pending: " +
                                 std::to_string(info.format));
    auto obj = get<ID3D11Texture2D>(texture);
    D3D11_TEXTURE2D_DESC desc{};
    obj->GetDesc(&desc);
    auto mip = sub % info.mips;
    desc.Width = std::max(1u, desc.Width >> mip);
    desc.Height = std::max(1u, desc.Height >> mip);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    Com<ID3D11Texture2D> staging;
    check(device_->CreateTexture2D(&desc, nullptr, &staging), "Create output staging texture");
    context_->OMSetRenderTargets(0, nullptr, nullptr);
    context_->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, obj, sub, nullptr);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    check(context_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Read output");
    Image out{desc.Width, desc.Height, info.format, {}, texture};
    try {
        out.rgba.resize(size_t(desc.Width) * desc.Height * 4);
        for (UINT y = 0; y < desc.Height; ++y)
            std::memcpy(out.rgba.data() + size_t(y) * desc.Width * 4,
                        static_cast<uint8_t *>(mapped.pData) + size_t(y) * mapped.RowPitch,
                        size_t(desc.Width) * 4);
        if (info.format == 87 || info.format == 90 || info.format == 91)
            for (size_t i = 0; i < out.rgba.size(); i += 4)
                std::swap(out.rgba[i], out.rgba[i + 2]);
    } catch (...) {
        context_->Unmap(staging.Get(), 0);
        throw;
    }
    context_->Unmap(staging.Get(), 0);
    return out;
}
std::vector<uint8_t> Replay::readBuffer(Id id) {
    Unpredicated guard(context_.Get());
    if (frame_.resource(id).type != 0x83)
        throw std::runtime_error("Readback requires a buffer resource");
    auto source = get<ID3D11Buffer>(id);
    D3D11_BUFFER_DESC desc{};
    source->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    desc.StructureByteStride = 0;
    Com<ID3D11Buffer> staging;
    check(device_->CreateBuffer(&desc, nullptr, &staging), "Create buffer staging");
    context_->CopyResource(staging.Get(), source);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    check(context_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Read buffer");
    std::vector<uint8_t> data;
    try {
        auto begin = static_cast<uint8_t *>(mapped.pData);
        data.assign(begin, begin + desc.ByteWidth);
    } catch (...) {
        context_->Unmap(staging.Get(), 0);
        throw;
    }
    context_->Unmap(staging.Get(), 0);
    return data;
}
std::string disassemble(Bytes data) {
    auto parts = readDxbcParts(data);
    if (std::none_of(parts.begin(), parts.end(),
                     [](const auto &part) { return part.first == 0x52444853 || part.first == 0x58454853; }))
        return "// Signature only; no executable shader instructions.\n";
    Com<ID3DBlob> blob;
    check(D3DDisassemble(data.data(), data.size(), D3D_DISASM_ENABLE_INSTRUCTION_NUMBERING, nullptr, &blob),
          "Disassemble DXBC");
    return {static_cast<const char *>(blob->GetBufferPointer()), blob->GetBufferSize()};
}
} // namespace flora
