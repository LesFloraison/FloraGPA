#include "present_bindings.hpp"
#include <wrl/client.h>
#include <array>
#include <vector>
#include <stdexcept>
namespace flora::capture::hud {
using Microsoft::WRL::ComPtr;
namespace {
struct LastError {DWORD value=GetLastError();~LastError(){SetLastError(value);}};
void check(HRESULT hr){if(FAILED(hr))throw std::runtime_error("Cannot create native presentation mirror view");}
using SrvGetter=void(STDMETHODCALLTYPE ID3D11DeviceContext::*)(UINT,UINT,ID3D11ShaderResourceView**);
using SrvSetter=void(STDMETHODCALLTYPE ID3D11DeviceContext::*)(UINT,UINT,ID3D11ShaderResourceView*const*);
constexpr SrvGetter srv_getters[]{&ID3D11DeviceContext::VSGetShaderResources,&ID3D11DeviceContext::HSGetShaderResources,&ID3D11DeviceContext::DSGetShaderResources,&ID3D11DeviceContext::GSGetShaderResources,&ID3D11DeviceContext::PSGetShaderResources,&ID3D11DeviceContext::CSGetShaderResources};
constexpr SrvSetter srv_setters[]{&ID3D11DeviceContext::VSSetShaderResources,&ID3D11DeviceContext::HSSetShaderResources,&ID3D11DeviceContext::DSSetShaderResources,&ID3D11DeviceContext::GSSetShaderResources,&ID3D11DeviceContext::PSSetShaderResources,&ID3D11DeviceContext::CSSetShaderResources};
bool uses(ID3D11View* view,IUnknown* clean){if(!view)return false;ComPtr<ID3D11Resource> resource;view->GetResource(&resource);ComPtr<IUnknown> identity;check(resource.As(&identity));return identity.Get()==clean;}
template<class T> struct Mirror {UINT stage,slot;ComPtr<T> application,native;};
}
struct PresentBindings::Impl {
    ComPtr<ID3D11DeviceContext> context;
    std::vector<Mirror<ID3D11ShaderResourceView>> srvs;
    std::vector<Mirror<ID3D11UnorderedAccessView>> uavs;
    std::vector<Mirror<ID3D11RenderTargetView>> rtvs;
    bool applied=false;
    UINT uav_slots=8;
    void output_uavs(bool restore) noexcept {
        bool needed=false;for(const auto& mirror:uavs)needed|=mirror.stage==1;if(!needed)return;
        std::array<ID3D11UnorderedAccessView*,64> raw{};std::array<ComPtr<ID3D11UnorderedAccessView>,64> owners;
        context->OMGetRenderTargetsAndUnorderedAccessViews(0,nullptr,nullptr,0,uav_slots,raw.data());for(UINT i=0;i<uav_slots;++i)owners[i].Attach(raw[i]);
        bool changed=false;for(const auto& mirror:uavs)if(mirror.stage==1){auto*& value=raw[mirror.slot];if(!restore){value=mirror.native.Get();changed=true;}else if(value==mirror.native.Get()){value=mirror.application.Get();changed=true;}}
        if(!changed)return;UINT first=uav_slots,last=0;for(UINT i=0;i<uav_slots;++i)if(raw[i]){first=(std::min)(first,i);last=i+1;}
        if(first==uav_slots)return;
        // OM's specified UAV range replaces the whole range, unlike CS's slot
        // setter. Include every surviving output UAV, with KEEP counter values.
        std::array<UINT,64> counts;counts.fill(UINT(-1));
        context->OMSetRenderTargetsAndUnorderedAccessViews(D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL,nullptr,nullptr,first,last-first,raw.data()+first,counts.data());
    }
    void outputs(bool restore) noexcept {
        if(rtvs.empty())return;
        std::array<ID3D11RenderTargetView*,8> raw{};ComPtr<ID3D11DepthStencilView> depth;
        context->OMGetRenderTargets(UINT(raw.size()),raw.data(),&depth);
        std::array<ComPtr<ID3D11RenderTargetView>,8> owners;for(size_t i=0;i<raw.size();++i)owners[i].Attach(raw[i]);
        bool changed=false;for(const auto& mirror:rtvs){auto*& value=raw[mirror.slot];if(!restore){value=mirror.native.Get();changed=true;}else if(value==mirror.native.Get()){value=mirror.application.Get();changed=true;}}
        if(!changed)return;UINT count=0;for(UINT i=0;i<raw.size();++i)if(raw[i])count=i+1;
        // KEEP preserves every UAV and its hidden counter. Do not use ClearState.
        context->OMSetRenderTargetsAndUnorderedAccessViews(count,raw.data(),depth.Get(),0,D3D11_KEEP_UNORDERED_ACCESS_VIEWS,nullptr,nullptr);
    }
    void inputs(bool restore) noexcept {
        for(const auto& mirror:srvs){
            if(restore){ComPtr<ID3D11ShaderResourceView> current;(context.Get()->*srv_getters[mirror.stage])(mirror.slot,1,&current);if(current!=mirror.native)continue;}
            auto* view=restore?mirror.application.Get():mirror.native.Get();(context.Get()->*srv_setters[mirror.stage])(mirror.slot,1,&view);
        }
        const UINT keep=UINT(-1);
        for(const auto& mirror:uavs){
            if(mirror.stage)continue;
            if(restore){ComPtr<ID3D11UnorderedAccessView> current;context->CSGetUnorderedAccessViews(mirror.slot,1,&current);if(current!=mirror.native)continue;}
            auto* view=restore?mirror.application.Get():mirror.native.Get();
            context->CSSetUnorderedAccessViews(mirror.slot,1,&view,&keep);
        }
        output_uavs(restore);
    }
};
PresentBindings::PresentBindings(ID3D11DeviceContext* context,ID3D11Texture2D* native,ID3D11Texture2D* clean):impl_(std::make_unique<Impl>()){
    LastError preserve;auto& p=*impl_;p.context=context;ComPtr<ID3D11Device> device;context->GetDevice(&device);ComPtr<IUnknown> identity;check(clean->QueryInterface(IID_PPV_ARGS(&identity)));
    // Finish all allocations and native view creation before mutating bindings.
    for(UINT stage=0;stage<6;++stage){
        std::array<ID3D11ShaderResourceView*,128> raw{};std::array<ComPtr<ID3D11ShaderResourceView>,128> views;
        (context->*srv_getters[stage])(0,UINT(raw.size()),raw.data());for(UINT slot=0;slot<raw.size();++slot)views[slot].Attach(raw[slot]);
        for(UINT slot=0;slot<views.size();++slot){
            if(!uses(views[slot].Get(),identity.Get()))continue;
            Mirror<ID3D11ShaderResourceView> mirror{stage,slot,views[slot]};D3D11_SHADER_RESOURCE_VIEW_DESC desc{};mirror.application->GetDesc(&desc);check(device->CreateShaderResourceView(native,&desc,&mirror.native));p.srvs.push_back(std::move(mirror));
        }
    }
    std::array<ID3D11RenderTargetView*,8> raw_rtvs{};std::array<ComPtr<ID3D11RenderTargetView>,8> rtvs;
    context->OMGetRenderTargets(UINT(raw_rtvs.size()),raw_rtvs.data(),nullptr);for(UINT slot=0;slot<rtvs.size();++slot)rtvs[slot].Attach(raw_rtvs[slot]);
    for(UINT slot=0;slot<rtvs.size();++slot){
        Mirror<ID3D11RenderTargetView> mirror{0,slot,rtvs[slot]};
        if(!uses(mirror.application.Get(),identity.Get()))continue;D3D11_RENDER_TARGET_VIEW_DESC desc{};mirror.application->GetDesc(&desc);check(device->CreateRenderTargetView(native,&desc,&mirror.native));p.rtvs.push_back(std::move(mirror));
    }
    const UINT slots=device->GetFeatureLevel()>=D3D_FEATURE_LEVEL_11_1?64u:8u;
    p.uav_slots=slots;
    for(UINT stage=0;stage<2;++stage){
        std::array<ID3D11UnorderedAccessView*,64> raw{};std::array<ComPtr<ID3D11UnorderedAccessView>,64> views;
        if(!stage)context->CSGetUnorderedAccessViews(0,slots,raw.data());else context->OMGetRenderTargetsAndUnorderedAccessViews(0,nullptr,nullptr,0,slots,raw.data());for(UINT slot=0;slot<slots;++slot)views[slot].Attach(raw[slot]);
        for(UINT slot=0;slot<slots;++slot){
            Mirror<ID3D11UnorderedAccessView> mirror{stage,slot,views[slot]};
            if(!uses(mirror.application.Get(),identity.Get()))continue;D3D11_UNORDERED_ACCESS_VIEW_DESC desc{};mirror.application->GetDesc(&desc);check(device->CreateUnorderedAccessView(native,&desc,&mirror.native));p.uavs.push_back(std::move(mirror));
        }
    }
    p.applied=true;p.inputs(false);p.outputs(false);
}
PresentBindings::~PresentBindings(){LastError preserve;restore();impl_.reset();}
void PresentBindings::restore() noexcept {LastError preserve;auto& p=*impl_;if(!p.applied)return;p.outputs(true);p.inputs(true);p.applied=false;}
}
