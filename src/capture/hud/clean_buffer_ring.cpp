#include "clean_buffer_ring.hpp"
#include <wrl/client.h>
#include <stdexcept>
#include <vector>
namespace flora::capture::hud {
using Microsoft::WRL::ComPtr;
namespace {
void check(HRESULT result,const char* reason){if(FAILED(result))throw std::runtime_error(reason);}
struct LastError {DWORD value=GetLastError();~LastError(){SetLastError(value);}};
// CopyResource is predicated in DX11. Private presentation copies must execute
// even when the application's predicate suppresses its own GPU commands. The
// caller excludes context access; restore the identical object/value before
// native Present or any application call, without polling/changing the query.
struct CopyScope {
    ID3D11DeviceContext* context;ComPtr<ID3D11Predicate> predicate;BOOL value=FALSE;
    explicit CopyScope(ID3D11DeviceContext* c):context(c){context->GetPredication(&predicate,&value);if(predicate)context->SetPredication(nullptr,FALSE);}
    ~CopyScope(){if(predicate)context->SetPredication(predicate.Get(),value);}
};
}
struct CleanBufferRing::Impl {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    std::vector<ComPtr<ID3D11Texture2D>> buffers;
    ComPtr<ID3D11Texture2D> temporary;
    DXGI_SWAP_CHAIN_DESC descriptor{};
};
CleanBufferRing::CleanBufferRing(IDXGISwapChain* native):impl_(std::make_unique<Impl>()){
    LastError preserve;auto& p=*impl_;
    check(native->GetDesc(&p.descriptor),"Clean buffers require a swap descriptor");
    const auto& d=p.descriptor;
    if(d.BufferCount<1 || d.BufferCount>16 || d.SampleDesc.Count!=1 || d.SampleDesc.Quality ||
       (d.SwapEffect!=DXGI_SWAP_EFFECT_SEQUENTIAL && d.SwapEffect!=DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL && d.SwapEffect!=DXGI_SWAP_EFFECT_FLIP_DISCARD) ||
       (d.BufferDesc.Format!=DXGI_FORMAT_R8G8B8A8_UNORM && d.BufferDesc.Format!=DXGI_FORMAT_B8G8R8A8_UNORM && d.BufferDesc.Format!=DXGI_FORMAT_R10G10B10A2_UNORM))
        throw std::runtime_error("Unsupported clean-buffer storage profile");
    check(native->GetDevice(IID_PPV_ARGS(&p.device)),"Clean buffers require DX11");p.device->GetImmediateContext(&p.context);
    CopyScope copies(p.context.Get());
    ComPtr<ID3D11Texture2D> first;check(native->GetBuffer(0,IID_PPV_ARGS(&first)),"Clean buffers require native slot zero");
    D3D11_TEXTURE2D_DESC desc{};first->GetDesc(&desc);
    if(desc.ArraySize!=1 || desc.MipLevels!=1 || desc.MiscFlags || desc.CPUAccessFlags || desc.Usage!=D3D11_USAGE_DEFAULT)
        throw std::runtime_error("Unsupported shared, stereo or CPU-visible clean-buffer storage");
    // DXGI permits sRGB views of an UNORM swap buffer. Ordinary typed UNORM
    // textures do not; private typeless storage preserves both view formats.
    desc.Format=desc.Format==DXGI_FORMAT_R8G8B8A8_UNORM?DXGI_FORMAT_R8G8B8A8_TYPELESS:desc.Format==DXGI_FORMAT_B8G8R8A8_UNORM?DXGI_FORMAT_B8G8R8A8_TYPELESS:DXGI_FORMAT_R10G10B10A2_TYPELESS;
    p.buffers.resize(d.BufferCount);
    for(UINT slot=0;slot<d.BufferCount;++slot){
        check(p.device->CreateTexture2D(&desc,nullptr,&p.buffers[slot]),"Cannot allocate clean logical buffer");
        // Flip-discard exposes only slot zero; other contents are undefined.
        if(!slot || d.SwapEffect!=DXGI_SWAP_EFFECT_FLIP_DISCARD){
            ComPtr<ID3D11Texture2D> source;check(native->GetBuffer(slot,IID_PPV_ARGS(&source)),"Cannot initialize clean logical buffer");
            p.context->CopyResource(p.buffers[slot].Get(),source.Get());
        }
    }
    desc.BindFlags=0;check(p.device->CreateTexture2D(&desc,nullptr,&p.temporary),"Cannot allocate clean rotation storage");
}
CleanBufferRing::~CleanBufferRing()=default;
UINT CleanBufferRing::size() const noexcept{return UINT(impl_->buffers.size());}
ID3D11Texture2D* CleanBufferRing::buffer(UINT index) const{return impl_->buffers.at(index).Get();}
void CleanBufferRing::prepare(IDXGISwapChain* native,UINT flags){
    LastError preserve;if(flags&DXGI_PRESENT_TEST)return;auto& p=*impl_;
    DXGI_SWAP_CHAIN_DESC current{};check(native->GetDesc(&current),"Cannot verify clean-buffer dimensions");
    if(current.BufferCount!=p.descriptor.BufferCount || current.SwapEffect!=p.descriptor.SwapEffect ||
       current.BufferDesc.Width!=p.descriptor.BufferDesc.Width || current.BufferDesc.Height!=p.descriptor.BufferDesc.Height || current.BufferDesc.Format!=p.descriptor.BufferDesc.Format)
        throw std::runtime_error("Clean-buffer dimensions or presentation model changed");
    ComPtr<ID3D11Texture2D> destination;check(native->GetBuffer(0,IID_PPV_ARGS(&destination)),"Cannot get native presentation surface");
    CopyScope copies(p.context.Get());p.context->CopyResource(destination.Get(),p.buffers[0].Get());
    check(p.device->GetDeviceRemovedReason(),"Clean presentation copy failed");
}
void CleanBufferRing::advance(HRESULT result,UINT flags){
    LastError preserve;if(result!=S_OK || (flags&(DXGI_PRESENT_TEST|DXGI_PRESENT_DO_NOT_SEQUENCE)))return;
    auto& p=*impl_;CopyScope copies(p.context.Get());if(p.buffers.size()>1){
        p.context->CopyResource(p.temporary.Get(),p.buffers.front().Get());
        for(size_t slot=0;slot+1<p.buffers.size();++slot)p.context->CopyResource(p.buffers[slot].Get(),p.buffers[slot+1].Get());
        p.context->CopyResource(p.buffers.back().Get(),p.temporary.Get());
    }
    check(p.device->GetDeviceRemovedReason(),"Clean-buffer rotation failed");
}
}
