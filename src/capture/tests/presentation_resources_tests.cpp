#include "presentation_resources.hpp"
#include "objects.hpp"
#include "resource_data.hpp"
#include <d3d11sdklayers.h>
#include <dxgi1_4.h>
#include <iostream>
#include <vector>
using namespace flora::capture;
using Microsoft::WRL::ComPtr;
void check(HRESULT hr){if(FAILED(hr))throw std::runtime_error("DX11 HRESULT "+std::to_string(uint32_t(hr)));}
void require(bool good,const char* text){if(!good)throw std::runtime_error(text);}
std::vector<int> diagnostics(ID3D11InfoQueue* info){std::vector<int> result;for(UINT64 i=0;i<info->GetNumStoredMessages();++i){SIZE_T size=0;check(info->GetMessage(i,nullptr,&size));std::vector<uint8_t> data(size);auto* message=reinterpret_cast<D3D11_MESSAGE*>(data.data());check(info->GetMessage(i,message,&size));if(message->Severity<=D3D11_MESSAGE_SEVERITY_WARNING)result.push_back(int(message->ID));}return result;}
LRESULT CALLBACK proc(HWND w,UINT m,WPARAM a,LPARAM b){return DefWindowProcW(w,m,a,b);}
int main(){try{
    WNDCLASSW cls{};cls.lpfnWndProc=proc;cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=L"FloraPresentationIdentity";require(RegisterClassW(&cls)!=0,"Window class failed");
    HWND window=CreateWindowW(cls.lpszClassName,L"Flora resource identity test",WS_OVERLAPPEDWINDOW,100,100,160,160,nullptr,nullptr,cls.hInstance,nullptr);require(window!=nullptr,"Window failed");
    DXGI_SWAP_CHAIN_DESC d{};d.BufferDesc.Width=d.BufferDesc.Height=64;d.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;d.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT|DXGI_USAGE_SHADER_INPUT;d.SampleDesc.Count=1;d.BufferCount=2;d.SwapEffect=DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;d.OutputWindow=window;d.Windowed=TRUE;
    ComPtr<ID3D11Device> native_device;ComPtr<ID3D11DeviceContext> native_context;ComPtr<IDXGISwapChain> swap;
    check(D3D11CreateDeviceAndSwapChain(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,D3D11_CREATE_DEVICE_DEBUG,nullptr,0,D3D11_SDK_VERSION,&d,&swap,&native_device,nullptr,&native_context));
    ComPtr<ID3D11InfoQueue> info;check(native_device.As(&info));
    ComPtr<ID3D11Texture2D> original,clean;check(swap->GetBuffer(0,IID_PPV_ARGS(&original)));D3D11_TEXTURE2D_DESC td{};original->GetDesc(&td);check(native_device->CreateTexture2D(&td,nullptr,&clean));
    std::vector<uint32_t> red(64*64,0xff0000ffu),blue(64*64,0xffff0000u);
    native_context->UpdateSubresource(original.Get(),0,nullptr,red.data(),64*4,0);native_context->UpdateSubresource(clean.Get(),0,nullptr,blue.data(),64*4,0);
    auto registration=std::make_unique<PresentationResource>(original.Get(),clean.Get());
    ComPtr<ID3D11Device> device=native_device;wrap_object(device.GetAddressOf(),"Presentation test device");
    ComPtr<ID3D11DeviceContext> context=native_context;wrap_object(context.GetAddressOf(),"Presentation test context");
    // Later logical slots have native read-only restrictions, even though
    // their backing clean textures themselves are ordinary writable textures.
    {ComPtr<ID3D11Texture2D> other,other_clean;check(swap->GetBuffer(1,IID_PPV_ARGS(&other)));D3D11_TEXTURE2D_DESC other_desc{};other->GetDesc(&other_desc);check(native_device->CreateTexture2D(&other_desc,nullptr,&other_clean));
        PresentationResource readonly(other.Get(),other_clean.Get());ComPtr<ID3D11RenderTargetView> rejected;require(diagnostics(info.Get()).empty(),"Unexpected initialization diagnostics");info->ClearStoredMessages();const auto expected=native_device->CreateRenderTargetView(other.Get(),nullptr,&rejected);require(FAILED(expected),"Native later slot unexpectedly writable");const auto expected_messages=diagnostics(info.Get());require(!expected_messages.empty(),"Native invalid-view diagnostic missing");info->ClearStoredMessages();
        wrap_object(other.GetAddressOf(),"Presentation read-only buffer");const auto actual=device->CreateRenderTargetView(other.Get(),nullptr,&rejected);require(actual==expected && !rejected,"Alias removed native read-only view restriction");require(diagnostics(info.Get())==expected_messages,"Read-only view diagnostics differ from native");info->ClearStoredMessages();}
    ComPtr<ID3D11Texture2D> application=original;wrap_object(application.GetAddressOf(),"Presentation test buffer");
    const auto resource_record=object_record(application.Get());require(resource_record!=nullptr,"Application buffer record missing");
    ComPtr<IUnknown> public_identity;check(application.As(&public_identity));
    ComPtr<IDXGIResource> dxgi;check(application.As(&dxgi));DXGI_USAGE usage=0,native_usage=0;check(dxgi->GetUsage(&usage));{ComPtr<IDXGIResource> r;check(original.As(&r));check(r->GetUsage(&native_usage));}require(usage==native_usage && (usage&DXGI_USAGE_BACK_BUFFER),"Public buffer lost DXGI usage");
    ComPtr<IUnknown> dxgi_identity;check(dxgi.As(&dxgi_identity));require(dxgi_identity==public_identity,"Cross-interface identity changed");
    require(unwrap(application.Get())==clean.Get(),"GPU operands do not use clean content");
    ComPtr<ID3D11RenderTargetView> view;check(device->CreateRenderTargetView(application.Get(),nullptr,&view));
    ComPtr<ID3D11Resource> resource;view->GetResource(&resource);ComPtr<IUnknown> returned_identity;check(resource.As(&returned_identity));require(returned_identity==public_identity,"View getter leaked clean resource identity");
    require(object_record(view.Get())->resource==resource_record->id,"View metadata references a private clone");
    auto* raw=view.Get();context->OMSetRenderTargets(1,&raw,nullptr);ComPtr<ID3D11RenderTargetView> bound;context->OMGetRenderTargets(1,&bound,nullptr);require(bound==view,"Context getter changed view identity");
    const auto snapshot=snapshot_resource(context.Get(),resource.Get());require(snapshot.record->id==resource_record->id && snapshot.subresources.size()==1,"Snapshot resource graph changed");
    require(snapshot.subresources[0].bytes.size()==blue.size()*4 && memcmp(snapshot.subresources[0].bytes.data(),blue.data(),blue.size()*4)==0,"Snapshot read decorated native pixels");
    ComPtr<IUnknown> clean_identity;check(clean.As(&clean_identity));require(find_native_record(clean_identity.Get())==resource_record,"Native clean lookup created a second record");
    const auto inventory=metadata_status();require(inventory["kinds"].value("texture2d1",0)+inventory["kinds"].value("texture2d",0)==2,"Private clean texture entered metadata inventory");
    // A native view must retain the real backbuffer after the proxy and alias
    // registration disappear, so DXGI itself enforces indirect Resize refs.
    ComPtr<ID3D11RenderTargetView> retained=unwrap(view.Get());context->ClearState();bound.Reset();view.Reset();resource.Reset();returned_identity.Reset();dxgi_identity.Reset();dxgi.Reset();public_identity.Reset();application.Reset();original.Reset();clean_identity.Reset();registration.reset();
    require(diagnostics(info.Get()).empty(),"Unexpected identity/snapshot diagnostics");info->ClearStoredMessages();
    const auto held=swap->ResizeBuffers(2,64,64,DXGI_FORMAT_UNKNOWN,0);require(held==DXGI_ERROR_INVALID_CALL,"Native view no longer blocks ResizeBuffers");
    // The preceding native DXGI rejection is intentional. Separate later
    // unexpected diagnostics from the failure-path diagnostic.
    info->ClearStoredMessages();retained.Reset();native_context->Flush();
    check(swap->ResizeBuffers(2,64,64,DXGI_FORMAT_UNKNOWN,0));
    require(!resource_record->alive,"Original resource lifetime did not end on Resize");
    require(object_status().value("interface_gaps",0ull)==0,"Presentation alias reported an interface gap");
    for(UINT64 i=0;i<info->GetNumStoredMessages();++i){SIZE_T size=0;check(info->GetMessage(i,nullptr,&size));std::vector<uint8_t> data(size);auto* message=reinterpret_cast<D3D11_MESSAGE*>(data.data());check(info->GetMessage(i,message,&size));if(message->Severity<=D3D11_MESSAGE_SEVERITY_WARNING)throw std::runtime_error(message->pDescription);}
    DestroyWindow(window);std::cout<<"Native presentation identity, getters, snapshot bytes, metadata and indirect Resize references passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
