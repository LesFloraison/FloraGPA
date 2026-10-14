// Native CPU-oracle verification of mirrored float outputs, fragment counts and restoration.
#include "pipeline_checkpoint.h"
#include "fragment_checkpoint.h"
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
            for(const UINT samples:{2u,4u})for(const unsigned mode:{0u,1u,2u,3u,4u,5u}){
                const bool half=mode==1,countOnly=mode>=2,bindingOnly=mode==3,masked=mode==4;
                const bool unusedOutput=mode==5;
                context->ClearState();debug->ClearStoredMessages();std::map<Id,Com<IUnknown>> objects;
                std::array<Com<ID3D11Texture2D>,3> textures;std::array<Com<ID3D11RenderTargetView>,3> targets;
                for(unsigned i=0;i<3;++i){D3D11_TEXTURE2D_DESC td{1,1,1,1,DXGI_FORMAT_R16G16B16A16_TYPELESS,{samples,0},D3D11_USAGE_DEFAULT,D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE,0,0};check(device->CreateTexture2D(&td,nullptr,&textures[i]),"Create oracle target");D3D11_RENDER_TARGET_VIEW_DESC vd{};vd.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;vd.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE2DMS;check(device->CreateRenderTargetView(textures[i].Get(),&vd,&targets[i]),"Create oracle RTV");objects[10+i*2]=textures[i];objects[11+i*2]=targets[i];}
                ID3D11RenderTargetView* raw[]{targets[0].Get(),targets[1].Get(),targets[2].Get()};context->OMSetRenderTargets(3,raw,nullptr);
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
                const auto mirrored=bindingOnly?pc:compile(countOnly?"struct O{float4 a:SV_Target0;float4 b:SV_Target1;float4 d:SV_Target3;};O main(){O o;o.a=float4(0.5,0.25,0.125,0.5);o.b=o.a;o.d=1;return o;}":"struct O{float4 a:SV_Target0;float4 b:SV_Target1;float4 c:SV_Target2;float4 d:SV_Target3;};O main(){O o;o.a=float4(0.5,0.25,0.125,0.5);o.b=o.c=o.a;o.d=1;return o;}","ps_4_0");
                const auto refuses=[&](auto action,const char* expected){try{action();}catch(const std::exception& e){require(std::string(e.what()).find(expected)!=std::string::npos,"Unexpected refusal");++refusals;return;}throw std::runtime_error("Missing refusal");};
                for(unsigned phase=0;phase<3;++phase){
                    const float initial[]{0.25f,0.5f,0.75f,1};for(const auto& target:targets)context->ClearRenderTargetView(target.Get(),initial);
                    diagnostic::FragmentCheckpoint observation;
                    {
                        if(phase==1){observation.apply(device.Get(),context.Get(),Bytes(static_cast<const uint8_t*>(mirrored->GetBufferPointer()),mirrored->GetBufferSize()),half,countOnly,masked);refuses([&]{observation.apply(device.Get(),context.Get(),{});},"cannot be reused");Com<ID3D11BlendState> applied;context->OMGetBlendState(&applied,nullptr,nullptr);D3D11_BLEND_DESC observed{};applied->GetDesc(&observed);require(observed.RenderTarget[3].RenderTargetWriteMask==(masked?0:15),"Counter write mask differs from request");context->Draw(3,0);}
                        {diagnostic::ScopedPixelShader shader;
                            if(phase==2){
                                const auto selectedCode=unusedOutput?mirrored:pc;
                                shader.apply(device.Get(),context.Get(),Bytes(static_cast<const uint8_t*>(selectedCode->GetBufferPointer()),selectedCode->GetBufferSize()));
                                refuses([&]{shader.apply(device.Get(),context.Get(),{});},"already active");
                                if(unusedOutput){
                                    Com<ID3D11PixelShader> active;context->PSGetShader(&active,nullptr,nullptr);objects[6]=active;
                                    auto applied=diagnostic::pipelineCheckpoint(context.Get(),objects);
                                    require(applied["shaders"]["ps"]==6,"Expected replacement PS identity");applied["shaders"]["ps"]=2;
                                    require(applied==baseline,"Shader-only replacement changed non-PS pipeline state");objects.erase(6);
                                }
                            }
                            context->Draw(3,0);
                        }
                        observation.restore();
                    }
                    if(phase==1)for(UINT sample=0;sample<samples;++sample){
                        const auto expectedColor=sample%2?std::array<uint32_t,4>{}:half?std::array<uint32_t,4>{0x3800,0x3400,0x3000,0x3800}:std::array<uint32_t,4>{0x3f000000,0x3e800000,0x3e000000,0x3f000000};
                        const auto expectedCount=(sample%2||bindingOnly||masked)?std::array<uint32_t,4>{}:std::array<uint32_t,4>{0x40000000,0x40000000,0x40000000,0x40000000};
                        if(!countOnly){require(diagnostic::readCheckpoint(device.Get(),observation.textures[0].Get(),half?12u:3u,sample,0,0,0)==expectedColor,"Mirrored float output differs from CPU oracle");++checks;}else require(!observation.textures[0],"Counter-only mode allocated a color mirror");
                        require(diagnostic::readCheckpoint(device.Get(),observation.textures[1].Get(),3,sample,0,0,0)==expectedCount,"Fragment count differs from CPU oracle");++checks;
                    }
                    require(diagnostic::pipelineCheckpoint(context.Get(),objects)==baseline,"Override failed to restore native bindings");
                    for(unsigned target=0;target<3;++target)for(UINT sample=0;sample<samples;++sample){
                        auto expected=(sample%2||target==2)?std::array<uint32_t,4>{0x3400,0x3800,0x3a00,0x3c00}:
                            target?(phase==1?std::array<uint32_t,4>{0x3d00,0x3c00,0x3c00,0x4000}:std::array<uint32_t,4>{0x3a00,0x3a00,0x3b00,0x3e00}):
                            phase==1?std::array<uint32_t,4>{0x3700,0x3500,0x3480,0x3900}:
                                     std::array<uint32_t,4>{0x3600,0x3600,0x3700,0x3a00};
                        require(diagnostic::readCheckpoint(device.Get(),textures[target].Get(),12,sample,0,0,0)==expected,"Blend output differs from CPU oracle");++checks;
                    }
                    require(diagnostic::pipelineCheckpoint(context.Get(),objects)==baseline,"Readback failed to restore native bindings");
                }
                refuses([&]{diagnostic::pipelineCheckpoint(context.Get(),{});},"no executed identity");
                refuses([&]{diagnostic::mirrorFragmentShader(Bytes(static_cast<const uint8_t*>(pc->GetBufferPointer()),pc->GetBufferSize()));},"verified GF2 pixel shader");
                context->OMSetBlendState(nullptr,factors,0x55555555);refuses([&]{diagnostic::FragmentCheckpoint observation;observation.apply(device.Get(),context.Get(),{});},"explicit blend state");context->OMSetBlendState(blend.Get(),factors,0x55555555);
                D3D11_TEXTURE2D_DESC ud{1,1,1,1,DXGI_FORMAT_R32_UINT,{1,0},D3D11_USAGE_DEFAULT,D3D11_BIND_UNORDERED_ACCESS,0,0};
                Com<ID3D11Texture2D> uavTexture;check(device->CreateTexture2D(&ud,nullptr,&uavTexture),"Create bound UAV refusal texture");Com<ID3D11UnorderedAccessView> uav;check(device->CreateUnorderedAccessView(uavTexture.Get(),nullptr,&uav),"Create bound UAV refusal view");auto rawUav=uav.Get();
                context->OMSetRenderTargetsAndUnorderedAccessViews(D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL,nullptr,nullptr,4,1,&rawUav,nullptr);
                refuses([&]{diagnostic::FragmentCheckpoint observation;observation.apply(device.Get(),context.Get(),{});},"bound OM UAVs");rawUav=nullptr;context->OMSetRenderTargetsAndUnorderedAccessViews(D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL,nullptr,nullptr,4,1,&rawUav,nullptr);
                require(!debug->GetNumMessagesDiscardedByMessageCountLimit(),"Lost debug messages");
                unsigned expectedWarnings=0;Json expectedMessages=Json::array();
                for(UINT64 i=0;i<debug->GetNumStoredMessagesAllowedByRetrievalFilter();++i){
                    SIZE_T size=0;check(debug->GetMessage(i,nullptr,&size),"Measure debug message");std::vector<uint8_t> bytes(size);auto* message=reinterpret_cast<D3D11_MESSAGE*>(bytes.data());check(debug->GetMessage(i,message,&size),"Read debug message");
                    if(unusedOutput&&diagnostic::unusedOutputWarning(message->Severity,message->ID,message->pDescription)){
                        ++expectedWarnings;expectedMessages.push_back({{"severity",message->Severity},{"id",message->ID},{"text",message->pDescription}});
                    }else require(message->Severity>D3D11_MESSAGE_SEVERITY_WARNING,message->pDescription);
                }
                require(expectedWarnings==(unusedOutput?1u:0u),"Unexpected number of unbound output warnings");
                require(!diagnostic::unusedOutputWarning(D3D11_MESSAGE_SEVERITY_ERROR,D3D11_MESSAGE_ID_DEVICE_DRAW_RENDERTARGETVIEW_NOT_SET,"slot 3"),"Error accepted as expected warning");
                require(!diagnostic::unusedOutputWarning(D3D11_MESSAGE_SEVERITY_WARNING,D3D11_MESSAGE_ID_UNKNOWN,"slot 3"),"Wrong diagnostic ID accepted");
                require(!diagnostic::unusedOutputWarning(D3D11_MESSAGE_SEVERITY_WARNING,D3D11_MESSAGE_ID_DEVICE_DRAW_RENDERTARGETVIEW_NOT_SET,"slot 2"),"Wrong slot accepted");
                report["cases"].push_back({{"warp",warp},{"samples",samples},{"half_mirror",half},{"count_only",countOnly},{"binding_only",bindingOnly},{"masked_counter",masked},{"exact_texels",checks},{"refusals",refusals},{"debug_warnings_or_errors",expectedWarnings},{"unexpected_debug_warnings_or_errors",0},{"pipeline",baseline}});
                report["cases"].back()["unused_shader_output"]=unusedOutput;
                report["cases"].back()["expected_unbound_output_warnings"]=expectedMessages;
            }
        }
        report["completed"]=true;std::ofstream stream(output);stream.exceptions(std::ios::badbit|std::ios::failbit);stream<<report.dump(2)<<'\n';stream.close();std::cout<<"Native fragment checkpoint checks passed\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
