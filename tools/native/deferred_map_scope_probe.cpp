// Development-only native DX11 experiment. No Flora runtime or GPA dependency.
#include <d3d11.h>
#include <d3d11sdklayers.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <array>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
using Microsoft::WRL::ComPtr;
void check(HRESULT hr) { if (FAILED(hr)) throw std::runtime_error("Native DX11 call failed"); }
int wmain(int argc, wchar_t **argv) {
    try {
        bool warp=false; UINT vendor=0;
        for(int i=1;i<argc;++i) {
            const std::wstring arg=argv[i];
            if(arg==L"--warp") warp=true;
            else if(arg==L"--vendor" && i+1<argc) vendor=std::stoul(argv[++i],nullptr,0);
            else throw std::runtime_error("Unknown argument");
        }
        if(warp && vendor) throw std::runtime_error("Conflicting device options");
        ComPtr<IDXGIAdapter1> selected;
        if(vendor) {
            ComPtr<IDXGIFactory1> factory; check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
            for(UINT i=0;;++i) {
                ComPtr<IDXGIAdapter1> candidate; const auto hr=factory->EnumAdapters1(i,&candidate);
                if(hr==DXGI_ERROR_NOT_FOUND) break; check(hr);
                DXGI_ADAPTER_DESC1 desc{}; check(candidate->GetDesc1(&desc));
                if(desc.VendorId==vendor) { selected=candidate; break; }
            }
            if(!selected) throw std::runtime_error("Requested adapter is absent");
        }
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> immediate, contexts[2];
        check(D3D11CreateDevice(selected.Get(),vendor?D3D_DRIVER_TYPE_UNKNOWN:warp?D3D_DRIVER_TYPE_WARP:D3D_DRIVER_TYPE_HARDWARE,
                               nullptr,D3D11_CREATE_DEVICE_DEBUG,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&immediate));
        for(auto& c:contexts) check(device->CreateDeferredContext(0,&c));
        auto buffer=[&](D3D11_USAGE usage,UINT cpu,UINT bind) {
            D3D11_BUFFER_DESC desc{16,usage,bind,cpu,0,0};
            ComPtr<ID3D11Buffer> value; check(device->CreateBuffer(&desc,nullptr,&value)); return value;
        };
        auto source=buffer(D3D11_USAGE_DYNAMIC,D3D11_CPU_ACCESS_WRITE,D3D11_BIND_CONSTANT_BUFFER);
        auto sink=buffer(D3D11_USAGE_DEFAULT,0,D3D11_BIND_CONSTANT_BUFFER);
        auto staging=buffer(D3D11_USAGE_STAGING,D3D11_CPU_ACCESS_READ,0);
        auto upload=[&](ID3D11DeviceContext* c,UINT tag) {
            D3D11_MAPPED_SUBRESOURCE mapped{}; check(c->Map(source.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped));
            const std::array<UINT,4> values{tag,tag+1,tag+2,tag+3};
            memcpy(mapped.pData,values.data(),sizeof(values)); c->Unmap(source.Get(),0);
        };
        auto read=[&](ID3D11Buffer* b) {
            immediate->CopyResource(staging.Get(),b); D3D11_MAPPED_SUBRESOURCE mapped{};
            check(immediate->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped));
            std::array<UINT,4> values{}; memcpy(values.data(),mapped.pData,sizeof(values));
            immediate->Unmap(staging.Get(),0); return values;
        };
        upload(immediate.Get(),7);
        ComPtr<ID3D11CommandList> lists[2];
        for(UINT i=0;i<2;++i) {
            upload(contexts[i].Get(),i+1);
            contexts[i]->CopyResource(sink.Get(),source.Get());
            check(contexts[i]->FinishCommandList(FALSE,&lists[i]));
        }
        upload(immediate.Get(),9);
        const auto before=read(source.Get());
        for(UINT i=0;i<4;++i) if(before[i]!=9+i) throw std::runtime_error("Initial immediate bytes differ");
        std::cout<<"before="<<before[0]<<'\n';
        UINT step=0;
        for(UINT index: {1u,0u,0u,1u}) {
            immediate->ExecuteCommandList(lists[index].Get(),step++%2?TRUE:FALSE);
            const auto actual=read(sink.Get()), outside=read(source.Get());
            std::cout<<"list="<<index<<" inside="<<actual[0]<<" outside="<<outside[0]<<'\n';
            for(UINT i=0;i<4;++i) if(actual[i]!=index+1+i) throw std::runtime_error("Retained list bytes differ");
            if(outside!=actual) throw std::runtime_error("Global resource bytes differ from executed discard");
        }
        ComPtr<ID3D11InfoQueue> info;check(device.As(&info));
        for(UINT64 i=0;i<info->GetNumStoredMessagesAllowedByRetrievalFilter();++i){
            SIZE_T size=0;check(info->GetMessage(i,nullptr,&size));
            auto bytes=std::make_unique<unsigned char[]>(size);auto* message=reinterpret_cast<D3D11_MESSAGE*>(bytes.get());check(info->GetMessage(i,message,&size));
            if(message->Severity<=D3D11_MESSAGE_SEVERITY_WARNING){std::cerr<<message->pDescription<<'\n';throw std::runtime_error("Debug Layer warning/error");}
        }
        return 0;
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
