#include "objects.hpp"
#include "command_stream.hpp"
#include "encoder.hpp"
#include <d3dcompiler.h>
#include <d3d11sdklayers.h>
#include <wrl/client.h>
#include <filesystem>
#include <fstream>
#include <iostream>
using namespace flora::capture;
using Microsoft::WRL::ComPtr;
namespace {
void require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
void check(HRESULT hr){if(FAILED(hr))throw std::runtime_error("Scissor fixture HRESULT "+std::to_string(UINT(hr)));}
ComPtr<ID3DBlob> compile(const char* text,const char* profile){ComPtr<ID3DBlob> code,error;const auto hr=D3DCompile(text,strlen(text),"capture scissor",nullptr,nullptr,"main",profile,0,0,&code,&error);if(FAILED(hr)&&error)throw std::runtime_error(static_cast<const char*>(error->GetBufferPointer()));check(hr);return code;}
std::vector<uint8_t> expected(UINT mode){std::vector<uint8_t> result;for(UINT y=0;y<8;++y)for(UINT x=0;x<8;++x){std::array<uint8_t,4> color{0,0,0,255};if(mode!=3 && x<4)color={255,0,0,255};if((mode==0 && x>=4) || mode==2)color={0,255,0,255};result.insert(result.end(),color.begin(),color.end());}return result;}
FrozenFrame capture(bool hardware,UINT mode){
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;check(D3D11CreateDevice(nullptr,hardware?D3D_DRIVER_TYPE_HARDWARE:D3D_DRIVER_TYPE_WARP,nullptr,D3D11_CREATE_DEVICE_DEBUG,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context));ComPtr<ID3D11InfoQueue> info;check(device.As(&info));info->ClearStoredMessages();wrap_object(device.GetAddressOf(),"Scissor device");wrap_object(context.GetAddressOf(),"Scissor context");
    D3D11_TEXTURE2D_DESC td{8,8,1,1,DXGI_FORMAT_R8G8B8A8_UNORM,{1,0},D3D11_USAGE_DEFAULT,D3D11_BIND_RENDER_TARGET,0,0};ComPtr<ID3D11Texture2D> texture;ComPtr<ID3D11RenderTargetView> rtv;check(device->CreateTexture2D(&td,nullptr,&texture));check(device->CreateRenderTargetView(texture.Get(),nullptr,&rtv));const float black[]{0,0,0,1};context->ClearRenderTargetView(rtv.Get(),black);
    auto v=compile("float4 main(uint i:SV_VertexID):SV_Position{return float4(i==2?3:-1,i==1?3:-1,0,1);}","vs_5_0"),r=compile("float4 main():SV_Target{return float4(1,0,0,1);}","ps_5_0"),g=compile("float4 main():SV_Target{return float4(0,1,0,1);}","ps_5_0");ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11PixelShader> red,green;check(device->CreateVertexShader(v->GetBufferPointer(),v->GetBufferSize(),nullptr,&vs));check(device->CreatePixelShader(r->GetBufferPointer(),r->GetBufferSize(),nullptr,&red));check(device->CreatePixelShader(g->GetBufferPointer(),g->GetBufferSize(),nullptr,&green));
    D3D11_RASTERIZER_DESC rd{};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=TRUE;rd.ScissorEnable=TRUE;ComPtr<ID3D11RasterizerState> enabled,disabled;check(device->CreateRasterizerState(&rd,&enabled));rd.ScissorEnable=FALSE;check(device->CreateRasterizerState(&rd,&disabled));
    context->VSSetShader(vs.Get(),nullptr,0);context->PSSetShader(red.Get(),nullptr,0);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);D3D11_VIEWPORT vp{0,0,8,8,0,1};context->RSSetViewports(1,&vp);context->RSSetState(enabled.Get());auto* target=rtv.Get();context->OMSetRenderTargets(1,&target,nullptr);D3D11_RECT left{0,0,4,8},right{4,0,8,8};context->RSSetScissorRects(mode==3?0:1,mode==3?nullptr:&left);
    auto recording=begin_recording(context.Get());context->Draw(3,0);context->PSSetShader(green.Get(),nullptr,0);if(mode==0)context->RSSetScissorRects(1,&right);else context->RSSetScissorRects(0,nullptr);if(mode==2)context->RSSetState(disabled.Get());context->Draw(3,0);auto commands=recording->finish();commands.require_complete();require(commands.commands.size()==2,"Scissor draws lost");require(commands.commands.back().bindings.scissors.size()==(mode==0?1:0),"Empty scissor array was not captured");
    auto final=snapshot_resource(context.Get(),texture.Get());require(final.subresources.at(0).bytes==expected(mode),"Native scissor pixels differ from CPU");auto frame=freeze_frame(std::move(commands),object_record(texture.Get())->id,std::move(final));context->ClearState();
    for(UINT64 i=0;i<info->GetNumStoredMessagesAllowedByRetrievalFilter();++i){SIZE_T size=0;check(info->GetMessage(i,nullptr,&size));std::vector<uint8_t> bytes(size);auto* m=reinterpret_cast<D3D11_MESSAGE*>(bytes.data());check(info->GetMessage(i,m,&size));require(m->Severity>D3D11_MESSAGE_SEVERITY_WARNING,m->pDescription);}return frame;
}
}
int wmain(int argc,wchar_t** argv){try{bool hardware=false;std::filesystem::path out;for(int i=1;i<argc;++i){if(std::wstring(argv[i])==L"--hardware")hardware=true;else out=argv[i];}if(!out.empty())std::filesystem::create_directories(out);for(UINT mode=0;mode<4;++mode){auto frame=capture(hardware,mode);require(object_status().at("active")==0 && metadata_status().at("alive")==0,"Native source objects survived encoding boundary");if(!out.empty()){auto name=std::to_string(mode);encode_frame(frame,out/(name+".gpa_frame"));auto pixels=expected(mode);std::ofstream file(out/(name+".expected.rgba"),std::ios::binary);file.write(reinterpret_cast<const char*>(pixels.data()),pixels.size());require(bool(file),"Failed oracle write");}}
    std::cout<<Json{{"passed",true},{"hardware",hardware},{"cases",4},{"source_gpu_objects_alive",0},{"debug_warnings_or_errors",0}}.dump()<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
