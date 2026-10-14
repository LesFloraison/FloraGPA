#pragma once
// Shared native producer and exact CPU oracle for MSAA wire/encoder tests.
// Returns owned samples after source COM objects die. Never imports Replay.
#include "msaa_readback.hpp"
#include "msaa_event_fixture.hpp"
#include "objects.hpp"
#include "command_stream.hpp"
#include "format/Writer.h"
#include <d3dcompiler.h>
#include <d3d11sdklayers.h>
#include <wrl/client.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <functional>
using namespace flora::capture;
using namespace flora::format;
using Microsoft::WRL::ComPtr;
namespace {
constexpr UINT width=13,height=7,layers=2;
const char* vertex="float4 main(uint i:SV_VertexID):SV_Position{return float4(i==2?3:-1,i==1?3:-1,0,1);}";
#ifdef FLORA_MSAA_RGBA16
constexpr DXGI_FORMAT storage_format=DXGI_FORMAT_R16G16B16A16_TYPELESS,integer_format=DXGI_FORMAT_R16G16B16A16_UINT,render_format=DXGI_FORMAT_R16G16B16A16_FLOAT,display_format=integer_format;
constexpr UINT display_tiles=2;
const char* produce=R"(cbuffer Layer:register(b0){uint layer;}
uint4 main(float4 p:SV_Position,uint s:SV_SampleIndex):SV_Target{
uint x=uint(p.x),y=uint(p.y);return uint4((x*257+y*13+layer*4001+s*977)&65535,(y*4003+layer*43+s*1543)&65535,(x*701+y*1103+layer*1901+s*3701)&65535,(x*8191+y*127+layer*32749+s*2047)&65535);})";
// All four values are exactly representable in binary16, including negative
// and greater-than-one values. The output oracle compares storage bits.
const char* change="float4 main():SV_Target{return float4(1.5,-1.0,0.00006103515625,0.333251953125);}";
std::string display(UINT){return "Texture2DMSArray<uint4> source:register(t0);float4 main(float4 p:SV_Position):SV_Target{uint2 q=uint2(p.xy);uint s=q.x/26,b=(q.x/13)%2;uint4 v=source.Load(int3(q.x%13,q.y%7,q.y/7),s);return float4((v>>(b*8))&255)/255.0;}";}
std::array<uint16_t,4> sample_words(UINT x,UINT y,UINT layer,UINT sample,bool changed){if(changed && x<width/2 && !(sample%2))return {0x3e00,0xbc00,0x0400,0x3555};return {uint16_t(x*257+y*13+layer*4001+sample*977),uint16_t(y*4003+layer*43+sample*1543),uint16_t(x*701+y*1103+layer*1901+sample*3701),uint16_t(x*8191+y*127+layer*32749+sample*2047)};}
std::array<uint8_t,4> pixel(UINT x,UINT y,UINT layer,UINT sample,bool changed,UINT tile=0){const auto words=sample_words(x,y,layer,sample,changed);std::array<uint8_t,4> bytes;for(size_t i=0;i<4;++i)bytes[i]=uint8_t(words[i]>>(tile*8));return bytes;}
ByteBuffer sample_bytes(UINT x,UINT y,UINT layer,UINT sample){ByteBuffer bytes;for(auto word:sample_words(x,y,layer,sample,false)){bytes.push_back(uint8_t(word));bytes.push_back(uint8_t(word>>8));}return bytes;}
#else
constexpr DXGI_FORMAT storage_format=DXGI_FORMAT_R8G8B8A8_TYPELESS,integer_format=DXGI_FORMAT_R8G8B8A8_UINT,render_format=integer_format,display_format=DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr UINT display_tiles=1;
const char* produce=R"(cbuffer Layer:register(b0){uint layer;}
uint4 main(float4 p:SV_Position,uint s:SV_SampleIndex):SV_Target{
uint x=uint(p.x),y=uint(p.y);return uint4((x*17+y*3+layer*71+s*29)&255,(y*31+layer*43+s*13)&255,(x*7+y*11+layer*19+s*37)&255,255);})";
const char* restore="Texture2D<uint4> plane:register(t0);uint4 main(float4 p:SV_Position):SV_Target{return plane.Load(int3(uint2(p.xy),0));}";
const char* change="uint4 main():SV_Target{return uint4(240,19,73,255);}";
std::string display(UINT samples){return "Texture2DMSArray<float4> source:register(t0);float4 main(float4 p:SV_Position):SV_Target{uint2 q=uint2(p.xy);uint s=q.x/13,l=q.y/7;return source.Load(int3(q.x%13,q.y%7,l),s);}";}
std::array<uint8_t,4> pixel(UINT x,UINT y,UINT layer,UINT sample,bool changed,UINT=0){if(changed && x<width/2 && !(sample%2))return {240,19,73,255};return {uint8_t(x*17+y*3+layer*71+sample*29),uint8_t(y*31+layer*43+sample*13),uint8_t(x*7+y*11+layer*19+sample*37),255};}
ByteBuffer sample_bytes(UINT x,UINT y,UINT layer,UINT sample){const auto p=pixel(x,y,layer,sample,false);return {p.begin(),p.end()};}
#endif
void require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
void check(HRESULT hr){if(FAILED(hr))throw std::runtime_error("MSAA wire fixture HRESULT "+std::to_string(UINT(hr)));}
ByteBuffer compile(const std::string& text,const char* profile){ComPtr<ID3DBlob> code,error;const auto hr=D3DCompile(text.data(),text.size(),"MSAA wire experiment",nullptr,nullptr,"main",profile,D3DCOMPILE_ENABLE_STRICTNESS,0,&code,&error);if(FAILED(hr)&&error)throw std::runtime_error(static_cast<const char*>(error->GetBufferPointer()));check(hr);auto* p=static_cast<const uint8_t*>(code->GetBufferPointer());return {p,p+code->GetBufferSize()};}
ByteBuffer expected(UINT samples,bool changed,bool initialized=true){ByteBuffer result;for(UINT y=0;y<height*layers;++y)for(UINT x=0;x<width*samples*display_tiles;++x){const auto sample=x/(width*display_tiles),tile=(x/width)%display_tiles;const bool modified=changed && x%width<width/2 && !(sample%2);const auto p=initialized||modified?pixel(x%width,y%height,y/height,sample,changed,tile):std::array<uint8_t,4>{0,0,0,0};result.insert(result.end(),p.begin(),p.end());}return result;}
struct Owned {MsaaSamples samples;ByteBuffer final;};
Owned native(bool hardware,UINT samples,FrozenFrame* captured=nullptr,const RecordingOptions& options={},bool events=false,bool deferred=false,bool copyMutation=false,
    const std::function<void(ID3D11DeviceContext*,const RecordedFrame&)>& inspect={}){
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;check(D3D11CreateDevice(nullptr,hardware?D3D_DRIVER_TYPE_HARDWARE:D3D_DRIVER_TYPE_WARP,nullptr,D3D11_CREATE_DEVICE_DEBUG,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context));
    ComPtr<ID3D11InfoQueue> info;check(device.As(&info));info->ClearStoredMessages();wrap_object(device.GetAddressOf(),"MSAA wire device");wrap_object(context.GetAddressOf(),"MSAA wire context");
    D3D11_TEXTURE2D_DESC td{width,height,1,layers,storage_format,{samples,0},D3D11_USAGE_DEFAULT,D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE,0,0};ComPtr<ID3D11Texture2D> source;check(device->CreateTexture2D(&td,nullptr,&source));
    const auto vsCode=compile(vertex,"vs_5_0"),psCode=compile(produce,"ps_5_0"),changeCode=compile(change,"ps_5_0"),displayCode=compile(display(samples),"ps_5_0");
    ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11PixelShader> ps,edit,show;check(device->CreateVertexShader(vsCode.data(),vsCode.size(),nullptr,&vs));check(device->CreatePixelShader(psCode.data(),psCode.size(),nullptr,&ps));check(device->CreatePixelShader(changeCode.data(),changeCode.size(),nullptr,&edit));check(device->CreatePixelShader(displayCode.data(),displayCode.size(),nullptr,&show));
    D3D11_BUFFER_DESC bd{16,D3D11_USAGE_DEFAULT,D3D11_BIND_CONSTANT_BUFFER,0,0,0};ComPtr<ID3D11Buffer> cb;check(device->CreateBuffer(&bd,nullptr,&cb));auto* b=cb.Get();
    D3D11_RASTERIZER_DESC rd{};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=TRUE;rd.MultisampleEnable=TRUE;ComPtr<ID3D11RasterizerState> raster,scissor;check(device->CreateRasterizerState(&rd,&raster));rd.ScissorEnable=TRUE;check(device->CreateRasterizerState(&rd,&scissor));
    context->VSSetShader(vs.Get(),nullptr,0);context->PSSetShader(ps.Get(),nullptr,0);context->PSSetConstantBuffers(0,1,&b);context->RSSetState(raster.Get());context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);D3D11_VIEWPORT vp{0,0,float(width),float(height),0,1};context->RSSetViewports(1,&vp);
    std::array<ComPtr<ID3D11RenderTargetView>,layers> views;
    for(UINT layer=0;layer<layers;++layer){D3D11_RENDER_TARGET_VIEW_DESC vd{};vd.Format=integer_format;vd.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY;vd.Texture2DMSArray={layer,1};check(device->CreateRenderTargetView(source.Get(),&vd,&views[layer]));auto* r=views[layer].Get();context->OMSetRenderTargets(1,&r,nullptr);const UINT data[]{layer,0,0,0};context->UpdateSubresource(cb.Get(),0,nullptr,data,0,0);context->Draw(3,0);}
    Owned result;result.samples=snapshot_msaa_samples(context.Get(),source.Get());
    for(const auto& plane:result.samples.planes){ByteBuffer oracle;for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x){const auto p=sample_bytes(x,y,plane.array_slice,plane.sample);oracle.insert(oracle.end(),p.begin(),p.end());}require(plane.bytes==oracle,"Native initial samples differ from CPU");}
    if constexpr(render_format!=integer_format)for(UINT layer=0;layer<layers;++layer){views[layer].Reset();D3D11_RENDER_TARGET_VIEW_DESC vd{};vd.Format=render_format;vd.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY;vd.Texture2DMSArray={layer,1};check(device->CreateRenderTargetView(source.Get(),&vd,&views[layer]));}
    MsaaEventFixture event;event.prepare(device.Get(),context.Get(),events,samples==4);
    std::shared_ptr<FrameRecording> recording;if(captured)recording=begin_recording(context.Get(),options);
    event.observe(context.Get());
    if(deferred){
        ComPtr<ID3D11DeviceContext> worker;check(device->CreateDeferredContext(0,&worker));
        ComPtr<ID3D11CommandList> editList,resetList;
        std::exception_ptr failure;
        // One worker owns the deferred context. Joining is the explicit CPU
        // synchronization point; only immediate submission contributes events.
        std::thread compileLists([&]{try{
            auto common=[&]{worker->VSSetShader(vs.Get(),nullptr,0);worker->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);worker->RSSetViewports(1,&vp);};
            common();worker->PSSetShader(edit.Get(),nullptr,0);worker->RSSetState(scissor.Get());
            D3D11_RECT rect{0,0,width/2,height};worker->RSSetScissorRects(1,&rect);worker->OMSetBlendState(nullptr,nullptr,0x55555555);
            for(const auto& view:views){auto* r=view.Get();worker->OMSetRenderTargets(1,&r,nullptr);worker->Draw(3,0);}
            if(copyMutation){D3D11_TEXTURE2D_DESC copyDesc{};source->GetDesc(&copyDesc);ComPtr<ID3D11Texture2D> copy;check(device->CreateTexture2D(&copyDesc,nullptr,&copy));worker->CopyResource(copy.Get(),source.Get());}
            check(worker->FinishCommandList(FALSE,&editList));
            common();worker->PSSetShader(ps.Get(),nullptr,0);worker->PSSetConstantBuffers(0,1,&b);worker->RSSetState(scissor.Get());
            worker->RSSetScissorRects(1,&rect);worker->OMSetBlendState(nullptr,nullptr,0x55555555);
            for(UINT layer=0;layer<layers;++layer){
                D3D11_RENDER_TARGET_VIEW_DESC vd{};vd.Format=integer_format;vd.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY;vd.Texture2DMSArray={layer,1};
                ComPtr<ID3D11RenderTargetView> original;check(device->CreateRenderTargetView(source.Get(),&vd,&original));auto* r=original.Get();
                worker->OMSetRenderTargets(1,&r,nullptr);const UINT data[]{layer,0,0,0};worker->UpdateSubresource(cb.Get(),0,nullptr,data,0,0);worker->Draw(3,0);
            }
            check(worker->FinishCommandList(FALSE,&resetList));
        }catch(...){failure=std::current_exception();}});compileLists.join();if(failure)std::rethrow_exception(failure);
        context->ExecuteCommandList(editList.Get(),TRUE);
        ComPtr<ID3D11PixelShader> actual;context->PSGetShader(&actual,nullptr,nullptr);require(actual.Get()==ps.Get(),"Deferred TRUE lost immediate PS");actual.Reset();
        context->ExecuteCommandList(resetList.Get(),FALSE);
        context->PSGetShader(&actual,nullptr,nullptr);require(!actual,"Deferred FALSE did not reset immediate PS");
        context->VSSetShader(vs.Get(),nullptr,0);context->PSSetShader(ps.Get(),nullptr,0);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->ExecuteCommandList(editList.Get(),TRUE);
        context->PSGetShader(&actual,nullptr,nullptr);require(actual.Get()==ps.Get(),"Repeated deferred TRUE lost immediate PS");
    }else{
    context->PSSetShader(edit.Get(),nullptr,0);context->RSSetState(scissor.Get());D3D11_RECT rect{0,0,width/2,height};context->RSSetScissorRects(1,&rect);context->OMSetBlendState(nullptr,nullptr,0x55555555);
    for(const auto& view:views){auto* r=view.Get();context->OMSetRenderTargets(1,&r,nullptr);context->Draw(3,0);}
    }
    context->OMSetRenderTargets(0,nullptr,nullptr);td={width*samples*display_tiles,height*layers,1,1,DXGI_FORMAT_R8G8B8A8_UNORM,{1,0},D3D11_USAGE_DEFAULT,D3D11_BIND_RENDER_TARGET,0,0};ComPtr<ID3D11Texture2D> output;ComPtr<ID3D11RenderTargetView> target;check(device->CreateTexture2D(&td,nullptr,&output));check(device->CreateRenderTargetView(output.Get(),nullptr,&target));
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};sd.Format=display_format;sd.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY;sd.Texture2DMSArray.ArraySize=layers;ComPtr<ID3D11ShaderResourceView> srv;check(device->CreateShaderResourceView(source.Get(),&sd,&srv));auto* s=srv.Get();auto* r=target.Get();context->PSSetShaderResources(0,1,&s);context->OMSetRenderTargets(1,&r,nullptr);context->PSSetShader(show.Get(),nullptr,0);context->RSSetState(raster.Get());context->OMSetBlendState(nullptr,nullptr,UINT_MAX);vp.Width=float(width*samples*display_tiles);vp.Height=float(height*layers);context->RSSetViewports(1,&vp);context->Draw(3,0);
    event.end(context.Get());
    std::optional<RecordedFrame> recorded;if(recording)recorded=recording->finish();
    auto final=snapshot_resource(context.Get(),output.Get());result.final=final.subresources.at(0).bytes;require(result.final==expected(samples,true),"Native final image differs from CPU");
    if(inspect){require(recorded.has_value(),"Inspection requires recording");inspect(context.Get(),*recorded);
        // Draw once more through the still-bound application pipeline. This
        // checks state restoration as well as the existing output bytes.
        require(snapshot_resource(context.Get(),output.Get()).subresources.at(0).bytes==result.final,"Inspection changed output bytes");
        context->Draw(3,0);
        require(snapshot_resource(context.Get(),output.Get()).subresources.at(0).bytes==result.final,"Inspection changed application draw state");}
    if(captured){recorded->require_complete();*captured=freeze_frame(std::move(*recorded),object_record(output.Get())->id,std::move(final));}
    context->ClearState();
    for(UINT64 i=0;i<info->GetNumStoredMessagesAllowedByRetrievalFilter();++i){SIZE_T size=0;check(info->GetMessage(i,nullptr,&size));ByteBuffer bytes(size);auto* m=reinterpret_cast<D3D11_MESSAGE*>(bytes.data());check(info->GetMessage(i,m,&size));require(m->Severity>D3D11_MESSAGE_SEVERITY_WARNING,m->pDescription);}
    return result;
}
}
