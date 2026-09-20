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
const SrvObservation &Replay::srvHistory(Id event, bool after) {
    const auto &entry = frame_.entry(event);
    Reader r(frame_.payload(event));
    r.skip(isDraw(entry.type) ? 16 : 8);
    const auto context = r.read<Id>();
    auto &history = srvHistories_[context];
    if (!history)
        history = std::make_unique<SrvHistory>(frame_, context);
    return history->advance(event, after);
}
void Replay::observeSrvBindings(Id event) {
    if (!srvBindings_.active())
        return;
    if (outputGap_) {
        srvBindings_.hazards(srvHazards_, srvHistory(event, true).outputs);
        return;
    }
    using Getter = void (STDMETHODCALLTYPE ID3D11DeviceContext::*)(UINT, UINT, ID3D11ShaderResourceView **);
    static constexpr Getter getters[]{
        &ID3D11DeviceContext::VSGetShaderResources, &ID3D11DeviceContext::HSGetShaderResources,
        &ID3D11DeviceContext::DSGetShaderResources, &ID3D11DeviceContext::GSGetShaderResources,
        &ID3D11DeviceContext::PSGetShaderResources, &ID3D11DeviceContext::CSGetShaderResources};
    for (unsigned stage = 0; stage < 6; ++stage) {
        if (!srvBindings_.active(stage))
            continue;
        std::array<ID3D11ShaderResourceView *, 128> views{};
        (context_.Get()->*getters[stage])(0, 128, views.data());
        for (unsigned slot = 0; slot < 128; ++slot) {
            if (!views[slot])
                srvBindings_.unbind(stage, slot);
            else
                views[slot]->Release();
        }
    }
}
} // namespace flora
