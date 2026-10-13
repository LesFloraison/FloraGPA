#pragma once
// Native application fixture only: no capture model, wrapper, encoder or replay.
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <array>
#include <algorithm>
#include <string>
#include <vector>
#include <stdexcept>
struct MsaaLiveRenderer {
    template<class T> using Ptr=Microsoft::WRL::ComPtr<T>;
    static constexpr UINT width=13,height=7,layers=2;
    UINT samples;
    Ptr<ID3D11Device> device;Ptr<ID3D11DeviceContext> context;
    Ptr<ID3D11Texture2D> source;Ptr<ID3D11ShaderResourceView> srv;
    std::array<Ptr<ID3D11RenderTargetView>,layers> views;
    Ptr<ID3D11VertexShader> vs;Ptr<ID3D11PixelShader> show;
    std::array<Ptr<ID3D11PixelShader>,5> edits;
    Ptr<ID3D11RasterizerState> raster,scissor;
    static void check(HRESULT hr){if(FAILED(hr))throw std::runtime_error("MSAA live native HRESULT "+std::to_string(UINT(hr)));}
    static Ptr<ID3DBlob> compile(const std::string& text,const char* profile){Ptr<ID3DBlob> code,error;auto hr=D3DCompile(text.data(),text.size(),"native-msaa",nullptr,nullptr,"main",profile,D3DCOMPILE_ENABLE_STRICTNESS,0,&code,&error);if(FAILED(hr)&&error)throw std::runtime_error(static_cast<const char*>(error->GetBufferPointer()));check(hr);return code;}
    Ptr<ID3D11PixelShader> pixel_shader(const std::string& text){auto code=compile(text,"ps_5_0");Ptr<ID3D11PixelShader> ps;check(device->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&ps));return ps;}
    static constexpr UINT colors[5][4]{{240,19,73,255},{19,240,73,255},{19,73,240,255},{240,73,19,255},{73,19,240,255}};
    MsaaLiveRenderer(ID3D11Device* d,ID3D11DeviceContext* c,UINT n):samples(n),device(d),context(c){
        D3D11_TEXTURE2D_DESC td{width,height,1,layers,DXGI_FORMAT_R8G8B8A8_TYPELESS,{samples,0},D3D11_USAGE_DEFAULT,D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE,0,0};check(device->CreateTexture2D(&td,nullptr,&source));
        auto code=compile("float4 main(uint i:SV_VertexID):SV_Position{return float4(i==2?3:-1,i==1?3:-1,0,1);}","vs_5_0");check(device->CreateVertexShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&vs));
        auto seed=pixel_shader("cbuffer C:register(b0){uint layer;}uint4 main(float4 p:SV_Position,uint s:SV_SampleIndex):SV_Target{uint x=uint(p.x),y=uint(p.y);return uint4((x*17+y*3+layer*71+s*29)&255,(y*31+layer*43+s*13)&255,(x*7+y*11+layer*19+s*37)&255,255);}");
        show=pixel_shader("Texture2DMSArray<float4> source:register(t0);float4 main(float4 p:SV_Position):SV_Target{uint2 q=uint2(p.xy);return source.Load(int3(q.x%13,q.y%7,q.y/7),q.x/13);}");
        for(size_t i=0;i<edits.size();++i){const auto* color=colors[i];edits[i]=pixel_shader("uint4 main():SV_Target{return uint4("+std::to_string(color[0])+","+std::to_string(color[1])+","+std::to_string(color[2])+",255);}");}
        D3D11_RASTERIZER_DESC rd{};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=TRUE;rd.MultisampleEnable=TRUE;check(device->CreateRasterizerState(&rd,&raster));rd.ScissorEnable=TRUE;check(device->CreateRasterizerState(&rd,&scissor));
        D3D11_VIEWPORT vp{0,0,float(width),float(height),0,1};context->VSSetShader(vs.Get(),nullptr,0);context->PSSetShader(seed.Get(),nullptr,0);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);context->RSSetState(raster.Get());context->RSSetViewports(1,&vp);
        for(UINT layer=0;layer<layers;++layer){D3D11_RENDER_TARGET_VIEW_DESC vd{};vd.Format=DXGI_FORMAT_R8G8B8A8_UINT;vd.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY;vd.Texture2DMSArray={layer,1};check(device->CreateRenderTargetView(source.Get(),&vd,&views[layer]));auto* r=views[layer].Get();context->OMSetRenderTargets(1,&r,nullptr);const UINT values[]{layer,0,0,0};D3D11_BUFFER_DESC bd{16,D3D11_USAGE_IMMUTABLE,D3D11_BIND_CONSTANT_BUFFER,0,0,0};D3D11_SUBRESOURCE_DATA data{values,0,0};Ptr<ID3D11Buffer> constants;check(device->CreateBuffer(&bd,&data,&constants));auto* b=constants.Get();context->PSSetConstantBuffers(0,1,&b);context->Draw(3,0);}
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};sd.Format=DXGI_FORMAT_R8G8B8A8_UNORM;sd.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY;sd.Texture2DMSArray.ArraySize=layers;check(device->CreateShaderResourceView(source.Get(),&sd,&srv));context->ClearState();
    }
    void render(ID3D11RenderTargetView* output,uint64_t frame){
        ID3D11ShaderResourceView* none=nullptr;context->PSSetShaderResources(0,1,&none);
        context->VSSetShader(vs.Get(),nullptr,0);context->PSSetShader(edits[frame%5].Get(),nullptr,0);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);context->RSSetState(scissor.Get());D3D11_VIEWPORT vp{0,0,float(width),float(height),0,1};context->RSSetViewports(1,&vp);D3D11_RECT rect{0,0,width/2,height};context->RSSetScissorRects(1,&rect);context->OMSetBlendState(nullptr,nullptr,0x55555555);
        for(const auto& view:views){auto* r=view.Get();context->OMSetRenderTargets(1,&r,nullptr);context->Draw(3,0);}
        context->OMSetRenderTargets(1,&output,nullptr);auto* s=srv.Get();context->PSSetShaderResources(0,1,&s);context->PSSetShader(show.Get(),nullptr,0);context->RSSetState(raster.Get());context->RSSetScissorRects(0,nullptr);context->OMSetBlendState(nullptr,nullptr,UINT_MAX);vp.Width=float(width*samples);vp.Height=float(height*layers);context->RSSetViewports(1,&vp);context->Draw(3,0);
    }
    void verify(ID3D11Texture2D* output,uint64_t frame){
        D3D11_TEXTURE2D_DESC d{};output->GetDesc(&d);d.Usage=D3D11_USAGE_STAGING;d.BindFlags=d.MiscFlags=0;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;Ptr<ID3D11Texture2D> staging;check(device->CreateTexture2D(&d,nullptr,&staging));context->CopyResource(staging.Get(),output);D3D11_MAPPED_SUBRESOURCE map{};check(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&map));bool exact=true;
        for(UINT y=0;y<height*layers;++y)for(UINT x=0;x<width*samples;++x){const UINT px=x%width,py=y%height,layer=y/height,sample=x/width;std::array<UINT,4> p{(px*17+py*3+layer*71+sample*29)&255,(py*31+layer*43+sample*13)&255,(px*7+py*11+layer*19+sample*37)&255,255};if(px<width/2 && !(sample%2))std::copy_n(colors[frame%5],4,p.begin());const auto* native=static_cast<const unsigned char*>(map.pData)+size_t(y)*map.RowPitch+x*4;for(UINT i=0;i<4;++i)exact=exact && native[i]==p[i];}
        context->Unmap(staging.Get(),0);if(!exact)throw std::runtime_error("MSAA live native output differs from CPU oracle");
    }
};
