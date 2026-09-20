#include "Replay.h"
namespace flora {
void Replay::applySamplerEdits(Id event) {
    const auto found = options_.samplerEdits.find(event);
    if (found == options_.samplerEdits.end())
        return;
    using Setter = void (STDMETHODCALLTYPE ID3D11DeviceContext::*)(UINT, UINT, ID3D11SamplerState *const *);
    static constexpr Setter setters[]{
        &ID3D11DeviceContext::VSSetSamplers, &ID3D11DeviceContext::HSSetSamplers,
        &ID3D11DeviceContext::DSSetSamplers, &ID3D11DeviceContext::GSSetSamplers,
        &ID3D11DeviceContext::PSSetSamplers, &ID3D11DeviceContext::CSSetSamplers};
    for (const auto &[target, desc] : found->second) {
        auto [stage, slot] = target;
        if (stage >= 6 || slot >= 16)
            throw std::runtime_error("Invalid sampler experiment target");
        if (desc.Filter & 256) {
            if (!samplerMinMax_) {
                D3D11_FEATURE_DATA_D3D11_OPTIONS1 caps{};
                samplerMinMax_ = SUCCEEDED(device_->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS1, &caps,
                                                                        sizeof(caps))) &&
                                 caps.MinMaxFiltering;
            }
            if (!*samplerMinMax_)
                throw std::runtime_error("Device does not support minimum/maximum sampler filtering");
        }
        static_assert(sizeof(D3D11_SAMPLER_DESC) == 52);
        std::array<uint8_t, 52> key;
        std::memcpy(key.data(), &desc, key.size());
        auto &state = editedSamplers_[key];
        if (!state)
            check(device_->CreateSamplerState(&desc, &state), "Create experimental sampler");
        auto ptr = state.Get();
        (context_.Get()->*setters[stage])(slot, 1, &ptr);
    }
}
} // namespace flora
