#include "presentation_storage.hpp"
#include "presentation_resources.hpp"
#include "clean_buffer_ring.hpp"
#include "present_bindings.hpp"
#include <vector>
#include <stdexcept>
namespace flora::capture {
using Microsoft::WRL::ComPtr;
namespace {void check(HRESULT hr){if(FAILED(hr))throw std::runtime_error("Cannot access native presentation storage");}}
struct PresentationStorage::Impl {
    hud::CleanBufferRing clean;
    ComPtr<ID3D11DeviceContext> context;
    std::vector<std::unique_ptr<PresentationResource>> aliases;
    std::vector<std::unique_ptr<hud::PresentBindings>> bindings;
    explicit Impl(IDXGISwapChain* swap):clean(swap){ComPtr<ID3D11Device> device;check(swap->GetDevice(IID_PPV_ARGS(&device)));device->GetImmediateContext(&context);}
};
PresentationStorage::PresentationStorage(IDXGISwapChain* swap):impl_(std::make_unique<Impl>(swap)){attach(swap);}
PresentationStorage::~PresentationStorage(){
    const DWORD error=GetLastError();
    // Released clean views can remain in D3D11's deferred-destruction queue.
    // Their private parent links retain real swap buffers, so drain that queue
    // after releasing our storage. Do not ClearState: application bindings and
    // externally held views must keep their normal lifetimes. The caller owns
    // the device's presentation gate during teardown and Resize.
    auto context=impl_->context;
    impl_.reset();
    context->Flush();
    context.Reset();
    SetLastError(error);
}
void PresentationStorage::detach(){
    const DWORD error=GetLastError();
    impl_->bindings.clear();impl_->aliases.clear();
    // A released clean RTV can still retain its real-buffer parent until
    // deferred destruction runs. Resize must not see that private reference.
    // Live application views remain alive; Flush does not unbind their state.
    impl_->context->Flush();
    SetLastError(error);
}
void PresentationStorage::attach(IDXGISwapChain* swap){
    auto& p=*impl_;if(!p.aliases.empty())throw std::logic_error("Presentation aliases already attached");
    DXGI_SWAP_CHAIN_DESC desc{};check(swap->GetDesc(&desc));
    std::vector<std::unique_ptr<PresentationResource>> aliases;
    const UINT count=desc.SwapEffect==DXGI_SWAP_EFFECT_FLIP_DISCARD?1:p.clean.size();
    for(UINT slot=0;slot<count;++slot){ComPtr<ID3D11Texture2D> buffer;check(swap->GetBuffer(slot,IID_PPV_ARGS(&buffer)));aliases.push_back(std::make_unique<PresentationResource>(buffer.Get(),p.clean.buffer(slot)));}
    p.aliases=std::move(aliases);
}
void PresentationStorage::before_present(IDXGISwapChain* swap,UINT flags){
    if(flags&DXGI_PRESENT_TEST)return;auto& p=*impl_;
    p.clean.prepare(swap,flags);
    try{for(UINT slot=0;slot<p.aliases.size();++slot){ComPtr<ID3D11Texture2D> buffer;check(swap->GetBuffer(slot,IID_PPV_ARGS(&buffer)));p.bindings.push_back(std::make_unique<hud::PresentBindings>(p.context.Get(),buffer.Get(),p.clean.buffer(slot)));}}
    catch(...){p.bindings.clear();throw;}
}
void PresentationStorage::after_present(IDXGISwapChain* swap,UINT flags,HRESULT result){
    auto& p=*impl_;p.bindings.clear();if(flags&DXGI_PRESENT_TEST)return;
    p.clean.advance(result,flags);
    // The now-writable physical slot may differ from the submitted slot. Only
    // logical clean contents rotate; never copy a pre-Present backup into it.
    p.clean.prepare(swap,flags);
}
}
