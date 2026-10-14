// Independent native D32S8 per-sample producer and exact CPU oracle.
#include "texel_checkpoint.h"
#include <bit>
#include <d3d11sdklayers.h>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
using namespace flora;
using Json=nlohmann::json;
namespace fs=std::filesystem;
namespace {
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
Com<ID3DBlob> compile(const std::string& text,const char* profile){Com<ID3DBlob> code,error;check(D3DCompile(text.data(),text.size(),"Depth checkpoint oracle",nullptr,nullptr,"main",profile,D3DCOMPILE_ENABLE_STRICTNESS,0,&code,&error),"Compile oracle");return code;}
}
int wmain(int argc,wchar_t** argv){
    try{
        require(argc==2,"Expected new report file");const fs::path output(argv[1]);require(!fs::exists(output),"Report already exists");
        Json report={{"completed",false},{"cases",Json::array()}};
        for(const bool warp:{true,false}){
            Com<IDXGIFactory1> factory;check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"Create test factory");
            Com<IDXGIAdapter1> selected;
            if(!warp)for(UINT i=0;;++i){Com<IDXGIAdapter1> adapter;if(factory->EnumAdapters1(i,&adapter)==DXGI_ERROR_NOT_FOUND)break;DXGI_ADAPTER_DESC1 desc{};check(adapter->GetDesc1(&desc),"Get test adapter");if(desc.VendorId==32902){selected=adapter;break;}}
            require(warp||selected,"Intel test adapter unavailable");
            Com<ID3D11Device> device;Com<ID3D11DeviceContext> context;
            check(D3D11CreateDevice(selected.Get(),warp?D3D_DRIVER_TYPE_WARP:D3D_DRIVER_TYPE_UNKNOWN,nullptr,D3D11_CREATE_DEVICE_DEBUG,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context),"Create test device");
            Com<ID3D11InfoQueue> diagnostics;check(device.As(&diagnostics),"Get test diagnostics");
            for(const bool typeless:{false,true})for(const UINT samples:{2u,4u}){
                context->ClearState();diagnostics->ClearStoredMessages();
                D3D11_TEXTURE2D_DESC td{5,3,1,2,typeless?DXGI_FORMAT_R32G8X24_TYPELESS:DXGI_FORMAT_D32_FLOAT_S8X24_UINT,{samples,0},D3D11_USAGE_DEFAULT,D3D11_BIND_DEPTH_STENCIL,0,0};
                Com<ID3D11Texture2D> source;check(device->CreateTexture2D(&td,nullptr,&source),"Create depth oracle");
                std::array<Com<ID3D11DepthStencilView>,2> views;
                for(UINT layer=0;layer<2;++layer){D3D11_DEPTH_STENCIL_VIEW_DESC vd{};vd.Format=DXGI_FORMAT_D32_FLOAT_S8X24_UINT;vd.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2DMSARRAY;vd.Texture2DMSArray={layer,1};check(device->CreateDepthStencilView(source.Get(),&vd,&views[layer]),"Create oracle DSV");context->ClearDepthStencilView(views[layer].Get(),D3D11_CLEAR_DEPTH|D3D11_CLEAR_STENCIL,0,0);}
                const auto vsCode=compile("float4 main(uint i:SV_VertexID):SV_Position{return float4(i==2?3:-1,i==1?3:-1,0,1);}","vs_5_0");
                Com<ID3D11VertexShader> vs;check(device->CreateVertexShader(vsCode->GetBufferPointer(),vsCode->GetBufferSize(),nullptr,&vs),"Create oracle VS");
                context->VSSetShader(vs.Get(),nullptr,0);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                D3D11_VIEWPORT viewport{0,0,5,3,0,1};context->RSSetViewports(1,&viewport);
                D3D11_RASTERIZER_DESC rd{};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=TRUE;rd.MultisampleEnable=TRUE;
                Com<ID3D11RasterizerState> raster;check(device->CreateRasterizerState(&rd,&raster),"Create oracle rasterizer");context->RSSetState(raster.Get());
                D3D11_DEPTH_STENCIL_DESC dd{};dd.DepthEnable=TRUE;dd.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;dd.DepthFunc=D3D11_COMPARISON_ALWAYS;
                dd.StencilEnable=TRUE;dd.StencilReadMask=dd.StencilWriteMask=255;dd.FrontFace={D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_REPLACE,D3D11_COMPARISON_ALWAYS};dd.BackFace=dd.FrontFace;
                Com<ID3D11DepthStencilState> depth;check(device->CreateDepthStencilState(&dd,&depth),"Create oracle depth state");
                for(UINT layer=0;layer<2;++layer)for(UINT sample=0;sample<samples;++sample){
                    const auto psCode=compile("float main():SV_Depth{return "+std::to_string(sample+layer*4+1)+".0/16.0;}","ps_5_0");
                    Com<ID3D11PixelShader> ps;check(device->CreatePixelShader(psCode->GetBufferPointer(),psCode->GetBufferSize(),nullptr,&ps),"Create oracle PS");
                    context->PSSetShader(ps.Get(),nullptr,0);context->OMSetRenderTargets(0,nullptr,views[layer].Get());context->OMSetBlendState(nullptr,nullptr,1u<<sample);context->OMSetDepthStencilState(depth.Get(),layer*61+sample*17);context->Draw(3,0);
                }
                const auto before=diagnostic::checkpoint(device.Get(),context.Get(),source.Get(),20);
                for(const auto& view:views)context->ClearDepthStencilView(view.Get(),D3D11_CLEAR_DEPTH|D3D11_CLEAR_STENCIL,0.875f,250);
                const auto after=diagnostic::checkpoint(device.Get(),context.Get(),source.Get(),20);
                unsigned checks=0;
                for(UINT epoch=0;epoch<2;++epoch)for(UINT layer=0;layer<2;++layer)for(UINT sample=0;sample<samples;++sample)for(const auto point:{std::array<UINT,2>{0,0},std::array<UINT,2>{4,2}}){
                    const auto words=diagnostic::readCheckpoint(device.Get(),epoch?after.Get():before.Get(),20,sample,layer,point[0],point[1]);
                    const float expected=epoch?0.875f:float(sample+layer*4+1)/16;
                    require(words==std::array<uint32_t,4>{std::bit_cast<uint32_t>(expected),epoch?250u:layer*61+sample*17,0,0},"Depth/stencil differs from CPU oracle");++checks;
                }
                Com<ID3D11DepthStencilView> actual;context->OMGetRenderTargets(0,nullptr,&actual);require(actual.Get()==views[1].Get(),"Checkpoint lost DSV binding");
                UINT mask{};context->OMGetBlendState(nullptr,nullptr,&mask);require(mask==(1u<<(samples-1)),"Checkpoint lost sample mask");
                Com<ID3D11DepthStencilState> actualDepth;UINT ref{};context->OMGetDepthStencilState(&actualDepth,&ref);require(actualDepth.Get()==depth.Get()&&ref==61+(samples-1)*17,"Checkpoint lost depth state");
                unsigned refusals=0;
                const auto refuses=[&](auto action,const char* expected){try{action();}catch(const std::exception& e){require(std::string(e.what()).find(expected)!=std::string::npos,"Unexpected refusal");++refusals;return;}throw std::runtime_error("Missing refusal");};
                refuses([&]{diagnostic::checkpoint(device.Get(),context.Get(),source.Get(),30);},"Incompatible checkpoint source");
                refuses([&]{diagnostic::checkpoint(device.Get(),context.Get(),source.Get(),0);},"Unsupported checkpoint format");
                refuses([&]{diagnostic::readCheckpoint(device.Get(),before.Get(),20,samples,0,0,0);},"Invalid checkpoint texel selection");
                refuses([&]{diagnostic::readCheckpoint(device.Get(),before.Get(),20,0,2,0,0);},"Invalid checkpoint texel selection");
                refuses([&]{diagnostic::readCheckpoint(device.Get(),before.Get(),20,0,0,5,0);},"Invalid checkpoint texel selection");
                require(!diagnostics->GetNumMessagesDiscardedByMessageCountLimit(),"Lost debug messages");
                for(UINT64 i=0;i<diagnostics->GetNumStoredMessagesAllowedByRetrievalFilter();++i){SIZE_T size=0;check(diagnostics->GetMessage(i,nullptr,&size),"Measure debug message");std::vector<uint8_t> bytes(size);auto* message=reinterpret_cast<D3D11_MESSAGE*>(bytes.data());check(diagnostics->GetMessage(i,message,&size),"Read debug message");require(message->Severity>D3D11_MESSAGE_SEVERITY_WARNING,message->pDescription);}
                report["cases"].push_back({{"warp",warp},{"typeless",typeless},{"samples",samples},{"exact_texels",checks},{"refusals",refusals},{"debug_warnings_or_errors",0}});
            }
        }
        report["completed"]=true;std::ofstream stream(output);stream.exceptions(std::ios::badbit|std::ios::failbit);stream<<report.dump(2)<<'\n';stream.close();std::cout<<report.dump()<<'\n';return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
