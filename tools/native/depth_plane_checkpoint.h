#pragma once
// Development-only semantic D32S8 readback. Padding bits and GPU row padding are excluded.
#include "texel_checkpoint.h"

namespace flora::diagnostic {
inline std::vector<uint8_t> readDepthPlane(ID3D11Device* device,ID3D11Texture2D* source,
                                         uint32_t sample,uint32_t layer,uint64_t budget=uint64_t(128)<<20) {
    D3D11_TEXTURE2D_DESC description{};source->GetDesc(&description);
    if(description.Format!=DXGI_FORMAT_R32G8X24_TYPELESS || description.MipLevels!=1 ||
       description.SampleDesc.Count<=1 || sample>=description.SampleDesc.Count || layer>=description.ArraySize)
        throw std::runtime_error("Invalid depth plane selection");
    const uint64_t rowBytes=uint64_t(description.Width)*8,bytes=rowBytes*description.Height;
    if(!bytes||bytes>budget||budget>(uint64_t(128)<<20))throw std::runtime_error("Depth plane read budget exceeded");
    const auto compile=[](const std::string& text,const char* profile){
        Com<ID3DBlob> code,error;check(D3DCompile(text.data(),text.size(),"Depth plane checkpoint",nullptr,nullptr,"main",profile,D3DCOMPILE_ENABLE_STRICTNESS,0,&code,&error),"Compile depth plane reader");return code;
    };
    const auto vsCode=compile("float4 main(uint i:SV_VertexID):SV_Position{return float4(i==2?3:-1,i==1?3:-1,0,1);}","vs_5_0");
    const auto coordinate="int3(int2(p.xy),"+std::to_string(layer)+"),"+std::to_string(sample);
    const auto psCode=compile("Texture2DMSArray<float> depth:register(t0);Texture2DMSArray<uint2> stencil:register(t1);uint2 main(float4 p:SV_Position):SV_Target{return uint2(asuint(depth.Load("+coordinate+")),stencil.Load("+coordinate+").y);}","ps_5_0");
    Com<ID3D11VertexShader> vs;Com<ID3D11PixelShader> ps;
    check(device->CreateVertexShader(vsCode->GetBufferPointer(),vsCode->GetBufferSize(),nullptr,&vs),"Create depth plane VS");
    check(device->CreatePixelShader(psCode->GetBufferPointer(),psCode->GetBufferSize(),nullptr,&ps),"Create depth plane PS");
    D3D11_SHADER_RESOURCE_VIEW_DESC view{};view.Format=DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;view.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY;view.Texture2DMSArray.ArraySize=description.ArraySize;
    Com<ID3D11ShaderResourceView> depth,stencil;
    check(device->CreateShaderResourceView(source,&view,&depth),"Create depth plane SRV");view.Format=DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;
    check(device->CreateShaderResourceView(source,&view,&stencil),"Create stencil plane SRV");
    D3D11_TEXTURE2D_DESC td{description.Width,description.Height,1,1,DXGI_FORMAT_R32G32_UINT,{1,0},D3D11_USAGE_DEFAULT,D3D11_BIND_RENDER_TARGET,0,0};
    Com<ID3D11Texture2D> output,staging;check(device->CreateTexture2D(&td,nullptr,&output),"Create depth plane output");
    Com<ID3D11RenderTargetView> rtv;check(device->CreateRenderTargetView(output.Get(),nullptr,&rtv),"Create depth plane RTV");
    td.Usage=D3D11_USAGE_STAGING;td.BindFlags=0;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    check(device->CreateTexture2D(&td,nullptr,&staging),"Create depth plane staging");
    Com<ID3D11DeviceContext> deferred,immediate;check(device->CreateDeferredContext(0,&deferred),"Create depth plane context");device->GetImmediateContext(&immediate);requireUnpredicated(immediate.Get());
    deferred->VSSetShader(vs.Get(),nullptr,0);deferred->PSSetShader(ps.Get(),nullptr,0);deferred->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const D3D11_VIEWPORT viewport{0,0,float(td.Width),float(td.Height),0,1};deferred->RSSetViewports(1,&viewport);
    D3D11_RASTERIZER_DESC rd{};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=TRUE;
    Com<ID3D11RasterizerState> raster;check(device->CreateRasterizerState(&rd,&raster),"Create depth plane rasterizer");deferred->RSSetState(raster.Get());
    ID3D11ShaderResourceView* inputs[]{depth.Get(),stencil.Get()};deferred->PSSetShaderResources(0,2,inputs);auto target=rtv.Get();deferred->OMSetRenderTargets(1,&target,nullptr);
    deferred->Draw(3,0);deferred->CopyResource(staging.Get(),output.Get());
    Com<ID3D11CommandList> commands;check(deferred->FinishCommandList(FALSE,&commands),"Finish depth plane commands");immediate->ExecuteCommandList(commands.Get(),TRUE);
    std::vector<uint8_t> result(static_cast<size_t>(bytes));
    D3D11_MAPPED_SUBRESOURCE mapped{};check(immediate->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped),"Map depth plane");
    const bool valid=mapped.pData&&mapped.RowPitch>=rowBytes;
    if(valid)for(UINT y=0;y<td.Height;++y)std::memcpy(result.data()+size_t(y)*size_t(rowBytes),static_cast<const uint8_t*>(mapped.pData)+size_t(y)*mapped.RowPitch,size_t(rowBytes));
    immediate->Unmap(staging.Get(),0);if(!valid)throw std::runtime_error("Invalid depth plane row pitch");
    check(device->GetDeviceRemovedReason(),"Depth plane device status");return result;
}
} // namespace flora::diagnostic
