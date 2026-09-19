#include "Replay.h"
#include "core/Commands.h"
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
Replay::Replay(const Frame &frame, ReplayOptions options) : frame_(frame), options_(std::move(options)) {
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL level{};
    check(D3D11CreateDevice(nullptr, options_.warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE, nullptr,
                            options_.debug ? D3D11_CREATE_DEVICE_DEBUG : 0, levels, 2, D3D11_SDK_VERSION,
                            &device_, &level, &context_),
          "Create DX11 device");
    check(context_.As(&context1_), "Query DX11.1 context");
    uavLimit_ = level >= D3D_FEATURE_LEVEL_11_1 ? 64 : 8;
}
Replay::~Replay() {
    if (context_) {
        context_->ClearState();
        context_->Flush();
    }
}
std::string Replay::adapter() const {
    Com<IDXGIDevice> dxgi;
    Com<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC desc{};
    check(device_.As(&dxgi), "Query DXGI device");
    check(dxgi->GetAdapter(&adapter), "Get adapter");
    check(adapter->GetDesc(&desc), "Get adapter description");
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
            if (info.format == 104 || info.format == 105)
                throw std::runtime_error("Legacy planar initial layout migration pending");
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
            if (auto it = options_.textures.find(id); it != options_.textures.end())
                data = it->second;
            if (uint64_t(info.mips) * info.layers > 30720)
                throw std::runtime_error("Too many texture subresources");
            std::vector<D3D11_SUBRESOURCE_DATA> initial;
            size_t offset = 0;
            if (!data.empty())
                for (uint32_t layer = 0; layer < info.layers; ++layer)
                    for (uint32_t mip = 0; mip < info.mips; ++mip) {
                        auto [pitch, rows] = pitches(std::max(1u, info.width >> mip),
                                                     std::max(1u, info.height >> mip), info.format);
                        uint64_t slice = uint64_t(pitch) * rows,
                                 size = slice * std::max(1u, info.depth >> mip);
                        if (slice > UINT32_MAX || offset > data.size() || size > data.size() - offset)
                            throw std::runtime_error("Texture initial size mismatch");
                        initial.push_back({data.data() + offset, pitch, UINT(slice)});
                        offset += size;
                    }
            if (!data.empty() && offset != data.size())
                throw std::runtime_error("Texture initial trailing bytes");
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
                result = obj;
            }
        } else if (t >= 0x90 && t <= 0x95) {
            auto data = frame_.shader(resource.data);
            if (auto it = options_.shaders.find(id); it != options_.shaders.end())
                data = it->second;
            Reader head(p);
            head.skip(32);
            auto linkage = head.read<Id>();
            auto so = head.read<Id>();
            if (linkage || (t == 0x91 && so))
                throw std::runtime_error("Class linkage / stream-output shader migration pending");
            Com<ID3D11ShaderReflection> reflection;
            check(D3DReflect(data.data(), data.size(), IID_ID3D11ShaderReflection, &reflection),
                  "Reflect shader");
            D3D11_SHADER_DESC sd{};
            check(reflection->GetDesc(&sd), "Shader descriptor");
            std::array<bool, 128> used{};
            // Stripped SM4 shaders can reflect no bindings while still sampling SRVs.
            // Only an actual RDEF chunk proves that undeclared slots are unused.
            Reader dxbc(data);
            dxbc.skip(20);
            if (dxbc.read<UINT>() != 1 || dxbc.read<UINT>() != data.size())
                throw std::runtime_error("DXBC container bounds");
            auto chunks = dxbc.read<UINT>();
            if (chunks > 256)
                throw std::runtime_error("DXBC chunk limit");
            bool hasRdef = false;
            for (UINT i = 0; i < chunks; ++i) {
                auto offset = dxbc.read<UINT>();
                if (offset < 32 + chunks * 4 || offset > data.size() || data.size() - offset < 8)
                    throw std::runtime_error("DXBC chunk offset");
                Reader chunk(data.subspan(offset));
                auto tag = chunk.read<UINT>(), size = chunk.read<UINT>();
                chunk.take(size);
                hasRdef |= tag == 0x46454452;
            }
            if (!hasRdef)
                used.fill(true);
            for (UINT i = 0; i < sd.BoundResources; ++i) {
                D3D11_SHADER_INPUT_BIND_DESC b{};
                check(reflection->GetResourceBindingDesc(i, &b), "Shader binding");
                if (b.Type == D3D_SIT_TBUFFER || b.Type == D3D_SIT_TEXTURE || b.Type == D3D_SIT_STRUCTURED ||
                    b.Type == D3D_SIT_BYTEADDRESS) {
                    if (b.BindPoint > 128 || b.BindCount > 128 - b.BindPoint)
                        throw std::runtime_error("SRV range overflow");
                    for (UINT j = 0; j < b.BindCount; ++j)
                        used[b.BindPoint + j] = true;
                }
            }
            usedSrvs_[id] = used;
#define CREATE_SHADER(Type, Method)                                                                          \
    {                                                                                                        \
        Com<Type> obj;                                                                                       \
        check(device_->Method(data.data(), data.size(), nullptr, &obj), #Method);                            \
        result = obj;                                                                                        \
    }
            switch (t) {
            case 0x90:
                CREATE_SHADER(ID3D11VertexShader, CreateVertexShader);
                break;
            case 0x91:
                CREATE_SHADER(ID3D11GeometryShader, CreateGeometryShader);
                break;
            case 0x92:
                CREATE_SHADER(ID3D11PixelShader, CreatePixelShader);
                break;
            case 0x93:
                CREATE_SHADER(ID3D11ComputeShader, CreateComputeShader);
                break;
            case 0x94:
                CREATE_SHADER(ID3D11DomainShader, CreateDomainShader);
                break;
            case 0x95:
                CREATE_SHADER(ID3D11HullShader, CreateHullShader);
                break;
            }
#undef CREATE_SHADER
        } else if (t == 0x82) {
            auto dataId = r.read<Id>();
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
            if (!desc.ForcedSampleCount &&
                desc.ConservativeRaster == D3D11_CONSERVATIVE_RASTERIZATION_MODE_OFF) {
                D3D11_RASTERIZER_DESC basic{};
                std::memcpy(&basic, &desc, sizeof basic);
                Com<ID3D11RasterizerState> obj;
                check(device_->CreateRasterizerState(&basic, &obj), "CreateRasterizerState");
                result = obj;
            } else {
                Com<ID3D11Device3> dev;
                check(device_.As(&dev), "Query Device3");
                Com<ID3D11RasterizerState2> obj;
                check(dev->CreateRasterizerState2(&desc, &obj), "CreateRasterizerState2");
                result = obj;
            }
        } else if (t == 0x8a) {
            auto desc = descriptor<D3D11_BLEND_DESC>(r.take(r.remaining()));
            Com<ID3D11BlendState> obj;
            check(device_->CreateBlendState(&desc, &obj), "CreateBlendState");
            result = obj;
        } else if (t == 0x10d) {
            auto desc = descriptor<D3D11_BLEND_DESC1>(r.take(r.remaining()));
            bool logic = false;
            for (UINT i = 0; i < (desc.IndependentBlendEnable ? 8u : 1u); ++i)
                logic |= desc.RenderTarget[i].LogicOpEnable != FALSE;
            if (!logic) {
                D3D11_BLEND_DESC basic{};
                basic.AlphaToCoverageEnable = desc.AlphaToCoverageEnable;
                basic.IndependentBlendEnable = desc.IndependentBlendEnable;
                for (UINT i = 0; i < 8; ++i) {
                    auto &a = desc.RenderTarget[i];
                    basic.RenderTarget[i] = {
                        a.BlendEnable,   a.SrcBlend,       a.DestBlend,    a.BlendOp,
                        a.SrcBlendAlpha, a.DestBlendAlpha, a.BlendOpAlpha, a.RenderTargetWriteMask};
                }
                Com<ID3D11BlendState> obj;
                check(device_->CreateBlendState(&basic, &obj), "CreateBlendState");
                result = obj;
            } else {
                Com<ID3D11Device1> dev;
                check(device_.As(&dev), "Query Device1");
                Com<ID3D11BlendState1> obj;
                check(dev->CreateBlendState1(&desc, &obj), "CreateBlendState1");
                result = obj;
            }
        } else
            throw std::runtime_error("Resource migration pending: " + resourceName(t) + " type " +
                                     std::to_string(t));
    } catch (const std::exception &e) {
        throw std::runtime_error("Resource " + std::to_string(id) + ": " + e.what());
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
    if (options_.debug)
        std::cerr << "state " << s.ib << " " << s.layout << " " << s.topology << " vp " << s.viewports
                  << " sc " << s.scissors << " mask " << s.sampleMask << " shader " << s.stages[0].shader
                  << " " << s.stages[4].shader << " cb " << s.stages[0].cb[0] << " " << s.stages[4].cb[0]
                  << " vb " << s.vb[0] << " " << s.strides[0] << " " << s.offsets[0] << "\n";
    std::array<ID3D11UnorderedAccessView *, 64> empty{};
    context_->OMSetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, uavLimit_, empty.data(),
                                                        nullptr);
    context_->CSSetUnorderedAccessViews(0, uavLimit_, empty.data(), nullptr);
    context_->SOSetTargets(0, nullptr, nullptr);
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
        if (x.classCount)
            throw std::runtime_error("Dynamic shader class migration pending");
        auto shader = object(x.shader);
        switch (stage) {
        case 0:
            context_->VSSetShader(static_cast<ID3D11VertexShader *>(shader), nullptr, 0);
            break;
        case 1:
            context_->HSSetShader(static_cast<ID3D11HullShader *>(shader), nullptr, 0);
            break;
        case 2:
            context_->DSSetShader(static_cast<ID3D11DomainShader *>(shader), nullptr, 0);
            break;
        case 3:
            context_->GSSetShader(static_cast<ID3D11GeometryShader *>(shader), nullptr, 0);
            break;
        case 4:
            context_->PSSetShader(static_cast<ID3D11PixelShader *>(shader), nullptr, 0);
            break;
        case 5:
            context_->CSSetShader(static_cast<ID3D11ComputeShader *>(shader), nullptr, 0);
            break;
        }
        std::array<ID3D11Buffer *, 14> cb{};
        std::array<ID3D11SamplerState *, 16> sam{};
        std::array<ID3D11ShaderResourceView *, 128> srv{};
        for (size_t i = 0; i < cb.size(); ++i)
            cb[i] = get<ID3D11Buffer>(x.cb[i]);
        for (size_t i = 0; i < sam.size(); ++i)
            sam[i] = get<ID3D11SamplerState>(x.samplers[i]);
        for (size_t i = 0; i < srv.size(); ++i)
            if (shader && usedSrvs_.at(x.shader)[i])
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
    if (s.viewports) {
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
    if (s.scissors) {
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
    if (s.predicate || s.soCount)
        throw std::runtime_error("Predication / stream-output replay migration pending");
    context_->SetPredication(nullptr, FALSE);
    bool extended = std::any_of(s.omExtended.begin(), s.omExtended.end(), [](Id x) { return x != 0; }) ||
                    std::any_of(s.csExtended.begin(), s.csExtended.end(), [](Id x) { return x != 0; });
    if (extended)
        throw std::runtime_error("Extended UAV snapshot migration pending");
    if (compute) {
        std::array<ID3D11UnorderedAccessView *, 8> uavs{};
        for (int i = 0; i < 8; ++i)
            uavs[i] = get<ID3D11UnorderedAccessView>(s.csUav[i]);
        context_->CSSetUnorderedAccessViews(0, 8, uavs.data(), nullptr);
        return;
    }
    if (s.rtCount > 8 || s.omStart > 64)
        throw std::runtime_error("Output snapshot limit");
    auto rtCount = std::min({s.rtCount, s.omStart, 8u});
    std::array<ID3D11RenderTargetView *, 8> rt{};
    for (UINT i = 0; i < rtCount; ++i)
        rt[i] = get<ID3D11RenderTargetView>(s.rtv[i]);
    auto depth = get<ID3D11DepthStencilView>(s.dsv);
    if (s.omStart < s.rtCount) {
        std::array<ID3D11UnorderedAccessView *, 8> uav{};
        for (UINT i = s.omStart; i < s.rtCount; ++i)
            uav[i - s.omStart] = get<ID3D11UnorderedAccessView>(s.rtv[i]);
        context_->OMSetRenderTargetsAndUnorderedAccessViews(rtCount, rt.data(), depth, s.omStart,
                                                            s.rtCount - s.omStart, uav.data(), nullptr);
    } else
        context_->OMSetRenderTargets(rtCount, rt.data(), depth);
    if (rtCount && s.rtv[0]) {
        Reader r(frame_.payload(s.rtv[0]));
        r.skip(16);
        lastTarget_ = r.read<Id>();
    }
}
void Replay::constantBuffers(const Entry &e) {
    Reader r(frame_.payload(e.id));
    r.skip(8);
    immediate(r.read<Id>());
    auto start = r.read<UINT>(), count = r.read<UINT>();
    if (start >= 14 || count > 14 - start)
        throw std::runtime_error("CB slot limit");
    auto ids = optional<Id>(r, count, 14);
    if (ids.size() != count)
        throw std::runtime_error("Missing CB array");
    bool window = e.type >= 0x24f && e.type <= 0x254;
    std::vector<UINT> first, sizes;
    if (window) {
        first = optional<UINT>(r, count, 14);
        sizes = optional<UINT>(r, count, 14);
        if (first.size() != sizes.size())
            throw std::runtime_error("CB1 window pair mismatch");
    }
    r.end();
    int stage = 0;
    if (e.type >= 0x249 && e.type <= 0x254)
        stage = (e.type - 0x249) % 6;
    else
        switch (e.type - 0x34de) {
        case 7:
            stage = 0;
            break;
        case 62:
            stage = 1;
            break;
        case 66:
            stage = 2;
            break;
        case 22:
            stage = 3;
            break;
        case 16:
            stage = 4;
            break;
        case 71:
            stage = 5;
            break;
        }
    for (UINT i = 0; i < count; ++i) {
        Range x{ids[i], first.empty() ? 0 : first[i], sizes.empty() ? 0 : sizes[i], !first.empty()};
        setRange(stage, start + i, get<ID3D11Buffer>(ids[i]), x);
        if (window)
            ranges_[stage][start + i] = x;
        else
            ranges_[stage].erase(start + i);
    }
}
void Replay::outputs(const Entry &e) {
    Reader r(frame_.payload(e.id));
    r.skip(8);
    immediate(r.read<Id>());
    UINT rtCount = 0, start = 0, count = 0;
    Id dsv = 0;
    std::vector<Id> rtvs, uavs;
    std::vector<UINT> initial;
    bool om = e.type == 0x34ff || e.type == 0x3500;
    if (om) {
        rtCount = r.read<UINT>();
        rtvs = optional<Id>(r, rtCount, 8);
        dsv = r.read<Id>();
        if (rtCount != UINT_MAX && rtvs.size() != rtCount)
            throw std::runtime_error("Missing RTV array");
    }
    if (e.type != 0x34ff) {
        start = r.read<UINT>();
        count = r.read<UINT>();
        uavs = optional<Id>(r, count);
        initial = optional<UINT>(r, count);
        if (count != UINT_MAX && (start > uavLimit_ || count > uavLimit_ - start || uavs.size() != count))
            throw std::runtime_error("UAV range invalid");
    }
    r.end();
    // Captures may omit unused placeholder views. A later full snapshot resolves them.
    bool missing = false;
    for (auto id : rtvs)
        if (id && !frame_.entries().contains(id))
            missing = true;
    for (auto id : uavs)
        if (id && !frame_.entries().contains(id))
            missing = true;
    if (dsv && !frame_.entries().contains(dsv))
        missing = true;
    if (missing) {
        counts["unresolved_output_setters"]++;
        return;
    }
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
}
void Replay::mappedWrites(const Entry &e) {
    Reader r(frame_.payload(e.id));
    r.skip(8);
    immediate(r.read<Id>());
    auto hr = r.read<int32_t>();
    auto id = r.read<Id>();
    auto sub = r.read<UINT>(), kind = r.read<UINT>(), flags = r.read<UINT>();
    auto dataId = r.read<Id>();
    r.end();
    if (hr < 0 || !dataId)
        return;
    if (kind < 2 || kind > 5)
        throw std::runtime_error("Invalid writable Map type");
    auto desc = frame_.resource(id);
    uint32_t row = 0, rows = 0, depth = 1;
    size_t size = 0;
    bool texture = desc.type != 0x83;
    if (texture) {
        auto info = textureInfo(desc);
        if (info.samples != 1 || !info.mips || sub >= info.mips * info.layers || kind == 5 ||
            info.format >= 103 && info.format <= 105)
            throw std::runtime_error("Unsupported mapped texture layout");
        auto mip = sub % info.mips;
        auto pitch = pitches(std::max(1u, info.width >> mip), std::max(1u, info.height >> mip), info.format);
        row = pitch.first;
        rows = pitch.second;
        depth = std::max(1u, info.depth >> mip);
        size = size_t(row) * rows * depth;
        if (frame_.entry(dataId).type != 1 && info.dimension != 2)
            throw std::runtime_error("Capture mapped texture pitches are unavailable");
    } else {
        if (sub)
            throw std::runtime_error("Buffer subresource must be zero");
        size = desc.desc[0];
    }
    auto updates = frame_.updates(dataId, size);
    auto obj = get<ID3D11Resource>(id);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    check(context_->Map(obj, sub, D3D11_MAP(kind), flags, &mapped), "Map captured writes");
    try {
        if (texture && frame_.entry(dataId).type == 1) {
            if (mapped.RowPitch < row || (depth > 1 && mapped.DepthPitch < uint64_t(mapped.RowPitch) * rows))
                throw std::runtime_error("Mapped pitch too small");
            auto data = updates[0].second;
            for (UINT z = 0; z < depth; ++z)
                for (UINT y = 0; y < rows; ++y)
                    std::memcpy(static_cast<uint8_t *>(mapped.pData) + size_t(z) * mapped.DepthPitch +
                                    size_t(y) * mapped.RowPitch,
                                data.data() + (size_t(z) * rows + y) * row, row);
        } else
            for (auto [offset, data] : updates)
                std::memcpy(static_cast<uint8_t *>(mapped.pData) + offset, data.data(), data.size());
    } catch (...) {
        context_->Unmap(obj, sub);
        throw;
    }
    context_->Unmap(obj, sub);
    counts["Map"]++;
}
void Replay::command(const Entry &e) {
    auto t = e.type;
    Bytes payload = frame_.payload(e.id);
    if (auto it = options_.commandPayloads.find(e.id); it != options_.commandPayloads.end())
        payload = it->second;
    Reader r(payload);
    if (isDraw(t)) {
        auto event = frame_.event(e.id);
        immediate(event.context);
        auto state = frame_.state(event.state);
        withBufferEdits(event, state, [&] {
            bind(state, t == 0x35 || t == 0x36);
            if ((options_.before && e.id == options_.until) || options_.disabled.contains(e.id) ||
                (options_.suppressDraws && t != 0x35 && t != 0x36))
                return false;
            const auto &a = event.args;
            if (options_.timings) {
                Timestamp timestamp{e.id};
                D3D11_QUERY_DESC desc{D3D11_QUERY_TIMESTAMP, 0};
                check(device_->CreateQuery(&desc, &timestamp.begin), "Create timestamp");
                check(device_->CreateQuery(&desc, &timestamp.end), "Create timestamp");
                context_->End(timestamp.begin.Get());
                timestamps_.push_back(std::move(timestamp));
            }
            switch (t) {
            case 0x35:
                context_->Dispatch(a[0], a[1], a[2]);
                break;
            case 0x36:
                context_->DispatchIndirect(get<ID3D11Buffer>(event.argumentBuffer), a[0]);
                break;
            case 0x37:
                context_->Draw(a[0], a[1]);
                break;
            case 0x38:
                throw std::runtime_error("DrawAuto count recovery migration pending");
            case 0x39:
                context_->DrawIndexed(a[0], a[1], int32_t(a[2]));
                break;
            case 0x3a:
                context_->DrawIndexedInstanced(a[0], a[1], a[2], int32_t(a[3]), a[4]);
                break;
            case 0x3b:
                context_->DrawIndexedInstancedIndirect(get<ID3D11Buffer>(event.argumentBuffer), a[0]);
                break;
            case 0x3c:
                context_->DrawInstanced(a[0], a[1], a[2], a[3]);
                break;
            case 0x3d:
                context_->DrawInstancedIndirect(get<ID3D11Buffer>(event.argumentBuffer), a[0]);
                break;
            }
            counts[commandName(t)]++;
            if (options_.timings)
                context_->End(timestamps_.back().end.Get());
            return true;
        });
        return;
    }
    if (isWritableCommand(t))
        validateWritableCommand(frame_, e.id);
    if (options_.disabled.contains(e.id))
        return;
    if ((t >= 0x249 && t <= 0x254) || t == 0x34e5 || t == 0x34ee || t == 0x34f4 || t == 0x351c ||
        t == 0x3520 || t == 0x3525) {
        constantBuffers(e);
        return;
    }
    if (t == 0x34ff || t == 0x3500 || t == 0x3522 || t == 0x25e) {
        outputs(e);
        return;
    }
    if (t == 0x246) {
        mappedWrites(e);
        return;
    }
    r.skip(16);
    if (t == 0x32 || t == 0x33 || t == 0x34) {
        auto view = r.read<Id>();
        if (!r.flag())
            throw std::runtime_error("Missing clear values");
        auto values = r.array<UINT, 4>();
        r.end();
        float floats[4];
        std::memcpy(floats, values.data(), 16);
        if (t == 0x32)
            context_->ClearRenderTargetView(get<ID3D11RenderTargetView>(view), floats);
        else if (t == 0x33)
            context_->ClearUnorderedAccessViewUint(get<ID3D11UnorderedAccessView>(view), values.data());
        else
            context_->ClearUnorderedAccessViewFloat(get<ID3D11UnorderedAccessView>(view), floats);
    } else if (t == 0x31) {
        auto view = r.read<Id>();
        auto flags = r.read<UINT>();
        auto depth = r.read<float>();
        auto stencil = r.read<uint8_t>();
        r.end();
        context_->ClearDepthStencilView(get<ID3D11DepthStencilView>(view), flags, depth, stencil);
    } else if (t == 0x3e) {
        auto dst = r.read<Id>(), src = r.read<Id>();
        r.end();
        context_->CopyResource(get<ID3D11Resource>(dst), get<ID3D11Resource>(src));
    } else if (t == 0x3f) {
        auto dst = r.read<Id>();
        auto offset = r.read<UINT>();
        auto src = r.read<Id>();
        r.end();
        context_->CopyStructureCount(get<ID3D11Buffer>(dst), offset, get<ID3D11UnorderedAccessView>(src));
    } else if (t == 0x40) {
        auto dst = r.read<Id>();
        auto sub = r.read<UINT>(), x = r.read<UINT>(), y = r.read<UINT>(), z = r.read<UINT>();
        auto src = r.read<Id>();
        auto srcSub = r.read<UINT>();
        bool has = r.flag();
        D3D11_BOX box{};
        if (has)
            box = r.read<D3D11_BOX>();
        r.end();
        context_->CopySubresourceRegion(get<ID3D11Resource>(dst), sub, x, y, z, get<ID3D11Resource>(src),
                                        srcSub, has ? &box : nullptr);
    } else if (t == 0x42) {
        auto dst = r.read<Id>();
        auto sub = r.read<UINT>();
        auto src = r.read<Id>();
        auto srcSub = r.read<UINT>(), format = r.read<UINT>();
        r.end();
        context_->ResolveSubresource(get<ID3D11Resource>(dst), sub, get<ID3D11Resource>(src), srcSub,
                                     DXGI_FORMAT(format));
    } else if (t == 0x245) {
        auto id = r.read<Id>();
        r.end();
        context_->GenerateMips(get<ID3D11ShaderResourceView>(id));
    } else if (t == 0x247) {
        auto layout = updateSourceLayout(frame_, e.id);
        D3D11_BOX box{};
        if (layout.hasBox)
            std::memcpy(&box, layout.box.data(), sizeof box);
        Bytes data;
        if (auto it = options_.updateSources.find(e.id); it != options_.updateSources.end())
            data = it->second;
        else
            data = frame_.data(layout.data);
        if (data.size() != layout.size)
            throw std::runtime_error("Packed UpdateSubresource size mismatch");
        context_->UpdateSubresource(get<ID3D11Resource>(layout.destination), layout.subresource,
                                    layout.hasBox ? &box : nullptr, data.data(), layout.rowPitch,
                                    layout.slicePitch);
    } else if (t == 0x242) {
        r.end();
        context_->ClearState();
        for (auto &ranges : ranges_)
            ranges.clear();
    } else if (t == 0x244) {
        r.end();
        context_->Flush();
    } else {
        static const std::set<uint16_t> auxiliary{
            0x3012, 0x3013, 0x3014, 0x3017, 0x3019, 0x302e, 0x3146, 0x324f, 0x3250, 0x3251, 0x3256, 0x3257,
            0x3261, 0x3575, 0x3576, 0x3577, 0x3578, 0x3597, 0x34e6, 0x34e7, 0x34e8, 0x34e9, 0x34ec, 0x34ed,
            0x34ef, 0x34f0, 0x34f1, 0x34f5, 0x34f6, 0x34f7, 0x34f8, 0x34fb, 0x3501, 0x3502, 0x3509, 0x350a,
            0x3519, 0x351a, 0x351b, 0x351d, 0x351e, 0x351f, 0x3521, 0x3523, 0x3524};
        if (!auxiliary.contains(t))
            throw std::runtime_error("Command migration pending: " + commandName(t));
        counts["state_or_auxiliary_records"]++;
        return;
    }
    counts[commandName(t)]++;
}
void Replay::run(const std::function<void(Id, size_t, size_t)> &progress) {
    if (options_.until && frame_.entry(options_.until).category != 7)
        throw std::runtime_error("Stop event is not an API command");
    context_->ClearState();
    objects_.clear();
    usedSrvs_.clear();
    counts.clear();
    for (auto &ranges : ranges_)
        ranges.clear();
    lastTarget_ = 0;
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
    size_t total = 0, done = 0;
    for (auto &[id, e] : frame_.entries())
        if (e.category == 7)
            ++total;
    for (auto &[id, e] : frame_.entries())
        if (e.category == 7) {
            if (options_.until &&
                (id > options_.until || (options_.before && id == options_.until && !isDraw(e.type))))
                break;
            try {
                command(e);
            } catch (const std::exception &error) {
                throw std::runtime_error("Event " + std::to_string(id) + " (" + commandName(e.type) +
                                         "): " + error.what());
            }
            if (progress && ((++done % 128) == 0 || isDraw(e.type)))
                progress(id, done, total);
        }
    if (options_.timings) {
        context_->End(stats.Get());
        context_->End(disjoint.Get());
    }
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
}
Image Replay::output(Id texture, UINT sub) {
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
    Com<ID3DBlob> blob;
    check(D3DDisassemble(data.data(), data.size(), D3D_DISASM_ENABLE_INSTRUCTION_NUMBERING, nullptr, &blob),
          "Disassemble DXBC");
    return {static_cast<const char *>(blob->GetBufferPointer()), blob->GetBufferSize()};
}
} // namespace flora
