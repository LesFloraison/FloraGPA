#include "BlendState.h"
namespace flora {
Com<ID3D11BlendState> Replay::createBlend(const D3D11_BLEND_DESC1 &desc, bool extended) {
    const bool logic = usesLogic(desc);
    Com<ID3D11Device1> dev;
    if (logic || extended)
        device_.As(&dev);
    if (logic && !dev)
        throw std::runtime_error("Logic operations require ID3D11Device1");
    Com<ID3D11BlendState> result;
    if ((!logic && !extended) || !dev) {
        D3D11_BLEND_DESC basic{};
        basic.AlphaToCoverageEnable = desc.AlphaToCoverageEnable;
        basic.IndependentBlendEnable = desc.IndependentBlendEnable;
        for (size_t i = 0; i < 8; ++i) {
            const auto &a = desc.RenderTarget[i];
            basic.RenderTarget[i] = {
                a.BlendEnable,   a.SrcBlend,       a.DestBlend,    a.BlendOp,
                a.SrcBlendAlpha, a.DestBlendAlpha, a.BlendOpAlpha, a.RenderTargetWriteMask};
        }
        check(device_->CreateBlendState(&basic, &result), "CreateBlendState");
    } else {
        if (logic) {
            D3D11_FEATURE_DATA_D3D11_OPTIONS options{};
            check(device_->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &options, sizeof options),
                  "Read blend logic support");
            if (!options.OutputMergerLogicOp)
                throw std::runtime_error("Device does not support OutputMergerLogicOp");
        }
        Com<ID3D11BlendState1> state;
        check(dev->CreateBlendState1(&desc, &state), "CreateBlendState1");
        result = state;
    }
    if (logic)
        logicBlendStates_[result.Get()] = result;
    return result;
}
void Replay::validateBlendOutputs(const State &state, ID3D11BlendState *blend) {
    if (!logicBlendStates_.contains(blend))
        return;
    for (UINT i = 0; i < std::min({state.rtCount, state.omStart, 8u}); ++i) {
        if (!state.rtv[i])
            continue;
        Reader view(frame_.payload(state.rtv[i], 5, 0x8d));
        view.skip(24);
        auto format = view.read<DXGI_FORMAT>();
        if (!logicFormats_.contains(format)) {
            D3D11_FEATURE_DATA_FORMAT_SUPPORT2 data{format, 0};
            check(device_->CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT2, &data, sizeof data),
                  "Read logic output format support");
            logicFormats_[format] =
                (data.OutFormatSupport2 & D3D11_FORMAT_SUPPORT2_OUTPUT_MERGER_LOGIC_OP) != 0;
        }
        if (!logicFormats_.at(format))
            throw std::runtime_error("RTV format " + std::to_string(format) +
                                     " does not support output-merger logic operations");
    }
}
void Replay::applyBlendEdit(Id event, const State &state) {
    auto it = options_.blendEdits.find(event);
    if (it == options_.blendEdits.end())
        return;
    const auto &edit = it->second;
    auto blend = edit.descriptor ? createBlend(*edit.descriptor, true)
                                 : Com<ID3D11BlendState>(get<ID3D11BlendState>(state.blend));
    validateBlendOutputs(state, blend.Get());
    const auto factor = edit.factor.value_or(state.blendFactor);
    context_->OMSetBlendState(blend.Get(), factor.data(), edit.sampleMask.value_or(state.sampleMask));
}
} // namespace flora
