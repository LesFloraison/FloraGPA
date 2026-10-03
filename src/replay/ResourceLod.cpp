#include "Replay.h"
namespace flora {
void Replay::requireResourceLod(Id resource) const {
    if (resourceLodAudit_ && resourceLodAudit_->clamped.contains(resource) &&
        !resourceLods_.contains(resource))
        throw std::runtime_error("Resource " + std::to_string(resource) +
                                 ": initial minimum LOD is unresolved at this access");
}
void Replay::requireBoundResourceLods(bool compute) {
    // Only inspect native bindings when a materialized clamp resource remains
    // unresolved. Binding a texture before its setter is legal and is not access.
    std::map<ID3D11Resource *, Id> unknown;
    if (!resourceLodAudit_)
        return;
    for (auto id : resourceLodAudit_->clamped)
        if (!resourceLods_.contains(id) && objects_.contains(id)) {
            Com<ID3D11Resource> resource;
            check(objects_.at(id).As(&resource), "Query unresolved LOD resource");
            unknown.emplace(resource.Get(), id);
        }
    if (unknown.empty())
        return;
    auto checkView = [&](ID3D11View *view) {
        if (!view)
            return;
        Com<ID3D11Resource> resource;
        view->GetResource(&resource);
        if (auto it = unknown.find(resource.Get()); it != unknown.end())
            requireResourceLod(it->second);
    };
    using Get = void (STDMETHODCALLTYPE ID3D11DeviceContext::*)(UINT, UINT, ID3D11ShaderResourceView **);
    static constexpr Get getters[]{
        &ID3D11DeviceContext::VSGetShaderResources, &ID3D11DeviceContext::HSGetShaderResources,
        &ID3D11DeviceContext::DSGetShaderResources, &ID3D11DeviceContext::GSGetShaderResources,
        &ID3D11DeviceContext::PSGetShaderResources, &ID3D11DeviceContext::CSGetShaderResources};
    for (unsigned stage = compute ? 5 : 0; stage < (compute ? 6u : 5u); ++stage) {
        std::array<ID3D11ShaderResourceView *, 128> raw{};
        std::array<Com<ID3D11ShaderResourceView>, 128> owned;
        (context_.Get()->*getters[stage])(0, 128, raw.data());
        for (size_t i = 0; i < raw.size(); ++i)
            owned[i].Attach(raw[i]);
        for (auto view : raw)
            checkView(view);
    }
    std::array<ID3D11UnorderedAccessView *, 64> raw{};
    std::array<Com<ID3D11UnorderedAccessView>, 64> owned;
    if (compute)
        context_->CSGetUnorderedAccessViews(0, uavLimit_, raw.data());
    else
        context_->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, uavLimit_, raw.data());
    for (size_t i = 0; i < raw.size(); ++i)
        owned[i].Attach(raw[i]);
    for (auto view : raw)
        checkView(view);
    if (!compute) {
        std::array<ID3D11RenderTargetView *, 8> targets{};
        std::array<Com<ID3D11RenderTargetView>, 8> targetOwners;
        Com<ID3D11DepthStencilView> depth;
        context_->OMGetRenderTargets(8, targets.data(), &depth);
        for (size_t i = 0; i < targets.size(); ++i)
            targetOwners[i].Attach(targets[i]);
        for (auto view : targets)
            checkView(view);
        checkView(depth.Get());
    }
}
} // namespace flora
