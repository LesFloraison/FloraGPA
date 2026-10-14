// Standalone diagnostic: fresh seed-specific shaders, exact integer outputs, actual loaded modules.
#include "process_modules.h"
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
#include <cstdint>
#include <cstring>
using Microsoft::WRL::ComPtr;
namespace fs=std::filesystem;
using Json=nlohmann::json;
namespace {
void require(bool value,const char* text){if(!value)throw std::runtime_error(text);}
void check(HRESULT result,const char* text){if(FAILED(result))throw std::runtime_error(std::string(text)+": "+std::to_string(uint32_t(result)));}
uint32_t number(const wchar_t* text){const std::wstring value(text);require(!value.empty()&&value.find_first_not_of(L"0123456789")==std::wstring::npos,"Expected unsigned decimal seed");const auto n=std::stoull(value);require(n&&n<=UINT32_MAX,"Seed outside uint32 range");return uint32_t(n);}
ComPtr<ID3DBlob> compile(const std::string& source,const char* profile){ComPtr<ID3DBlob> code,error;check(D3DCompile(source.data(),source.size(),"Fresh driver diagnostic",nullptr,nullptr,"main",profile,D3DCOMPILE_ENABLE_STRICTNESS,0,&code,&error),"Compile diagnostic DXBC");return code;}
void write(const fs::path& path,const void* bytes,size_t length){std::ofstream stream(path,std::ios::binary);stream.exceptions(std::ios::badbit|std::ios::failbit);stream.write(static_cast<const char*>(bytes),length);stream.close();}
}
int wmain(int argc,wchar_t** argv){
    fs::path output;Json report={{"completed",false},{"acceptance_established",false}};
    try{
        require(argc==3,"Expected NEW_OUT SEED");const auto seed=number(argv[2]);const auto requested=fs::absolute(argv[1]);require(!fs::exists(requested),"Output already exists");require(fs::create_directory(requested),"Cannot create output directory");output=requested;
        ComPtr<IDXGIFactory1> factory;check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"Create factory");ComPtr<IDXGIAdapter1> selected;DXGI_ADAPTER_DESC1 description{};
        for(UINT i=0;;++i){ComPtr<IDXGIAdapter1> adapter;const auto hr=factory->EnumAdapters1(i,&adapter);if(hr==DXGI_ERROR_NOT_FOUND)break;check(hr,"Enumerate adapter");DXGI_ADAPTER_DESC1 desc{};check(adapter->GetDesc1(&desc),"Describe adapter");if(desc.VendorId==32902){selected=adapter;description=desc;break;}}
        require(bool(selected),"Intel adapter unavailable");ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;D3D_FEATURE_LEVEL level{};const D3D_FEATURE_LEVEL requestedLevel=D3D_FEATURE_LEVEL_11_1;
        check(D3D11CreateDevice(selected.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,0,&requestedLevel,1,D3D11_SDK_VERSION,&device,&level,&context),"Create Intel feature-level 11.1 device");
        const auto vsCode=compile("float4 main(uint i:SV_VertexID):SV_Position{return float4(i==2?3:-1,i==1?3:-1,0,1);}","vs_4_0");
        const auto psCode=compile("uint4 main(float4 p:SV_Position):SV_Target{uint2 v=uint2(p.xy);return uint4(v.x+"+std::to_string(seed)+"u,v.y^"+std::to_string(seed)+"u,v.x*"+std::to_string(seed|1u)+"u+v.y,0xcafebabeu);}","ps_4_0");
        write(output/"vertex.dxbc",vsCode->GetBufferPointer(),vsCode->GetBufferSize());write(output/"pixel.dxbc",psCode->GetBufferPointer(),psCode->GetBufferSize());
        ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11PixelShader> ps;check(device->CreateVertexShader(vsCode->GetBufferPointer(),vsCode->GetBufferSize(),nullptr,&vs),"Create VS");check(device->CreatePixelShader(psCode->GetBufferPointer(),psCode->GetBufferSize(),nullptr,&ps),"Create PS");
        D3D11_TEXTURE2D_DESC td{13,7,1,1,DXGI_FORMAT_R32G32B32A32_UINT,{1,0},D3D11_USAGE_DEFAULT,D3D11_BIND_RENDER_TARGET,0,0};ComPtr<ID3D11Texture2D> target,staging;check(device->CreateTexture2D(&td,nullptr,&target),"Create target");ComPtr<ID3D11RenderTargetView> rtv;check(device->CreateRenderTargetView(target.Get(),nullptr,&rtv),"Create RTV");
        td.Usage=D3D11_USAGE_STAGING;td.BindFlags=0;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;check(device->CreateTexture2D(&td,nullptr,&staging),"Create staging");
        context->VSSetShader(vs.Get(),nullptr,0);context->PSSetShader(ps.Get(),nullptr,0);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);const D3D11_VIEWPORT viewport{0,0,13,7,0,1};context->RSSetViewports(1,&viewport);
        D3D11_RASTERIZER_DESC rd{};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=TRUE;ComPtr<ID3D11RasterizerState> raster;check(device->CreateRasterizerState(&rd,&raster),"Create rasterizer");context->RSSetState(raster.Get());auto view=rtv.Get();context->OMSetRenderTargets(1,&view,nullptr);context->Draw(3,0);context->CopyResource(staging.Get(),target.Get());
        std::vector<uint8_t> pixels(13*7*16);D3D11_MAPPED_SUBRESOURCE mapped{};check(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped),"Map output");const bool valid=mapped.pData&&mapped.RowPitch>=13*16;
        if(valid)for(unsigned y=0;y<7;++y)std::memcpy(pixels.data()+y*13*16,static_cast<const uint8_t*>(mapped.pData)+size_t(y)*mapped.RowPitch,13*16);context->Unmap(staging.Get(),0);require(valid,"Invalid output row pitch");
        for(uint32_t y=0;y<7;++y)for(uint32_t x=0;x<13;++x){const uint32_t expected[]{x+seed,y^seed,x*(seed|1u)+y,0xcafebabeu};require(!std::memcmp(pixels.data()+(y*13+x)*16,expected,sizeof expected),"Rendered integer result differs from CPU oracle");}
        check(device->GetDeviceRemovedReason(),"Device status");write(output/"output.rgba32u",pixels.data(),pixels.size());
        report.update({{"seed",seed},{"vendor",description.VendorId},{"device",description.DeviceId},{"feature_level",uint32_t(level)},{"creation_flags",device->GetCreationFlags()},{"width",13},{"height",7},{"exact_texels",91},{"loaded_modules",flora::diagnostic::processModules()},{"completed",true}});
        const auto json=report.dump(2);write(output/"report.json",json.data(),json.size());std::cout<<report.dump()<<'\n';return 0;
    }catch(const std::exception& e){report["error"]=e.what();if(!output.empty()){const auto json=report.dump(2);try{write(output/"report.json",json.data(),json.size());}catch(...){}}std::cerr<<report.dump()<<'\n';return 1;}
}
