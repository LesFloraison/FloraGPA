// Native CPU-oracle verification of a scoped RT0 blend override and state getters.
#include "pipeline_checkpoint.h"
#include "texel_checkpoint.h"
#include <d3d11sdklayers.h>
#include <fstream>
#include <iostream>
using namespace flora;
using Json=nlohmann::json;
namespace fs=std::filesystem;
namespace {
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
Com<ID3DBlob> compile(const std::string& text,const char* profile){Com<ID3DBlob> code,error;check(D3DCompile(text.data(),text.size(),"Blend override oracle",nullptr,nullptr,"main",profile,D3DCOMPILE_ENABLE_STRICTNESS,0,&code,&error),"Compile oracle");return code;}
}
int wmain(int argc,wchar_t** argv){
    try{
        require(argc==2,"Expected new report file");const fs::path output(argv[1]);require(!fs::exists(output),"Report already exists");
        Json report={{"completed",false},{"cases",Json::array()}};
        for(const bool warp:{true,false}){
            Com<IDXGIFactory1> factory;check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"Create test factory");Com<IDXGIAdapter1> selected;
            if(!warp)for(UINT i=0;;++i){Com<IDXGIAdapter1> adapter;if(factory->EnumAdapters1(i,&adapter)==DXGI_ERROR_NOT_FOUND)break;DXGI_ADAPTER_DESC1 desc{};check(adapter->GetDesc1(&desc),"Get adapter");if(desc.VendorId==32902){selected=adapter;break;}}
            require(warp||selected,"Intel adapter unavailable");Com<ID3D11Device> device;Com<ID3D11DeviceContext> context;
            check(D3D11CreateDevice(selected.Get(),warp?D3D_DRIVER_TYPE_WARP:D3D_DRIVER_TYPE_UNKNOWN,nullptr,D3D11_CREATE_DEVICE_DEBUG,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context),"Create test device");
            Com<ID3D11InfoQueue> debug;check(device.As(&debug),"Get test diagnostics");
            for(const UINT samples:{2u,4u}){
                context->ClearState();debug->ClearStoredMessages();std::map<Id,Com<IUnknown>> objects;
                std::array<Com<ID3D11Texture2D>,2> textures;std::array<Com<ID3D11RenderTargetView>,2> targets;
                for(unsigned i=0;i<2;++i){D3D11_TEXTURE2D_DESC td{1,1,1,1,DXGI_FORMAT_R16G16B16A16_TYPELESS,{samples,0},D3D11_USAGE_DEFAULT,D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE,0,0};check(device->CreateTexture2D(&td,nullptr,&textures[i]),"Create oracle target");D3D11_RENDER_TARGET_VIEW_DESC vd{};vd.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;vd.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE2DMS;check(device->CreateRenderTargetView(textures[i].Get(),&vd,&targets[i]),"Create oracle RTV");objects[10+i*2]=textures[i];objects[11+i*2]=targets[i];}
                ID3D11RenderTargetView* raw[]{targets[0].Get(),targets[1].Get()};context->OMSetRenderTargets(2,raw,nullptr);
                const auto vc=compile("float4 main(uint i:SV_VertexID):SV_Position{return float4(i==2?3:-1,i==1?3:-1,0,1);}","vs_4_0");
                const auto pc=compile("struct O{float4 a:SV_Target0;float4 b:SV_Target1;};O main(){O o;o.a=float4(0.5,0.25,0.125,0.5);o.b=o.a;return o;}","ps_4_0");
                Com<ID3D11VertexShader> vs;Com<ID3D11PixelShader> ps;check(device->CreateVertexShader(vc->GetBufferPointer(),vc->GetBufferSize(),nullptr,&vs),"Create oracle VS");check(device->CreatePixelShader(pc->GetBufferPointer(),pc->GetBufferSize(),nullptr,&ps),"Create oracle PS");objects[1]=vs;objects[2]=ps;context->VSSetShader(vs.Get(),nullptr,0);context->PSSetShader(ps.Get(),nullptr,0);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                D3D11_RASTERIZER_DESC rd{};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=rd.MultisampleEnable=TRUE;
                Com<ID3D11RasterizerState> raster;check(device->CreateRasterizerState(&rd,&raster),"Create oracle rasterizer");objects[3]=raster;context->RSSetState(raster.Get());D3D11_VIEWPORT viewport{0,0,1,1,0,1};context->RSSetViewports(1,&viewport);D3D11_RECT rect{0,0,1,1};context->RSSetScissorRects(1,&rect);
                D3D11_SAMPLER_DESC sd{};sd.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sd.ComparisonFunc=D3D11_COMPARISON_ALWAYS;sd.MaxAnisotropy=1;sd.MaxLOD=8;
                Com<ID3D11SamplerState> sampler;check(device->CreateSamplerState(&sd,&sampler),"Create oracle sampler");objects[4]=sampler;auto s=sampler.Get();context->PSSetSamplers(3,1,&s);
                D3D11_BLEND_DESC bd{};bd.IndependentBlendEnable=TRUE;
                for(auto& rt:bd.RenderTarget){rt.SrcBlend=rt.SrcBlendAlpha=D3D11_BLEND_ONE;rt.DestBlend=rt.DestBlendAlpha=D3D11_BLEND_ONE;rt.BlendOp=rt.BlendOpAlpha=D3D11_BLEND_OP_ADD;rt.RenderTargetWriteMask=15;}
                bd.RenderTarget[0].BlendEnable=bd.RenderTarget[1].BlendEnable=TRUE;bd.RenderTarget[0].SrcBlend=bd.RenderTarget[0].SrcBlendAlpha=D3D11_BLEND_SRC_ALPHA;bd.RenderTarget[0].DestBlend=bd.RenderTarget[0].DestBlendAlpha=D3D11_BLEND_INV_SRC_ALPHA;
                Com<ID3D11BlendState> blend;check(device->CreateBlendState(&bd,&blend),"Create oracle blend state");objects[5]=blend;const float factors[]{0.125f,0.25f,0.5f,0.75f};context->OMSetBlendState(blend.Get(),factors,0x55555555);
                const auto baseline=diagnostic::pipelineCheckpoint(context.Get(),objects);
                unsigned checks=0,refusals=0;
                const auto refuses=[&](auto action,const char* expected){try{action();}catch(const std::exception& e){require(std::string(e.what()).find(expected)!=std::string::npos,"Unexpected refusal");++refusals;return;}throw std::runtime_error("Missing refusal");};
                for(unsigned phase=0;phase<3;++phase){
                    const float initial[]{0.25f,0.5f,0.75f,1};for(const auto& target:targets)context->ClearRenderTargetView(target.Get(),initial);
                    {diagnostic::UnblendedRt0 override;
                        if(phase==1){override.apply(device.Get(),context.Get());refuses([&]{override.apply(device.Get(),context.Get());},"already active");}
                        context->Draw(3,0);
                    }
                    require(diagnostic::pipelineCheckpoint(context.Get(),objects)==baseline,"Override failed to restore native bindings");
                    for(unsigned target=0;target<2;++target)for(UINT sample=0;sample<samples;++sample){
                        auto expected=sample%2?std::array<uint32_t,4>{0x3400,0x3800,0x3a00,0x3c00}:
                            target?std::array<uint32_t,4>{0x3a00,0x3a00,0x3b00,0x3e00}:
                            phase==1?std::array<uint32_t,4>{0x3800,0x3400,0x3000,0x3800}:
                                     std::array<uint32_t,4>{0x3600,0x3600,0x3700,0x3a00};
                        require(diagnostic::readCheckpoint(device.Get(),textures[target].Get(),12,sample,0,0,0)==expected,"Blend output differs from CPU oracle");++checks;
                    }
                    require(diagnostic::pipelineCheckpoint(context.Get(),objects)==baseline,"Readback failed to restore native bindings");
                }
                refuses([&]{diagnostic::pipelineCheckpoint(context.Get(),{});},"no executed identity");
                context->OMSetBlendState(nullptr,factors,0x55555555);refuses([&]{diagnostic::UnblendedRt0 override;override.apply(device.Get(),context.Get());},"explicit independent blend");
                bd.IndependentBlendEnable=FALSE;Com<ID3D11BlendState> shared;check(device->CreateBlendState(&bd,&shared),"Create shared blend refusal");context->OMSetBlendState(shared.Get(),factors,0x55555555);refuses([&]{diagnostic::UnblendedRt0 override;override.apply(device.Get(),context.Get());},"explicit independent blend");context->OMSetBlendState(blend.Get(),factors,0x55555555);
                require(!debug->GetNumMessagesDiscardedByMessageCountLimit(),"Lost debug messages");
                for(UINT64 i=0;i<debug->GetNumStoredMessagesAllowedByRetrievalFilter();++i){SIZE_T size=0;check(debug->GetMessage(i,nullptr,&size),"Measure debug message");std::vector<uint8_t> bytes(size);auto* message=reinterpret_cast<D3D11_MESSAGE*>(bytes.data());check(debug->GetMessage(i,message,&size),"Read debug message");require(message->Severity>D3D11_MESSAGE_SEVERITY_WARNING,message->pDescription);}
                report["cases"].push_back({{"warp",warp},{"samples",samples},{"exact_texels",checks},{"refusals",refusals},{"debug_warnings_or_errors",0},{"pipeline",baseline}});
            }
        }
        report["completed"]=true;std::ofstream stream(output);stream.exceptions(std::ios::badbit|std::ios::failbit);stream<<report.dump(2)<<'\n';stream.close();std::cout<<"Native blend override checks passed\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
