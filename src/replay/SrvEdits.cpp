#include "Replay.h"
namespace flora {
void Replay::applySrvEdits(Id event, const State &state) {
    const auto edits = options_.srvEdits.find(event);
    if (edits == options_.srvEdits.end())
        return;
    using Setter =
        void (STDMETHODCALLTYPE ID3D11DeviceContext::*)(UINT, UINT, ID3D11ShaderResourceView *const *);
    static constexpr Setter setters[]{
        &ID3D11DeviceContext::VSSetShaderResources, &ID3D11DeviceContext::HSSetShaderResources,
        &ID3D11DeviceContext::DSSetShaderResources, &ID3D11DeviceContext::GSSetShaderResources,
        &ID3D11DeviceContext::PSSetShaderResources, &ID3D11DeviceContext::CSSetShaderResources};
    for (const auto &[target, descriptor] : edits->second) {
        const auto [stage, slot] = target;
        if (stage >= 6 || slot >= 128)
            throw std::runtime_error("Invalid SRV experiment target");
        auto current = get<ID3D11ShaderResourceView>(state.stages[stage].srv[slot]);
        if (!current)
            throw std::runtime_error("SRV descriptor editing requires a bound non-null view");
        // The captured view may currently refer to a scoped input clone.
        Com<ID3D11Resource> owner;
        current->GetResource(&owner);
        Com<ID3D11ShaderResourceView> view;
        check(device_->CreateShaderResourceView(owner.Get(), &descriptor, &view), "Create experimental SRV");
        const auto name = std::string("Experiment ") + samplerStageNames[stage] + " t" +
                          std::to_string(slot) + " at GPA " + std::to_string(event);
        view->SetPrivateData(WKPDID_D3DDebugObjectName, UINT(name.size()), name.data());
        auto ptr = view.Get();
        (context_.Get()->*setters[stage])(slot, 1, &ptr);
        // The context owns a bound view. Avoid retaining one object per event.
    }
}
} // namespace flora
