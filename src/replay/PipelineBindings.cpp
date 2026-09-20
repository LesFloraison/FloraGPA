#include "Replay.h"
namespace flora {
bool Replay::pipelineSetter(const Entry &entry, Bytes payload) {
    if (!isPipelineSetter(entry.type))
        return false;
    const auto edit = options_.pipelineSetters.find(entry.id);
    if (edit == options_.pipelineSetters.end() && !activePipelineBindings_.contains(entry.type)) {
        ++counts["state_or_auxiliary_records"];
        return true;
    }
    const auto binding =
        edit == options_.pipelineSetters.end() ? readPipelineSetter(entry.type, payload) : edit->second;
    if (binding.type != entry.type)
        throw std::runtime_error("Pipeline setter type mismatch");
    validatePipelineBinding(frame_, binding);
    const auto &s = binding.values;
    if (auto stage = shaderSetterStage(entry.type)) {
        const auto &v = s.stages[*stage];
        bindShader(*stage, v.shader, std::span<const Id>(v.classes.data(), v.classCount));
    }
    switch (entry.type - 0x34de) {
    case 24:
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY(s.topology));
        break;
    case 35:
        context_->OMSetBlendState(get<ID3D11BlendState>(s.blend), s.blendFactor.data(), s.sampleMask);
        break;
    case 36:
        context_->OMSetDepthStencilState(get<ID3D11DepthStencilState>(s.depthState), s.stencilRef);
        break;
    case 43:
        context_->RSSetState(get<ID3D11RasterizerState>(s.rasterizer));
        break;
    case 44: {
        if (device_->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0)
            throw std::runtime_error("Viewport editing requires feature level 11_0");
        std::vector<D3D11_VIEWPORT> rows;
        for (auto &v : *s.viewportValues)
            rows.push_back({v[0], v[1], v[2], v[3], v[4], v[5]});
        context_->RSSetViewports(UINT(rows.size()), rows.empty() ? nullptr : rows.data());
        break;
    }
    case 45: {
        std::vector<D3D11_RECT> rows;
        for (auto &v : *s.scissorValues)
            rows.push_back({v[0], v[1], v[2], v[3]});
        context_->RSSetScissorRects(UINT(rows.size()), rows.empty() ? nullptr : rows.data());
        break;
    }
    }
    if (edit == options_.pipelineSetters.end())
        activePipelineBindings_.erase(entry.type);
    else
        activePipelineBindings_[entry.type] = binding;
    ++counts["state_or_auxiliary_records"];
    return true;
}
} // namespace flora
