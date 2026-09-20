#include "Replay.h"
#include <d3d11shader.h>
#include <d3dcompiler.h>
namespace flora {
Com<ID3D11RasterizerState> Replay::createRasterizer(const D3D11_RASTERIZER_DESC2 &desc) {
    Com<ID3D11RasterizerState> result;
    if (!desc.ForcedSampleCount && !desc.ConservativeRaster) {
        D3D11_RASTERIZER_DESC base{};
        std::memcpy(&base, &desc, sizeof base);
        check(device_->CreateRasterizerState(&base, &result), "CreateRasterizerState");
        rasterizerExtensions_.erase(result.Get());
        return result;
    }
    if ((desc.ForcedSampleCount != 0 && desc.ForcedSampleCount != 1 && desc.ForcedSampleCount != 2 &&
         desc.ForcedSampleCount != 4 && desc.ForcedSampleCount != 8 && desc.ForcedSampleCount != 16) ||
        UINT(desc.ConservativeRaster) > 1)
        throw std::runtime_error("Unsupported rasterizer extension enum");
    if (desc.ConservativeRaster) {
        if (desc.FillMode != D3D11_FILL_SOLID)
            throw std::runtime_error("Conservative rasterization requires solid fill");
        Com<ID3D11Device3> dev;
        check(device_.As(&dev), "Conservative rasterization requires Device3");
        D3D11_FEATURE_DATA_D3D11_OPTIONS2 options{};
        check(device_->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS2, &options, sizeof options),
              "Read conservative raster tier");
        if (!options.ConservativeRasterizationTier)
            throw std::runtime_error("Device does not support conservative rasterization");
        Com<ID3D11RasterizerState2> state;
        check(dev->CreateRasterizerState2(&desc, &state), "CreateRasterizerState2");
        result = state;
    } else {
        Com<ID3D11Device1> dev;
        check(device_.As(&dev), "Forced sample count requires Device1");
        D3D11_RASTERIZER_DESC1 base{};
        std::memcpy(&base, &desc, sizeof base);
        Com<ID3D11RasterizerState1> state;
        check(dev->CreateRasterizerState1(&base, &state), "CreateRasterizerState1");
        result = state;
    }
    rasterizerExtensions_[result.Get()] = {result, desc.ForcedSampleCount};
    return result;
}
void Replay::applyRasterizerEdit(Id event) {
    auto it = options_.rasterizerEdits.find(event);
    if (it == options_.rasterizerEdits.end())
        return;
    const auto &edit = it->second;
    if (edit.descriptor) {
        auto state = createRasterizer(*edit.descriptor);
        context_->RSSetState(state.Get());
    }
    if (edit.viewports) {
        if (device_->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0)
            throw std::runtime_error("Viewport editing requires feature level 11_0");
        context_->RSSetViewports(UINT(edit.viewports->size()),
                                 edit.viewports->empty() ? nullptr : edit.viewports->data());
    }
    if (edit.scissors)
        context_->RSSetScissorRects(UINT(edit.scissors->size()),
                                    edit.scissors->empty() ? nullptr : edit.scissors->data());
}
void Replay::validateRasterizer(const State &state) {
    if (rasterizerExtensions_.empty())
        return;
    Com<ID3D11RasterizerState> rs;
    context_->RSGetState(&rs);
    const auto ext = rasterizerExtensions_.find(rs.Get());
    if (ext == rasterizerExtensions_.end() || !ext->second.second)
        return;
    const auto forced = ext->second.second;
    if (state.dsv)
        throw std::runtime_error("Forced sample count requires no DSV binding");
    Com<ID3D11DepthStencilState> ds;
    context_->OMGetDepthStencilState(&ds, nullptr);
    D3D11_DEPTH_STENCIL_DESC desc{};
    if (ds)
        ds->GetDesc(&desc);
    if (!ds || desc.DepthEnable)
        throw std::runtime_error("Forced sample count requires depth testing disabled");
    for (UINT i = 0; i < std::min({state.rtCount, state.omStart, 8u}); ++i) {
        if (!state.rtv[i])
            continue;
        Reader view(frame_.payload(state.rtv[i], 5, 0x8d));
        view.skip(16);
        auto resource = frame_.resource(view.read<Id>());
        if (forced > 1 && resource.type != 0x83 && textureInfo(resource).samples != 1)
            throw std::runtime_error("Forced sample count > 1 requires single-sample RTVs");
    }
    auto ps = state.stages[4].shader;
    if (!ps)
        return;
    Bytes bytes = options_.shaders.contains(ps) ? Bytes(options_.shaders.at(ps))
                                                : frame_.shader(frame_.resource(ps).data);
    Com<ID3D11ShaderReflection> reflection;
    check(D3DReflect(bytes.data(), bytes.size(), IID_ID3D11ShaderReflection, &reflection),
          "Reflect forced-sample shader");
    if (reflection->IsSampleFrequencyShader())
        throw std::runtime_error("Forced sample count forbids sample-frequency pixel shaders");
    D3D11_SHADER_DESC shader{};
    check(reflection->GetDesc(&shader), "Read forced-sample shader");
    for (UINT i = 0; i < shader.OutputParameters; ++i) {
        D3D11_SIGNATURE_PARAMETER_DESC p{};
        check(reflection->GetOutputParameterDesc(i, &p), "Read pixel output");
        if (p.SystemValueType == D3D_NAME_DEPTH || p.SystemValueType == D3D_NAME_DEPTH_GREATER_EQUAL ||
            p.SystemValueType == D3D_NAME_DEPTH_LESS_EQUAL ||
            (p.SemanticName &&
             (!_stricmp(p.SemanticName, "SV_Depth") || !_stricmp(p.SemanticName, "SV_DepthGreaterEqual") ||
              !_stricmp(p.SemanticName, "SV_DepthLessEqual"))))
            throw std::runtime_error("Forced sample count forbids depth-output pixel shaders");
    }
}
} // namespace flora
