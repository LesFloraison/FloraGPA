#pragma once
// Independent native producer for the D32S8 initialization wire experiment.
// The producer selects complete stencil values; the initializer uses bit passes.
#include "msaa_readback.hpp"
#include "msaa_event_fixture.hpp"
#include "objects.hpp"
#include "command_stream.hpp"
#include "format/Writer.h"
#include <d3dcompiler.h>
#include <d3d11sdklayers.h>
#include <wrl/client.h>
#include <bit>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
using namespace flora::capture;
using namespace flora::format;
using Microsoft::WRL::ComPtr;
namespace {
constexpr UINT width=13,height=7,layers=2;
#ifdef FLORA_MSAA_D24
constexpr auto depth_storage=DXGI_FORMAT_R24G8_TYPELESS,depth_view_format=DXGI_FORMAT_D24_UNORM_S8_UINT,
    depth_srv_format=DXGI_FORMAT_R24_UNORM_X8_TYPELESS,stencil_srv_format=DXGI_FORMAT_X24_TYPELESS_G8_UINT;
UINT depth_bits(float value){const auto packed=uint32_t(std::round(double(value)*16777215.0));return std::bit_cast<UINT>(float(double(packed)/16777215.0));}
#else
constexpr auto depth_storage=DXGI_FORMAT_R32G8X24_TYPELESS,depth_view_format=DXGI_FORMAT_D32_FLOAT_S8X24_UINT,
    depth_srv_format=DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS,stencil_srv_format=DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;
UINT depth_bits(float value){return std::bit_cast<UINT>(value);}
#endif
const char* vertex="float4 main(uint i:SV_VertexID):SV_Position{return float4(i==2?3:-1,i==1?3:-1,0,1);}";
const char* produce=R"(cbuffer Values:register(b0){uint layer,sample,reference,pad;}
float main(float4 p:SV_Position):SV_Depth {
 uint x=uint(p.x),y=uint(p.y);
 if(((x*13+y*7+layer*97+sample*19)&255)!=reference)discard;
 uint n=x==0?0:x==1?16777216:x==2?8388608:x==3?1:x==4?16777215:(x*0x45d9f3bu+y*0x119de1f3u+layer*0x12345u+sample*0x5a37u+1)&0xffffff;
 return float(n)/16777216;})";
const char* restore_depth="Texture2D<uint2> plane:register(t0);float main(float4 p:SV_Position):SV_Depth{return asfloat(plane.Load(int3(uint2(p.xy),0)).x);}";
const char* restore_stencil="Texture2D<uint2> plane:register(t0);cbuffer Values:register(b0){uint bit;};void main(float4 p:SV_Position){if((plane.Load(int3(uint2(p.xy),0)).y&bit)==0)discard;}";
const char* change="float main():SV_Depth{return 0.25;}";
const char* display=R"(Texture2DMSArray<float> depth:register(t0);Texture2DMSArray<uint2> stencil:register(t1);
float4 main(float4 p:SV_Position):SV_Target{
 uint2 q=uint2(p.xy);uint s=q.x/26,t=(q.x/13)%2;int3 c=int3(q.x%13,q.y%7,q.y/7);
 if(t==1)return float4(stencil.Load(c,s).y,0,0,255)/255.0;
 uint d=asuint(depth.Load(c,s));return float4(d&255,(d>>8)&255,(d>>16)&255,d>>24)/255.0;})";
void require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
void check(HRESULT hr){if(FAILED(hr))throw std::runtime_error("MSAA depth fixture HRESULT "+std::to_string(UINT(hr)));}
ByteBuffer compile(const char* text,const char* profile){ComPtr<ID3DBlob> code,error;const auto hr=D3DCompile(text,strlen(text),"MSAA depth initialization",nullptr,nullptr,"main",profile,D3DCOMPILE_ENABLE_STRICTNESS,0,&code,&error);if(FAILED(hr)&&error)throw std::runtime_error(static_cast<const char*>(error->GetBufferPointer()));check(hr);auto* p=static_cast<const uint8_t*>(code->GetBufferPointer());return {p,p+code->GetBufferSize()};}
UINT depth_word(UINT x,UINT y,UINT layer,UINT sample){const UINT n=x==0?0:x==1?16777216:x==2?8388608:x==3?1:x==4?16777215:(x*0x45d9f3bu+y*0x119de1f3u+layer*0x12345u+sample*0x5a37u+1)&0xffffff;return depth_bits(float(n)/16777216);}
UINT stencil_byte(UINT x,UINT y,UINT layer,UINT sample){return (x*13+y*7+layer*97+sample*19)&255;}
ByteBuffer expected(UINT samples,bool edits,bool initialized=true){ByteBuffer bytes;for(UINT y=0;y<height*layers;++y)for(UINT x=0;x<width*samples*2;++x){const UINT sample=x/(width*2),tile=(x/width)%2,px=x%width,py=y%height,layer=y/height;const bool modified=edits && px<width/2 && !(sample%2);const UINT d=modified?depth_bits(0.25f):initialized?depth_word(px,py,layer,sample):0,s=modified?0xa6:initialized?stencil_byte(px,py,layer,sample):0;if(tile)bytes.insert(bytes.end(),{uint8_t(s),0,0,255});else for(UINT b=0;b<4;++b)bytes.push_back(uint8_t(d>>(b*8)));}return bytes;}
D3D11_DEPTH_STENCIL_DESC depth_state(bool stencil=true){D3D11_DEPTH_STENCIL_DESC d{};d.DepthEnable=TRUE;d.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;d.DepthFunc=D3D11_COMPARISON_ALWAYS;d.StencilEnable=stencil;d.StencilReadMask=255;d.StencilWriteMask=255;d.FrontFace={D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_REPLACE,D3D11_COMPARISON_ALWAYS};d.BackFace=d.FrontFace;return d;}
struct Owned{MsaaSamples samples;ByteBuffer final;};
Owned native(bool hardware,UINT samples,FrozenFrame* captured=nullptr,const RecordingOptions& options={},bool events=false,bool deferred=false,bool copyMutation=false){
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;check(D3D11CreateDevice(nullptr,hardware?D3D_DRIVER_TYPE_HARDWARE:D3D_DRIVER_TYPE_WARP,nullptr,D3D11_CREATE_DEVICE_DEBUG,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context));
    ComPtr<ID3D11InfoQueue> info;check(device.As(&info));info->ClearStoredMessages();wrap_object(device.GetAddressOf(),"MSAA depth fixture device");wrap_object(context.GetAddressOf(),"MSAA depth fixture context");
    D3D11_TEXTURE2D_DESC td{width,height,1,layers,depth_storage,{samples,0},D3D11_USAGE_DEFAULT,D3D11_BIND_DEPTH_STENCIL|D3D11_BIND_SHADER_RESOURCE,0,0};ComPtr<ID3D11Texture2D> source;check(device->CreateTexture2D(&td,nullptr,&source));
    auto code=compile(vertex,"vs_5_0");ComPtr<ID3D11VertexShader> vs;check(device->CreateVertexShader(code.data(),code.size(),nullptr,&vs));
    ComPtr<ID3D11PixelShader> ps,edit,show;code=compile(produce,"ps_5_0");check(device->CreatePixelShader(code.data(),code.size(),nullptr,&ps));code=compile(change,"ps_5_0");check(device->CreatePixelShader(code.data(),code.size(),nullptr,&edit));code=compile(display,"ps_5_0");check(device->CreatePixelShader(code.data(),code.size(),nullptr,&show));
    D3D11_BUFFER_DESC bd{16,D3D11_USAGE_DEFAULT,D3D11_BIND_CONSTANT_BUFFER,0,0,0};ComPtr<ID3D11Buffer> cb;check(device->CreateBuffer(&bd,nullptr,&cb));auto* b=cb.Get();
    D3D11_RASTERIZER_DESC rd{};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=TRUE;rd.MultisampleEnable=TRUE;ComPtr<ID3D11RasterizerState> raster,scissor;check(device->CreateRasterizerState(&rd,&raster));rd.ScissorEnable=TRUE;check(device->CreateRasterizerState(&rd,&scissor));
    auto ds=depth_state();ComPtr<ID3D11DepthStencilState> state;check(device->CreateDepthStencilState(&ds,&state));
    context->VSSetShader(vs.Get(),nullptr,0);context->PSSetShader(ps.Get(),nullptr,0);context->PSSetConstantBuffers(0,1,&b);context->RSSetState(raster.Get());context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);D3D11_VIEWPORT vp{0,0,float(width),float(height),0,1};context->RSSetViewports(1,&vp);
    std::array<ComPtr<ID3D11DepthStencilView>,layers> views;
    for(UINT layer=0;layer<layers;++layer){D3D11_DEPTH_STENCIL_VIEW_DESC vd{};vd.Format=depth_view_format;vd.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2DMSARRAY;vd.Texture2DMSArray={layer,1};check(device->CreateDepthStencilView(source.Get(),&vd,&views[layer]));context->ClearDepthStencilView(views[layer].Get(),3,0,0);context->OMSetRenderTargets(0,nullptr,views[layer].Get());
        for(UINT sample=0;sample<samples;++sample){context->OMSetBlendState(nullptr,nullptr,1u<<sample);for(UINT ref=0;ref<256;++ref){const UINT values[]{layer,sample,ref,0};context->UpdateSubresource(cb.Get(),0,nullptr,values,0,0);context->OMSetDepthStencilState(state.Get(),ref);context->Draw(3,0);}}}
    Owned result;result.samples=snapshot_msaa_samples(context.Get(),source.Get());
    for(const auto& plane:result.samples.planes){ByteBuffer oracle;for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x){
        const auto depth_value=depth_word(x,y,plane.array_slice,plane.sample),stencil_value=stencil_byte(x,y,plane.array_slice,plane.sample);
#ifdef FLORA_MSAA_D24
        const UINT packed=UINT(std::round(double(std::bit_cast<float>(depth_value))*16777215.0))|(stencil_value<<24);
        for(UINT b=0;b<4;++b)oracle.push_back(uint8_t(packed>>(b*8)));
#else
        for(UINT word:{depth_value,stencil_value})for(UINT b=0;b<4;++b)oracle.push_back(uint8_t(word>>(b*8)));
#endif
    }require(plane.bytes==oracle,"Initial depth/stencil samples differ from CPU words");}
    MsaaEventFixture event;event.prepare(device.Get(),context.Get(),events,samples==4);
    std::shared_ptr<FrameRecording> recording;if(captured)recording=begin_recording(context.Get(),options);
    event.observe(context.Get());
    if(deferred){
        ComPtr<ID3D11DeviceContext> worker;check(device->CreateDeferredContext(0,&worker));
        worker->VSSetShader(vs.Get(),nullptr,0);worker->PSSetShader(edit.Get(),nullptr,0);worker->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        worker->RSSetViewports(1,&vp);worker->RSSetState(scissor.Get());D3D11_RECT rect{0,0,width/2,height};worker->RSSetScissorRects(1,&rect);
        worker->OMSetBlendState(nullptr,nullptr,0x55555555);worker->OMSetDepthStencilState(state.Get(),0xa6);
        for(const auto& view:views){worker->OMSetRenderTargets(0,nullptr,view.Get());worker->Draw(3,0);}
        if(copyMutation){D3D11_TEXTURE2D_DESC copyDesc{};source->GetDesc(&copyDesc);ComPtr<ID3D11Texture2D> copy;check(device->CreateTexture2D(&copyDesc,nullptr,&copy));worker->CopyResource(copy.Get(),source.Get());}
        ComPtr<ID3D11CommandList> list;check(worker->FinishCommandList(FALSE,&list));
        context->ExecuteCommandList(list.Get(),TRUE);
        ComPtr<ID3D11DepthStencilState> actual;UINT ref=0;context->OMGetDepthStencilState(&actual,&ref);
        require(actual.Get()==state.Get() && ref==255,"Deferred depth TRUE lost state/ref");actual.Reset();
        context->ExecuteCommandList(list.Get(),FALSE);context->OMGetDepthStencilState(&actual,&ref);
        require(!actual && !ref,"Deferred depth FALSE did not reset state/ref");
        context->VSSetShader(vs.Get(),nullptr,0);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    }else{
    context->PSSetShader(edit.Get(),nullptr,0);context->RSSetState(scissor.Get());D3D11_RECT rect{0,0,width/2,height};context->RSSetScissorRects(1,&rect);context->OMSetBlendState(nullptr,nullptr,0x55555555);context->OMSetDepthStencilState(state.Get(),0xa6);
    for(const auto& view:views){context->OMSetRenderTargets(0,nullptr,view.Get());context->Draw(3,0);}
    }
    context->OMSetRenderTargets(0,nullptr,nullptr);context->OMSetDepthStencilState(nullptr,0);
    td={width*samples*2,height*layers,1,1,DXGI_FORMAT_R8G8B8A8_UNORM,{1,0},D3D11_USAGE_DEFAULT,D3D11_BIND_RENDER_TARGET,0,0};ComPtr<ID3D11Texture2D> output;ComPtr<ID3D11RenderTargetView> target;check(device->CreateTexture2D(&td,nullptr,&output));check(device->CreateRenderTargetView(output.Get(),nullptr,&target));
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};sd.Format=depth_srv_format;sd.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY;sd.Texture2DMSArray.ArraySize=layers;ComPtr<ID3D11ShaderResourceView> depth,stencil;check(device->CreateShaderResourceView(source.Get(),&sd,&depth));sd.Format=stencil_srv_format;check(device->CreateShaderResourceView(source.Get(),&sd,&stencil));ID3D11ShaderResourceView* srvs[]{depth.Get(),stencil.Get()};auto* r=target.Get();context->PSSetShaderResources(0,2,srvs);context->OMSetRenderTargets(1,&r,nullptr);context->PSSetShader(show.Get(),nullptr,0);context->RSSetState(raster.Get());context->OMSetBlendState(nullptr,nullptr,UINT_MAX);vp.Width=float(td.Width);vp.Height=float(td.Height);context->RSSetViewports(1,&vp);context->Draw(3,0);
    event.end(context.Get());
    std::optional<RecordedFrame> recorded;if(recording)recorded=recording->finish();
    auto final=snapshot_resource(context.Get(),output.Get());result.final=final.subresources[0].bytes;require(result.final==expected(samples,true),"Native final depth/stencil byte tiles differ from CPU");context->ClearState();
    for(UINT64 i=0;i<info->GetNumStoredMessagesAllowedByRetrievalFilter();++i){SIZE_T size=0;check(info->GetMessage(i,nullptr,&size));ByteBuffer bytes(size);auto* m=reinterpret_cast<D3D11_MESSAGE*>(bytes.data());check(info->GetMessage(i,m,&size));if(m->Severity<=D3D11_MESSAGE_SEVERITY_WARNING)throw std::runtime_error(m->pDescription);}
    if(captured){recorded->require_complete();*captured=freeze_frame(std::move(*recorded),object_record(output.Get())->id,std::move(final));}
    return result;
}
}
