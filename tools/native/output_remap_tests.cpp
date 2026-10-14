#include "output_remap_checkpoint.h"
#include "pipeline_checkpoint.h"
#include <d3d11sdklayers.h>
#include <fstream>
#include <iostream>
using namespace flora;
using Json=nlohmann::json;
namespace fs=std::filesystem;
namespace {
void require(bool value,const char* text){if(!value)throw std::runtime_error(text);}
Com<ID3DBlob> compile(const std::string& text,const char* profile){
    Com<ID3DBlob> code,error;
    check(D3DCompile(text.data(),text.size(),"Output remap oracle",nullptr,nullptr,"main",profile,D3DCOMPILE_ENABLE_STRICTNESS,0,&code,&error),"Compile output remap oracle");return code;
}
Bytes bytes(const Com<ID3DBlob>& blob){return Bytes(static_cast<const uint8_t*>(blob->GetBufferPointer()),blob->GetBufferSize());}
}
int wmain(int argc,wchar_t** argv){
    fs::path output;Json report={{"completed",false},{"acceptance_established",false},{"cases",Json::array()}};
    try{
        require(argc==2,"Expected new report path");require(!fs::exists(argv[1]),"Report already exists");output=argv[1];
        for(bool warp:{true,false}){
            Com<IDXGIFactory1> factory;check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"Create factory");
            Com<IDXGIAdapter1> selected;
            if(!warp)for(UINT i=0;;++i){Com<IDXGIAdapter1> adapter;const auto hr=factory->EnumAdapters1(i,&adapter);if(hr==DXGI_ERROR_NOT_FOUND)break;check(hr,"Enumerate adapter");DXGI_ADAPTER_DESC1 desc{};check(adapter->GetDesc1(&desc),"Describe adapter");if(desc.VendorId==32902){selected=adapter;break;}}
            require(warp||selected,"Intel adapter unavailable");
            Com<ID3D11Device> device;Com<ID3D11DeviceContext> context;const D3D_FEATURE_LEVEL level=D3D_FEATURE_LEVEL_11_1;
            check(D3D11CreateDevice(selected.Get(),warp?D3D_DRIVER_TYPE_WARP:D3D_DRIVER_TYPE_UNKNOWN,nullptr,D3D11_CREATE_DEVICE_DEBUG,&level,1,D3D11_SDK_VERSION,&device,nullptr,&context),"Create oracle device");
            Com<ID3D11InfoQueue> debug;check(device.As(&debug),"Get debug queue");
            for(UINT samples:{2u,4u}){
                context->ClearState();debug->ClearStoredMessages();std::map<Id,Com<IUnknown>> objects;
                std::array<Com<ID3D11Texture2D>,4> textures;std::array<Com<ID3D11RenderTargetView>,4> views;
                for(unsigned i=0;i<4;++i){D3D11_TEXTURE2D_DESC td{1,1,1,1,DXGI_FORMAT_R16G16B16A16_TYPELESS,{samples,0},D3D11_USAGE_DEFAULT,D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE,0,0};check(device->CreateTexture2D(&td,nullptr,&textures[i]),"Create output");D3D11_RENDER_TARGET_VIEW_DESC vd{};vd.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;vd.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE2DMS;check(device->CreateRenderTargetView(textures[i].Get(),&vd,&views[i]),"Create output view");objects[10+i*2]=textures[i];objects[11+i*2]=views[i];}
                ID3D11RenderTargetView* raw[]{views[0].Get(),views[1].Get(),views[2].Get(),nullptr};context->OMSetRenderTargets(4,raw,nullptr);
                const auto vc=compile("float4 main(uint i:SV_VertexID):SV_Position{return float4(i==2?3:-1,i==1?3:-1,0,1);}","vs_4_0");
                const std::string body="O main(){O o;o.a=float4(0.5,0.25,0.125,0.5);o.b=float4(0.125,0.5,0.25,0.75);return o;}";
                const auto pc=compile("struct O{float4 a:SV_Target0;float4 b:SV_Target1;};"+body,"ps_4_0");
                const auto remapped=compile("struct O{float4 a:SV_Target0;float4 b:SV_Target3;};"+body,"ps_4_0");
                Com<ID3D11VertexShader> vs;Com<ID3D11PixelShader> ps;
                check(device->CreateVertexShader(vc->GetBufferPointer(),vc->GetBufferSize(),nullptr,&vs),"Create VS");check(device->CreatePixelShader(pc->GetBufferPointer(),pc->GetBufferSize(),nullptr,&ps),"Create PS");
                objects[1]=vs;objects[2]=ps;context->VSSetShader(vs.Get(),nullptr,0);context->PSSetShader(ps.Get(),nullptr,0);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                D3D11_RASTERIZER_DESC rd{};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=rd.MultisampleEnable=TRUE;
                Com<ID3D11RasterizerState> raster;check(device->CreateRasterizerState(&rd,&raster),"Create rasterizer");objects[3]=raster;context->RSSetState(raster.Get());const D3D11_VIEWPORT viewport{0,0,1,1,0,1};context->RSSetViewports(1,&viewport);
                D3D11_BLEND_DESC bd{};bd.IndependentBlendEnable=TRUE;
                for(auto& rt:bd.RenderTarget){rt.SrcBlend=rt.SrcBlendAlpha=D3D11_BLEND_ONE;rt.DestBlend=rt.DestBlendAlpha=D3D11_BLEND_ONE;rt.BlendOp=rt.BlendOpAlpha=D3D11_BLEND_OP_ADD;rt.RenderTargetWriteMask=15;}
                bd.RenderTarget[0].BlendEnable=bd.RenderTarget[1].BlendEnable=TRUE;
                bd.RenderTarget[0].SrcBlend=bd.RenderTarget[0].SrcBlendAlpha=D3D11_BLEND_SRC_ALPHA;bd.RenderTarget[0].DestBlend=bd.RenderTarget[0].DestBlendAlpha=D3D11_BLEND_INV_SRC_ALPHA;
                Com<ID3D11BlendState> blend;check(device->CreateBlendState(&bd,&blend),"Create blend");objects[4]=blend;
                const float factors[]{0.125f,0.25f,0.5f,0.75f};context->OMSetBlendState(blend.Get(),factors,0x55555555);
                const auto baseline=diagnostic::pipelineCheckpoint(context.Get(),objects);
                unsigned checks=0,refusals=0;Json appliedPipeline;
                const auto refuses=[&](auto action,const char* reason){try{action();}catch(const std::exception& e){require(std::string(e.what()).find(reason)!=std::string::npos,"Unexpected refusal");++refusals;return;}throw std::runtime_error("Missing refusal");};
                for(bool remap:{false,true}){
                    const float initial[]{0.25f,0.5f,0.75f,1};for(const auto& view:views)context->ClearRenderTargetView(view.Get(),initial);
                    {
                        diagnostic::OutputRemapCheckpoint scope;
                        if(remap){
                            scope.apply(device.Get(),context.Get(),bytes(remapped));
                            refuses([&]{scope.apply(device.Get(),context.Get(),{});},"cannot be reused");
                            Com<ID3D11PixelShader> activePs;Com<ID3D11BlendState> activeBlend;context->PSGetShader(&activePs,nullptr,nullptr);context->OMGetBlendState(&activeBlend,nullptr,nullptr);objects[20]=activePs;objects[21]=activeBlend;
                            appliedPipeline=diagnostic::pipelineCheckpoint(context.Get(),objects);auto normalized=appliedPipeline;
                            require(normalized["shaders"]["ps"]==20&&normalized["blend"]["id"]==21,"Replacement identities differ");
                            require(normalized["rtv"][1].is_null()&&normalized["rtv"][3]==baseline["rtv"][1],"RTV remap differs");
                            require(normalized["blend"]["targets"][3]==baseline["blend"]["targets"][1],"Blend remap differs");
                            normalized["shaders"]["ps"]=2;normalized["blend"]["id"]=4;normalized["blend"]["targets"][3]=baseline["blend"]["targets"][3];normalized["rtv"][1]=normalized["rtv"][3];normalized["rtv"][3]=nullptr;
                            require(normalized==baseline,"Unexpected pipeline change during remap");objects.erase(20);objects.erase(21);
                        }
                        context->Draw(3,0);context->Draw(3,0);
                        scope.restore();if(remap)refuses([&]{scope.apply(device.Get(),context.Get(),{});},"cannot be reused");
                    }
                    require(diagnostic::pipelineCheckpoint(context.Get(),objects)==baseline,"Remap failed to restore pipeline");
                    for(unsigned target=0;target<4;++target)for(UINT sample=0;sample<samples;++sample){
                        const auto expected=(sample%2||target>=2)?std::array<uint32_t,4>{0x3400,0x3800,0x3a00,0x3c00}:target==0?std::array<uint32_t,4>{0x3700,0x3500,0x3480,0x3900}:std::array<uint32_t,4>{0x3800,0x3e00,0x3d00,0x4100};
                        require(diagnostic::readCheckpoint(device.Get(),textures[target].Get(),12,sample,0,0,0)==expected,"Remapped output differs from CPU oracle");++checks;
                    }
                    require(diagnostic::pipelineCheckpoint(context.Get(),objects)==baseline,"Readback changed pipeline");
                }
                refuses([&]{diagnostic::remapFragmentOutput(bytes(pc));},"verified GF2 pixel shader");
                raw[3]=views[3].Get();context->OMSetRenderTargets(4,raw,nullptr);refuses([&]{diagnostic::OutputRemapCheckpoint scope;scope.apply(device.Get(),context.Get(),{});},"vacant RTV3");raw[3]=nullptr;context->OMSetRenderTargets(4,raw,nullptr);
                raw[1]=nullptr;context->OMSetRenderTargets(4,raw,nullptr);refuses([&]{diagnostic::OutputRemapCheckpoint scope;scope.apply(device.Get(),context.Get(),{});},"RTV0/1");raw[1]=views[1].Get();context->OMSetRenderTargets(4,raw,nullptr);
                context->PSSetShader(nullptr,nullptr,0);refuses([&]{diagnostic::OutputRemapCheckpoint scope;scope.apply(device.Get(),context.Get(),{});},"PS without classes");context->PSSetShader(ps.Get(),nullptr,0);
                context->OMSetBlendState(nullptr,factors,0x55555555);refuses([&]{diagnostic::OutputRemapCheckpoint scope;scope.apply(device.Get(),context.Get(),{});},"explicit blend state");context->OMSetBlendState(blend.Get(),factors,0x55555555);
                for(unsigned invalid=0;invalid<3;++invalid){auto d=bd;if(invalid==0)d.AlphaToCoverageEnable=TRUE;else if(invalid==1)d.IndependentBlendEnable=FALSE;else {d.IndependentBlendEnable=FALSE;d.RenderTarget[0].SrcBlend=D3D11_BLEND_SRC1_COLOR;for(unsigned i=1;i<8;++i){d.RenderTarget[i].BlendEnable=FALSE;d.RenderTarget[i].RenderTargetWriteMask=0;}}Com<ID3D11BlendState> bad;check(device->CreateBlendState(&d,&bad),"Create refusal blend");context->OMSetBlendState(bad.Get(),factors,0x55555555);refuses([&]{diagnostic::OutputRemapCheckpoint scope;scope.apply(device.Get(),context.Get(),{});},invalid==2?"dual-source":"independent blending");context->OMSetBlendState(blend.Get(),factors,0x55555555);}
                D3D11_TEXTURE2D_DESC ud{1,1,1,1,DXGI_FORMAT_R32_UINT,{1,0},D3D11_USAGE_DEFAULT,D3D11_BIND_UNORDERED_ACCESS,0,0};Com<ID3D11Texture2D> uavTexture;Com<ID3D11UnorderedAccessView> uav;check(device->CreateTexture2D(&ud,nullptr,&uavTexture),"Create refusal UAV texture");check(device->CreateUnorderedAccessView(uavTexture.Get(),nullptr,&uav),"Create refusal UAV");auto rawUav=uav.Get();context->OMSetRenderTargetsAndUnorderedAccessViews(D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL,nullptr,nullptr,4,1,&rawUav,nullptr);refuses([&]{diagnostic::OutputRemapCheckpoint scope;scope.apply(device.Get(),context.Get(),{});},"bound OM UAVs");rawUav=nullptr;context->OMSetRenderTargetsAndUnorderedAccessViews(D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL,nullptr,nullptr,4,1,&rawUav,nullptr);
                require(diagnostic::pipelineCheckpoint(context.Get(),objects)==baseline,"Refusal changed original state");
                require(!debug->GetNumMessagesDiscardedByMessageCountLimit(),"Lost debug messages");
                for(UINT64 i=0;i<debug->GetNumStoredMessages();++i){SIZE_T size=0;check(debug->GetMessage(i,nullptr,&size),"Measure debug message");std::vector<uint8_t> storage(size);auto* message=reinterpret_cast<D3D11_MESSAGE*>(storage.data());check(debug->GetMessage(i,message,&size),"Read debug message");require(message->Severity>D3D11_MESSAGE_SEVERITY_WARNING,message->pDescription);}
                report["cases"].push_back({{"warp",warp},{"samples",samples},{"feature_level",device->GetFeatureLevel()},{"exact_texels",checks},{"refusals",refusals},{"debug_warnings_or_errors",0},{"original_pipeline",baseline},{"remapped_pipeline",appliedPipeline}});
            }
        }
        report["completed"]=true;std::ofstream stream(output);stream.exceptions(std::ios::badbit|std::ios::failbit);stream<<report.dump(2)<<'\n';stream.close();std::cout<<"Output remap checks passed\n";return 0;
    }catch(const std::exception& error){report["error"]=error.what();if(!output.empty()){std::ofstream stream(output);stream<<report.dump(2)<<'\n';}std::cerr<<error.what()<<'\n';return 1;}
}
