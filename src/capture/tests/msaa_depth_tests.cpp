#include "msaa_readback.hpp"
#include "objects.hpp"
#include "command_stream.hpp"
#include <d3dcompiler.h>
#include <d3d11sdklayers.h>
#include <wrl/client.h>
#include <bit>
#include <iostream>
using namespace flora::capture;
using Microsoft::WRL::ComPtr;
namespace {
unsigned checks=0,cases=0;
unsigned native_unorm_cpu_differences=0;
std::string phase;
void require(bool value,const char* why){if(!value)throw std::runtime_error(phase+": "+why);++checks;}
void check(HRESULT hr){if(FAILED(hr))throw std::runtime_error(phase+": HRESULT "+std::to_string(uint32_t(hr)));}
template<class F>void rejects(F f,const char* why){try{f();}catch(const std::exception& e){require(std::string(e.what()).find(why)!=std::string::npos,e.what());return;}throw std::runtime_error("Expected rejection");}
ComPtr<ID3DBlob> compile(const char* source,const char* profile){ComPtr<ID3DBlob> code,errors;const auto hr=D3DCompile(source,strlen(source),"MSAA depth producer",nullptr,nullptr,"main",profile,D3DCOMPILE_ENABLE_STRICTNESS,0,&code,&errors);if(FAILED(hr))throw std::runtime_error(errors?static_cast<const char*>(errors->GetBufferPointer()):"Compile failed");return code;}
constexpr UINT width=67,height=19,layers=2;
UINT numerator(UINT x,UINT y,UINT layer,UINT sample){return x==0?0:x==1?16777216:x==2?8388608:x==3?1:x==4?16777215:(x*0x45d9f3bu+y*0x119de1f3u+layer*0x12345u+sample*0x5a37u+1)&0xffffff;}
UINT stencil(UINT x,UINT y,UINT layer,UINT sample){return (x*13+y*7+layer*97+sample*19)&255;}
void verify(const MsaaSamples& data,DXGI_FORMAT typed,DXGI_FORMAT original,UINT samples,const std::vector<std::vector<uint8_t>>& oracle,bool count_quantization=false){
    const bool packed=typed==DXGI_FORMAT_D32_FLOAT_S8X24_UINT,normalized=typed==DXGI_FORMAT_D16_UNORM || typed==DXGI_FORMAT_D24_UNORM_S8_UINT;
    const UINT size=typed==DXGI_FORMAT_D16_UNORM?2:packed?8:4;
    const auto encoding=typed==DXGI_FORMAT_D16_UNORM?MsaaSampleEncoding::depth16:typed==DXGI_FORMAT_D32_FLOAT?MsaaSampleEncoding::depth32:packed?MsaaSampleEncoding::depth32_stencil8:MsaaSampleEncoding::depth24_stencil8;
    require(data.encoding==encoding && data.shape.format==original && data.shape.samples==samples && data.quality==0,"Depth identity/encoding lost");
    require(data.planes.size()==layers*samples,"Missing depth sample planes");
    for(UINT layer=0;layer<layers;++layer)for(UINT sample=0;sample<samples;++sample){
        const auto& p=data.planes[layer*samples+sample];require(p.array_slice==layer && p.sample==sample && p.row_bytes==width*size,"Depth plane layout mismatch");
        std::vector<uint8_t> expected;
        for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x){
            const auto n=numerator(x,y,layer,sample);
            uint32_t depth=normalized?uint32_t((uint64_t(n)*(size==2?65535:16777215)+8388608)/16777216):std::bit_cast<uint32_t>(float(n)/16777216);
            if(typed==DXGI_FORMAT_D24_UNORM_S8_UINT)depth|=stencil(x,y,layer,sample)<<24;
            for(UINT b=0;b<(size==2?2:4);++b)expected.push_back(uint8_t(depth>>(8*b)));
            if(packed){expected.push_back(uint8_t(stencil(x,y,layer,sample)));expected.insert(expected.end(),3,0);}
        }
        const auto& native_expected=oracle.at(layer*samples+sample);
        require(native_expected.size()==expected.size(),"Independent native oracle size mismatch");
        // Raster depth quantization at a half step can differ from the CPU's
        // round-up formula. The independently rendered single-sample producer
        // supplies exact native storage, never the helper being tested.
        if(!normalized)require(native_expected==expected,"Native float depth/stencil differs from CPU oracle");
        else for(size_t at=0;at<expected.size();at+=size){
            if(count_quantization && memcmp(native_expected.data()+at,expected.data()+at,size==2?2:3))++native_unorm_cpu_differences;
            if(size==4)require(native_expected[at+3]==expected[at+3],"Native stencil differs from CPU oracle");
        }
        if(p.bytes!=native_expected){for(size_t i=0;i<expected.size();++i)if(p.bytes.at(i)!=native_expected[i]){std::cerr<<"layer="<<layer<<" sample="<<sample<<" byte="<<i<<" actual="<<UINT(p.bytes[i])<<" expected="<<UINT(native_expected[i])<<'\n';break;}}
        require(p.bytes==native_expected,"Depth/stencil sample differs from independent exact native storage");
    }
}
void run(ID3D11Device* device,ID3D11DeviceContext* context,DXGI_FORMAT typed,bool typeless,UINT samples){
    phase="depth format "+std::to_string(UINT(typed))+" typeless "+std::to_string(typeless)+" samples "+std::to_string(samples);
    const auto access=msaa_sample_access(typed,true);const bool has_stencil=access.stencil_view!=DXGI_FORMAT_UNKNOWN;
    D3D11_TEXTURE2D_DESC desc{};desc.Width=width;desc.Height=height;desc.MipLevels=1;desc.ArraySize=layers;desc.Format=typeless?access.storage:typed;desc.SampleDesc.Count=samples;desc.BindFlags=D3D11_BIND_DEPTH_STENCIL;
    ComPtr<ID3D11Texture2D> texture;check(device->CreateTexture2D(&desc,nullptr,&texture));
    auto code=compile("float4 main(uint i:SV_VertexID):SV_Position{return float4(i==2?3:-1,i==1?3:-1,0,1);}","vs_5_0");ComPtr<ID3D11VertexShader> vs;check(device->CreateVertexShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&vs));
    code=compile(R"(cbuffer Values:register(b0){uint layer,sample,bit,pad;}
float main(float4 p:SV_Position):SV_Depth {
 uint x=uint(p.x),y=uint(p.y);
 if(bit!=0){uint s=(x*13+y*7+layer*97+sample*19)&255;if((s&bit)==0)discard;return 0;}
 uint n=x==0?0:x==1?16777216:x==2?8388608:x==3?1:x==4?16777215:(x*0x45d9f3bu+y*0x119de1f3u+layer*0x12345u+sample*0x5a37u+1)&0xffffff;return float(n)/16777216;
})","ps_5_0");ComPtr<ID3D11PixelShader> ps;check(device->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&ps));
    D3D11_BUFFER_DESC bd{};bd.ByteWidth=16;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;ComPtr<ID3D11Buffer> constants;check(device->CreateBuffer(&bd,nullptr,&constants));auto* cb=constants.Get();
    D3D11_RASTERIZER_DESC rd{};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=TRUE;rd.MultisampleEnable=TRUE;ComPtr<ID3D11RasterizerState> rs;check(device->CreateRasterizerState(&rd,&rs));
    D3D11_DEPTH_STENCIL_DESC ds{};ds.DepthEnable=TRUE;ds.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;ds.DepthFunc=D3D11_COMPARISON_ALWAYS;
    ComPtr<ID3D11DepthStencilState> depth;check(device->CreateDepthStencilState(&ds,&depth));
    context->ClearState();context->VSSetShader(vs.Get(),nullptr,0);context->PSSetShader(ps.Get(),nullptr,0);context->PSSetConstantBuffers(0,1,&cb);context->RSSetState(rs.Get());context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D11_VIEWPORT vp{0,0,float(width),float(height),0,1};context->RSSetViewports(1,&vp);
    D3D11_TEXTURE2D_DESC reference_desc=desc;reference_desc.SampleDesc.Count=1;reference_desc.ArraySize=layers*samples;
    ComPtr<ID3D11Texture2D> reference;check(device->CreateTexture2D(&reference_desc,nullptr,&reference));
    for(bool single:{true,false})for(UINT layer=0;layer<layers;++layer){
        for(UINT sample=0;sample<samples;++sample){
            D3D11_DEPTH_STENCIL_VIEW_DESC vd{};vd.Format=typed;
            if(single){vd.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2DARRAY;vd.Texture2DArray={0,layer*samples+sample,1};}
            else{vd.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2DMSARRAY;vd.Texture2DMSArray={layer,1};}
            ComPtr<ID3D11DepthStencilView> view;check(device->CreateDepthStencilView(single?reference.Get():texture.Get(),&vd,&view));
            if(single || sample==0)context->ClearDepthStencilView(view.Get(),D3D11_CLEAR_DEPTH|(has_stencil?D3D11_CLEAR_STENCIL:0),0,0);
            context->OMSetRenderTargets(0,nullptr,view.Get());context->OMSetBlendState(nullptr,nullptr,single?UINT_MAX:1u<<sample);context->OMSetDepthStencilState(depth.Get(),0);UINT values[]{layer,sample,0,0};context->UpdateSubresource(constants.Get(),0,nullptr,values,0,0);context->Draw(3,0);
            if(has_stencil)for(UINT bit=1;bit<=128;bit*=2){
                D3D11_DEPTH_STENCIL_DESC sd{};sd.DepthEnable=FALSE;sd.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ZERO;sd.DepthFunc=D3D11_COMPARISON_ALWAYS;sd.StencilEnable=TRUE;sd.StencilReadMask=255;sd.StencilWriteMask=uint8_t(bit);sd.FrontFace={D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_REPLACE,D3D11_COMPARISON_ALWAYS};sd.BackFace=sd.FrontFace;
                ComPtr<ID3D11DepthStencilState> state;check(device->CreateDepthStencilState(&sd,&state));context->OMSetDepthStencilState(state.Get(),255);values[2]=bit;context->UpdateSubresource(constants.Get(),0,nullptr,values,0,0);context->Draw(3,0);
            }
        }
    }
    // Independent application storage oracle: ordinary native Copy/Map only,
    // with no multisample shader read, snapshot helper or resolved pixels.
    reference_desc.Usage=D3D11_USAGE_STAGING;reference_desc.BindFlags=0;reference_desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;check(unwrap(device)->CreateTexture2D(&reference_desc,nullptr,&staging));auto* native=unwrap(context);native->CopyResource(staging.Get(),unwrap(reference.Get()));
    const UINT pixel_bytes=typed==DXGI_FORMAT_D16_UNORM?2:typed==DXGI_FORMAT_D32_FLOAT_S8X24_UINT?8:4;
    std::vector<std::vector<uint8_t>> oracle;
    for(UINT index=0;index<layers*samples;++index){D3D11_MAPPED_SUBRESOURCE map{};check(native->Map(staging.Get(),index,D3D11_MAP_READ,0,&map));
        std::vector<uint8_t> bytes(size_t(width)*height*pixel_bytes);for(UINT y=0;y<height;++y)memcpy(bytes.data()+size_t(y)*width*pixel_bytes,static_cast<const uint8_t*>(map.pData)+size_t(y)*map.RowPitch,width*pixel_bytes);native->Unmap(staging.Get(),index);
        if(pixel_bytes==8)for(size_t at=5;at<bytes.size();at+=8)memset(bytes.data()+at,0,3);
        oracle.push_back(std::move(bytes));}
    ComPtr<ID3D11DepthStencilView> bound;ComPtr<ID3D11DepthStencilState> state;UINT stencil_reference=0;context->OMGetRenderTargets(0,nullptr,&bound);context->OMGetDepthStencilState(&state,&stencil_reference);
    const auto bindings=[&]{ComPtr<ID3D11DepthStencilView> actual;ComPtr<ID3D11DepthStencilState> actual_state;UINT actual_ref=0;context->OMGetRenderTargets(0,nullptr,&actual);context->OMGetDepthStencilState(&actual_state,&actual_ref);require(actual==bound && actual_state==state && actual_ref==stencil_reference,"Depth/stencil state identity changed");ComPtr<ID3D11BlendState> blend;FLOAT factor[4]{};UINT mask=0;context->OMGetBlendState(&blend,factor,&mask);require(!blend && mask==(1u<<(samples-1)),"Sample mask changed");};
    const auto objects=object_status().at("created");auto recording=begin_recording(context);SetLastError(0x7451);
    auto data=snapshot_msaa_samples(context,texture.Get());require(GetLastError()==0x7451,"Depth readback changed LastError");auto frame=recording->finish();frame.require_complete();require(frame.commands.empty() && frame.initial_resources.empty(),"Depth helper entered application stream");recording.reset();
    verify(data,typed,desc.Format,samples,oracle,true);bindings();require(object_status().at("created")==objects,"Depth helper objects registered");
    SnapshotOptions limited;limited.byte_limit=data.planes.size()*data.planes[0].bytes.size()-1;rejects([&]{snapshot_msaa_samples(context,texture.Get(),limited);},"byte limit");
    SnapshotOptions cancelled;UINT calls=0;cancelled.cancelled=[&]{return ++calls==8;};rejects([&]{snapshot_msaa_samples(context,texture.Get(),cancelled);},"cancelled");bindings();verify(snapshot_msaa_samples(context,texture.Get()),typed,desc.Format,samples,oracle);
    rejects([&]{snapshot_resource(context,texture.Get());},"MSAA");
    auto lifetime=object_record(texture.Get());context->ClearState();bound.Reset();state.Reset();texture.Reset();context->Flush();require(!lifetime->alive,"Depth sample data retained COM source");verify(data,typed,desc.Format,samples,oracle);++cases;
}
}
int main(int argc,char** argv){try{
    const bool hardware=argc>1 && std::string(argv[1])=="--hardware";ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    check(D3D11CreateDevice(nullptr,hardware?D3D_DRIVER_TYPE_HARDWARE:D3D_DRIVER_TYPE_WARP,nullptr,D3D11_CREATE_DEVICE_DEBUG,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context));ComPtr<ID3D11InfoQueue> info;check(device.As(&info));info->ClearStoredMessages();wrap_object(device.GetAddressOf(),"MSAA depth device");wrap_object(context.GetAddressOf(),"MSAA depth context");
    for(auto format:{DXGI_FORMAT_D16_UNORM,DXGI_FORMAT_D32_FLOAT,DXGI_FORMAT_D24_UNORM_S8_UINT,DXGI_FORMAT_D32_FLOAT_S8X24_UINT})for(bool typeless:{false,true})for(UINT samples:{2u,4u})run(device.Get(),context.Get(),format,typeless,samples);
    for(UINT64 i=0;i<info->GetNumStoredMessagesAllowedByRetrievalFilter();++i){SIZE_T size=0;check(info->GetMessage(i,nullptr,&size));std::vector<uint8_t> bytes(size);auto* m=reinterpret_cast<D3D11_MESSAGE*>(bytes.data());check(info->GetMessage(i,m,&size));if(m->Severity<=D3D11_MESSAGE_SEVERITY_WARNING)throw std::runtime_error(m->pDescription);}
    require(object_status().at("interface_gaps")==0,"Unexpected interface gaps");context.Reset();device.Reset();info.Reset();require(object_status().at("active")==0 && metadata_status().at("alive")==0,"Depth sample capture leaked native objects");
    std::cout<<Json{{"passed",true},{"checks",checks},{"cases",cases},{"hardware",hardware},{"debug_warnings_or_errors",0},{"native_unorm_cpu_differences",native_unorm_cpu_differences},{"scope","Exact native depth/stencil storage oracle; unused D32S8 padding canonicalized to zero; no MSAA frame encoding"}}.dump()<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<phase<<": "<<e.what()<<'\n';return 1;}}
