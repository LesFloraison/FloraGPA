#include "BlendState.h"
#include "Replay.h"
#include "core/ClassLinkage.h"
#include "core/Commands.h"
#include "core/Dxbc.h"
#include <d3dcompiler.h>
#include <tuple>
namespace flora {
Com<IUnknown> Replay::createCapturedShader(Id id, uint16_t t, Bytes data, Id linkage, Id so) {
    Com<IUnknown> result;
    auto classLinkage = get<ID3D11ClassLinkage>(linkage);
    Com<ID3D11ShaderReflection> reflection;
    D3D11_SHADER_DESC sd{};
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
    bool hasRdef = false, hasInterfaces = false;
    Bytes program;
    for (UINT i = 0; i < chunks; ++i) {
        auto offset = dxbc.read<UINT>();
        if (offset < 32 + chunks * 4 || offset > data.size() || data.size() - offset < 8)
            throw std::runtime_error("DXBC chunk offset");
        Reader chunk(data.subspan(offset));
        auto tag = chunk.read<UINT>(), size = chunk.read<UINT>();
        auto body = chunk.take(size);
        if (tag == 0x58454853 || (tag == 0x52444853 && program.empty()))
            program = body;
        hasRdef |= tag == 0x46454452;
        hasInterfaces |= tag == 0x45434649;
    }
    bool passthrough = t == 0x91 && so && (program.empty() || (Reader(program).read<UINT>() >> 16) != 2);
    if (passthrough) {
        if (!program.empty()) {
            auto stage = Reader(program).read<UINT>() >> 16;
            if (stage != 1 && stage != 4)
                throw std::runtime_error("SO passthrough requires VS, DS or signature bytecode");
        }
        passthroughShaders_.insert(id);
    } else {
        check(D3DReflect(data.data(), data.size(), IID_ID3D11ShaderReflection, &reflection),
              "Reflect shader");
        check(reflection->GetDesc(&sd), "Shader descriptor");
    }
    if (!hasRdef && !passthrough)
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
    // Stripped SM4 reflection does not provide a reliable interface count.
    auto slots = hasInterfaces && !passthrough ? reflection->GetNumInterfaceSlots() : 0;
    validateClassProgram(program, slots);
    interfaceSlots_[id] = slots;
    usedSrvs_[id] = used;
#define CREATE_SHADER(Type, Method)                                                                          \
    {                                                                                                        \
        Com<Type> obj;                                                                                       \
        check(device_->Method(data.data(), data.size(), classLinkage, &obj), #Method);                       \
        result = obj;                                                                                        \
    }
    switch (t) {
    case 0x90:
        CREATE_SHADER(ID3D11VertexShader, CreateVertexShader);
        break;
    case 0x91:
        if (so)
            result = createStreamOutputShader(data, so, classLinkage);
        else
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
    Com<IUnknown> identity;
    check(result.As(&identity), "Shader identity");
    shaderCounterUse_[identity.Get()] = passthrough ? true : shaderMayUseHiddenCounters(data);
    if (resourceLodAudit_ && !resourceLodAudit_->clamped.empty()) {
        resourceLodShaderSrvs_[identity.Get()] =
            passthrough ? std::array<bool, 128>{} : shaderSrvDeclarations(data);
    }
    return result;
}
namespace {
bool sameBlend(const D3D11_BLEND_DESC1 &a, const D3D11_BLEND_DESC1 &b) {
    if (bool(a.AlphaToCoverageEnable) != bool(b.AlphaToCoverageEnable) ||
        bool(a.IndependentBlendEnable) != bool(b.IndependentBlendEnable))
        return false;
    for (unsigned i = 0; i < (a.IndependentBlendEnable ? 8u : 1u); ++i) {
        const auto &x = a.RenderTarget[i], &y = b.RenderTarget[i];
        if (bool(x.BlendEnable) != bool(y.BlendEnable) || bool(x.LogicOpEnable) != bool(y.LogicOpEnable) ||
            x.RenderTargetWriteMask != y.RenderTargetWriteMask)
            return false;
        if (x.BlendEnable &&
            std::tie(x.SrcBlend, x.DestBlend, x.BlendOp, x.SrcBlendAlpha, x.DestBlendAlpha, x.BlendOpAlpha) !=
                std::tie(y.SrcBlend, y.DestBlend, y.BlendOp, y.SrcBlendAlpha, y.DestBlendAlpha,
                         y.BlendOpAlpha))
            return false;
        if (x.LogicOpEnable && x.LogicOp != y.LogicOp)
            return false;
    }
    return true;
}
} // namespace
void Replay::pipelineCreation(const Entry &entry) {
    if (!pipelineCreationAudit_)
        pipelineCreationAudit_ = auditPipelineCreations(frame_);
    const auto &c = requirePipelineCreation(*pipelineCreationAudit_, entry.id);
    if (c.result != 0) {
        ++counts["pipeline_creation_observations"];
        return;
    }
    if (!c.note.empty()) {
        ++counts[c.type == 0x3580 ? "unmaterialized_layout_creations" : "unmaterialized_so_creations"];
        return;
    }
    Com<IUnknown> created;
    if (isStateCreation(c.type)) {
        const bool saved = frame_.entries().contains(c.resource);
        auto savedBytes = saved ? frame_.capturedPayload(c.resource).subspan(16) : Bytes{};
        bool equal = true;
        auto create = [&]<class T, class Desc>(auto method) {
            Desc desc{};
            std::memcpy(&desc, c.descriptor.data(), sizeof desc);
            Com<T> v;
            check((device_.Get()->*method)(&desc, v.GetAddressOf()), "Captured state creation");
            return v;
        };
        if (c.type == 0x3589) {
            auto v = create.template operator()<ID3D11BlendState, D3D11_BLEND_DESC>(
                &ID3D11Device::CreateBlendState);
            if (saved) {
                Com<ID3D11BlendState1> extended;
                check(v.As(&extended), "Query created blend state");
                D3D11_BLEND_DESC1 actual{};
                extended->GetDesc1(&actual);
                equal = sameBlend(actual, decodeBlend(savedBytes, frame_.entry(c.resource).type == 0x10d));
            }
            created = v;
        } else if (c.type == 0x358a) {
            auto v = create.template operator()<ID3D11DepthStencilState, D3D11_DEPTH_STENCIL_DESC>(
                &ID3D11Device::CreateDepthStencilState);
            if (saved) {
                D3D11_DEPTH_STENCIL_DESC actual{};
                v->GetDesc(&actual);
                auto p = reinterpret_cast<const uint8_t *>(&actual);
                equal = std::equal(savedBytes.begin(), savedBytes.begin() + 18, p) &&
                        std::equal(savedBytes.begin() + 20, savedBytes.end(), p + 20);
            }
            created = v;
        } else if (c.type == 0x358b) {
            auto v = create.template operator()<ID3D11RasterizerState, D3D11_RASTERIZER_DESC>(
                &ID3D11Device::CreateRasterizerState);
            if (saved) {
                Com<ID3D11RasterizerState2> extended;
                check(v.As(&extended), "Query created rasterizer state");
                D3D11_RASTERIZER_DESC2 actual{};
                extended->GetDesc2(&actual);
                equal = std::equal(savedBytes.begin(), savedBytes.end(),
                                   reinterpret_cast<const uint8_t *>(&actual));
            }
            created = v;
        } else {
            auto v = create.template operator()<ID3D11SamplerState, D3D11_SAMPLER_DESC>(
                &ID3D11Device::CreateSamplerState);
            if (saved) {
                D3D11_SAMPLER_DESC actual{};
                v->GetDesc(&actual);
                equal = std::equal(savedBytes.begin(), savedBytes.end(),
                                   reinterpret_cast<const uint8_t *>(&actual));
            }
            created = v;
        }
        if (!equal)
            throw std::runtime_error("Native created state does not reproduce saved canonical descriptor");
        if (auto it = objects_.find(c.resource); it != objects_.end()) {
            Com<IUnknown> a, b;
            check(it->second.As(&a), "Existing immutable state identity");
            check(created.As(&b), "Created immutable state identity");
            if (a.Get() != b.Get())
                throw std::runtime_error("Repeated state creation did not preserve cached native identity");
        }
    } else if (c.type == 0x3580) {
        std::vector<D3D11_INPUT_ELEMENT_DESC> elements;
        for (const auto &element : c.elements) {
            const auto &v = element.fields;
            elements.push_back({element.name.c_str(), v[0], DXGI_FORMAT(v[1]), v[2], v[3],
                                D3D11_INPUT_CLASSIFICATION(v[4]), v[5]});
        }
        Com<ID3D11InputLayout> layout;
        check(device_->CreateInputLayout(elements.data(), UINT(elements.size()), c.bytecode.data(),
                                         c.bytecode.size(), &layout),
              "Captured input layout creation");
        created = layout;
    } else {
        created = createCapturedShader(c.resource, pipelineCreatedType(c.type), c.bytecode, c.linkage,
                                       c.streamOutputId);
        if (auto it = options_.shaders.find(c.resource); it != options_.shaders.end())
            created = createCapturedShader(c.resource, pipelineCreatedType(c.type), it->second, c.linkage,
                                           c.streamOutputId);
    }
    objects_.insert_or_assign(c.resource, created);
    ++counts[commandName(c.type)];
}
} // namespace flora
