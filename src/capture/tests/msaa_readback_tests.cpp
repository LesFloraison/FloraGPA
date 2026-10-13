#include "msaa_readback.hpp"
#include "objects.hpp"
#include "command_stream.hpp"
#include <d3dcompiler.h>
#include <d3d11sdklayers.h>
#include <wrl/client.h>
#include <array>
#include <cstring>
#include <iostream>

using namespace flora::capture;
using Microsoft::WRL::ComPtr;
namespace {
unsigned checks=0,cases=0;
std::string phase;
void require(bool value,const char* why){if(!value)throw std::runtime_error(phase+": "+why);++checks;}
void hr(HRESULT value){if(FAILED(value))throw std::runtime_error(phase+": HRESULT "+std::to_string(uint32_t(value)));}
template<class F>void rejects(F action,const char* reason){try{action();}catch(const std::exception& e){require(std::string(e.what()).find(reason)!=std::string::npos,e.what());return;}throw std::runtime_error("Expected refusal");}
ComPtr<ID3DBlob> compile(const std::string& source,const char* profile){
    ComPtr<ID3DBlob> code,errors;const auto result=D3DCompile(source.data(),source.size(),"MSAA independent producer",nullptr,nullptr,"main",profile,D3DCOMPILE_ENABLE_STRICTNESS,0,&code,&errors);
    if(FAILED(result))throw std::runtime_error(errors?static_cast<const char*>(errors->GetBufferPointer()):"Compile failed");return code;
}
constexpr UINT width=7,height=5,layers=2;
uint32_t expected(UINT x,UINT y,UINT layer,UINT sample,UINT component){
    constexpr uint32_t seeds[]{0x7fc12345u,0x80000000u,1u,0x7f800000u,0xff800000u,0x00007e15u,0x00008000u};
    return (seeds[x] ^ y*257u ^ sample*0x01020304u ^ layer*0x11223344u)+component*0x01020307u;
}
void verify(const MsaaSamples& data,const D3D11_TEXTURE2D_DESC& d){
    const auto access=msaa_integer_access(d.Format);
    require(data.shape.format==d.Format && data.shape.samples==d.SampleDesc.Count && data.quality==d.SampleDesc.Quality,"Original format/sample quality changed");
    require(data.planes.size()==layers*d.SampleDesc.Count,"Missing sample planes");
    for(UINT l=0;l<layers;++l)for(UINT s=0;s<d.SampleDesc.Count;++s){
        const auto& p=data.planes[l*d.SampleDesc.Count+s];
        require(p.array_slice==l && p.sample==s && p.row_bytes==width*access.components*access.component_bytes,"Sample/layer addressing lost");
        std::vector<uint8_t> bytes;
        for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x)for(UINT c=0;c<access.components;++c){
            const auto value=expected(x,y,l,s,c);for(UINT b=0;b<access.component_bytes;++b)bytes.push_back(uint8_t(value>>(8*b)));
        }
        require(p.bytes==bytes,"Sample bytes differ from independent CPU oracle");
    }
}
ComPtr<ID3D11Texture2D> produce(ID3D11Device* device,ID3D11DeviceContext* context,DXGI_FORMAT format,UINT samples){
    phase="format "+std::to_string(UINT(format))+" samples "+std::to_string(samples);
    const auto access=msaa_integer_access(format);
    D3D11_TEXTURE2D_DESC d{};d.Width=width;d.Height=height;d.ArraySize=layers;d.MipLevels=1;d.Format=access.storage;d.SampleDesc.Count=samples;d.BindFlags=D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> producer,result;hr(device->CreateTexture2D(&d,nullptr,&producer));
    auto vs_code=compile("float4 main(uint i:SV_VertexID):SV_Position{return float4(i==2?3:-1,i==1?3:-1,0,1);}","vs_5_0");
    ComPtr<ID3D11VertexShader> vs;hr(device->CreateVertexShader(vs_code->GetBufferPointer(),vs_code->GetBufferSize(),nullptr,&vs));
    const uint32_t mask=access.component_bytes==4?UINT_MAX:((1u<<(8*access.component_bytes))-1);
    auto ps_code=compile("cbuffer Layer:register(b0){uint layer;}uint4 main(float4 p:SV_Position,uint s:SV_SampleIndex):SV_Target{uint seeds[7]={0x7fc12345u,0x80000000u,1u,0x7f800000u,0xff800000u,0x7e15u,0x8000u};uint base=seeds[uint(p.x)] ^ uint(p.y)*257u ^ s*0x01020304u ^ layer*0x11223344u;return (base+uint4(0,0x01020307u,0x0204060eu,0x03060915u))&"+std::to_string(mask)+"u;}","ps_5_0");
    ComPtr<ID3D11PixelShader> ps;hr(device->CreatePixelShader(ps_code->GetBufferPointer(),ps_code->GetBufferSize(),nullptr,&ps));
    D3D11_BUFFER_DESC bd{};bd.ByteWidth=16;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;ComPtr<ID3D11Buffer> constants;hr(device->CreateBuffer(&bd,nullptr,&constants));
    D3D11_RASTERIZER_DESC rd{};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=TRUE;rd.MultisampleEnable=TRUE;
    ComPtr<ID3D11RasterizerState> raster;hr(device->CreateRasterizerState(&rd,&raster));
    context->ClearState();context->VSSetShader(vs.Get(),nullptr,0);context->PSSetShader(ps.Get(),nullptr,0);context->RSSetState(raster.Get());
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);D3D11_VIEWPORT vp{0,0,float(width),float(height),0,1};context->RSSetViewports(1,&vp);
    auto* cb=constants.Get();context->PSSetConstantBuffers(0,1,&cb);
    for(UINT layer=0;layer<layers;++layer){
        const UINT values[]{layer,0,0,0};context->UpdateSubresource(constants.Get(),0,nullptr,values,0,0);
        D3D11_RENDER_TARGET_VIEW_DESC vd{};vd.Format=access.view;vd.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY;vd.Texture2DMSArray={layer,1};
        ComPtr<ID3D11RenderTargetView> rtv;hr(device->CreateRenderTargetView(producer.Get(),&vd,&rtv));auto* v=rtv.Get();context->OMSetRenderTargets(1,&v,nullptr);context->Draw(3,0);
    }
    context->OMSetRenderTargets(0,nullptr,nullptr);
    d.Format=format;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;hr(device->CreateTexture2D(&d,nullptr,&result));context->CopyResource(result.Get(),producer.Get());
    return result;
}
void layout_checks(){
    ResourceShape shape;shape.dimension=D3D11_RESOURCE_DIMENSION_TEXTURE2D;shape.width=width;shape.height=height;shape.samples=4;shape.array_size=layers;shape.format=DXGI_FORMAT_R8G8B8A8_UNORM;
    const auto exact=size_t(width)*height*4*4*layers;
    auto layout=msaa_sample_layout(shape,0,exact);require(layout.planes.size()==8 && layout.planes[0].bytes.empty(),"Layout allocates payload before GPU success");
    rejects([&]{msaa_sample_layout(shape,0,exact-1);},"byte limit");
    shape.samples=3;rejects([&]{msaa_sample_layout(shape,0);},"dimensions");shape.samples=4;
    shape.array_size=2049;rejects([&]{msaa_sample_layout(shape,0);},"dimensions");shape.array_size=layers;
    shape.misc_flags=D3D11_RESOURCE_MISC_SHARED;rejects([&]{msaa_sample_layout(shape,0);},"special");shape.misc_flags=0;
    shape.width=16384;shape.height=16384;rejects([&]{msaa_sample_layout(shape,0,SIZE_MAX);},"workspace");
    for(auto format:{DXGI_FORMAT_D16_UNORM,DXGI_FORMAT_D32_FLOAT,DXGI_FORMAT_D24_UNORM_S8_UINT,DXGI_FORMAT_B8G8R8A8_UNORM,DXGI_FORMAT_R10G10B10A2_UNORM})rejects([&]{msaa_integer_access(format);},"MSAA");
}
void preserved(ID3D11DeviceContext* context,ID3D11PixelShader* ps,ID3D11Buffer* cb){
    ComPtr<ID3D11PixelShader> current;context->PSGetShader(&current,nullptr,nullptr);require(current.Get()==ps,"PS identity changed");
    ComPtr<ID3D11Buffer> constant;context->PSGetConstantBuffers(0,1,&constant);require(constant.Get()==cb,"CB identity changed");
    D3D11_PRIMITIVE_TOPOLOGY topology{};context->IAGetPrimitiveTopology(&topology);require(topology==D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST,"Topology changed");
    UINT count=1;D3D11_VIEWPORT vp{};context->RSGetViewports(&count,&vp);require(count==1 && vp.Width==width && vp.Height==height,"Viewport changed");
}
void run(ID3D11Device* device,ID3D11DeviceContext* context){
    for(auto format:{DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,DXGI_FORMAT_R16G16B16A16_FLOAT,
        DXGI_FORMAT_R32G32B32A32_FLOAT,DXGI_FORMAT_R32G32_UINT,DXGI_FORMAT_R16G16_SINT,
        DXGI_FORMAT_R32_FLOAT,DXGI_FORMAT_R8G8_SNORM,DXGI_FORMAT_R16_FLOAT,DXGI_FORMAT_R8_UNORM})for(UINT samples:{2u,4u}){
        auto texture=produce(device,context,format,samples);D3D11_TEXTURE2D_DESC desc{};texture->GetDesc(&desc);
        ComPtr<ID3D11PixelShader> ps;context->PSGetShader(&ps,nullptr,nullptr);ComPtr<ID3D11Buffer> cb;context->PSGetConstantBuffers(0,1,&cb);
        const auto created=object_status().at("created");
        auto recording=begin_recording(context);SetLastError(0x18c27);
        auto data=snapshot_msaa_samples(context,texture.Get());require(GetLastError()==0x18c27,"Snapshot changed LastError");
        auto frame=recording->finish();frame.require_complete();require(frame.commands.empty() && frame.initial_resources.empty(),"Internal compute entered app command stream");recording.reset();
        require(object_status().at("created")==created,"Internal objects entered application registry");
        require(data.resource==object_record(texture.Get())->id,"Source identity lost");verify(data,desc);preserved(context,ps.Get(),cb.Get());
        const auto again=snapshot_msaa_samples(context,texture.Get());verify(again,desc);
        SnapshotOptions limited;limited.byte_limit=1;rejects([&]{snapshot_msaa_samples(context,texture.Get(),limited);},"byte limit");preserved(context,ps.Get(),cb.Get());
        SnapshotOptions timed;timed.timeout_ms=0;rejects([&]{snapshot_msaa_samples(context,texture.Get(),timed);},"timed out");
        SnapshotOptions cancel;UINT polls=0;cancel.cancelled=[&]{return ++polls==7;};rejects([&]{snapshot_msaa_samples(context,texture.Get(),cancel);},"cancelled");preserved(context,ps.Get(),cb.Get());verify(snapshot_msaa_samples(context,texture.Get()),desc);
        // Production publication remains refused until a separate encoder path
        // actually restores samples in both players.
        rejects([&]{snapshot_resource(context,texture.Get());},"MSAA");
        auto lifetime=object_record(texture.Get());context->ClearState();texture.Reset();require(!lifetime->alive,"Sample model retained source COM lifetime");verify(data,desc);++cases;
    }
    auto texture=produce(device,context,DXGI_FORMAT_R8G8B8A8_UNORM,4);
    D3D11_QUERY_DESC qd{D3D11_QUERY_PIPELINE_STATISTICS,0};ComPtr<ID3D11Query> query;hr(device->CreateQuery(&qd,&query));
    context->Begin(query.Get());rejects([&]{snapshot_msaa_samples(context,texture.Get());},"active query");context->End(query.Get());
    D3D11_QUERY_DATA_PIPELINE_STATISTICS stats{};HRESULT ready=S_FALSE;const auto deadline=GetTickCount64()+5000;
    while((ready=context->GetData(query.Get(),&stats,sizeof(stats),0))==S_FALSE && GetTickCount64()<deadline)Sleep(1);
    hr(ready);require(ready==S_OK && stats.CSInvocations==0,"Rejected readback affected active query");
    qd.Query=D3D11_QUERY_OCCLUSION_PREDICATE;ComPtr<ID3D11Predicate> predicate;hr(device->CreatePredicate(&qd,&predicate));context->Begin(predicate.Get());context->End(predicate.Get());context->SetPredication(predicate.Get(),TRUE);
    rejects([&]{snapshot_msaa_samples(context,texture.Get());},"predication");ComPtr<ID3D11Predicate> current;BOOL value=FALSE;context->GetPredication(&current,&value);require(current==predicate && value==TRUE,"Rejected readback changed predicate");context->SetPredication(nullptr,FALSE);
    ComPtr<ID3D11DeviceContext> deferred;hr(device->CreateDeferredContext(0,&deferred));rejects([&]{snapshot_msaa_samples(deferred.Get(),texture.Get());},"immediate");
    D3D11_TEXTURE2D_DESC desc{};texture->GetDesc(&desc);
    ComPtr<ID3D11Texture2D> unknown;hr(unwrap(device)->CreateTexture2D(&desc,nullptr,&unknown));
    const auto registered=object_status().at("created");rejects([&]{snapshot_msaa_samples(context,unknown.Get());},"not tracked");require(object_status().at("created")==registered,"Readback registered an unknown native resource");
    ComPtr<ID3D11Device> other;ComPtr<ID3D11DeviceContext> other_context;
    hr(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&other,nullptr,&other_context));
    wrap_object(other_context.GetAddressOf(),"MSAA other-device context");rejects([&]{snapshot_msaa_samples(other_context.Get(),texture.Get());},"device mismatch");
    desc.Format=DXGI_FORMAT_R32_TYPELESS;desc.BindFlags=D3D11_BIND_DEPTH_STENCIL;ComPtr<ID3D11Texture2D> depth;hr(device->CreateTexture2D(&desc,nullptr,&depth));
    SnapshotOptions depth_limit;depth_limit.byte_limit=1;rejects([&]{snapshot_msaa_samples(context,depth.Get(),depth_limit);},"byte limit");
    // No shader readback may run inside a query issued through a known wrapper;
    // raw/interface-escape histories are refused by the runtime gap guard.
    context->ClearState();context->Flush();
}
void hidden_state(ID3D11Device* device,ID3D11DeviceContext* context){
    auto texture=produce(device,context,DXGI_FORMAT_R8G8B8A8_UNORM,4);context->ClearState();phase="MSAA hidden-state isolation";
    auto code=compile("cbuffer M:register(b0){float marker;}float4 main():SV_Position{return float4(marker,2,3,1);}","vs_5_0");
    ComPtr<ID3D11VertexShader> vs;hr(device->CreateVertexShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&vs));
    code=compile("struct V{float4 p:SV_Position;};[maxvertexcount(1)]void main(point V p[1],inout PointStream<V> s){s.Append(p[0]);}","gs_5_0");
    D3D11_SO_DECLARATION_ENTRY entry{0,"SV_Position",0,0,4,0};UINT stride=16;ComPtr<ID3D11GeometryShader> gs;
    hr(device->CreateGeometryShaderWithStreamOutput(code->GetBufferPointer(),code->GetBufferSize(),&entry,1,&stride,1,D3D11_SO_NO_RASTERIZED_STREAM,nullptr,&gs));
    std::array<float,24> expected_data;expected_data.fill(-123.f);D3D11_SUBRESOURCE_DATA init{expected_data.data(),0,0};
    D3D11_BUFFER_DESC bd{};bd.ByteWidth=sizeof(expected_data);bd.BindFlags=D3D11_BIND_STREAM_OUTPUT;ComPtr<ID3D11Buffer> stream;hr(device->CreateBuffer(&bd,&init,&stream));
    bd.ByteWidth=16;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;ComPtr<ID3D11Buffer> constant;hr(device->CreateBuffer(&bd,nullptr,&constant));auto* cb=constant.Get();context->VSSetConstantBuffers(0,1,&cb);
    context->VSSetShader(vs.Get(),nullptr,0);context->GSSetShader(gs.Get(),nullptr,0);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
    auto* target=stream.Get();UINT offset=16;context->SOSetTargets(1,&target,&offset);
    auto draw=[&](float marker){const float data[]{marker,0,0,0};context->UpdateSubresource(constant.Get(),0,nullptr,data,0,0);context->Draw(1,0);};
    bd.ByteWidth=256;bd.BindFlags=D3D11_BIND_UNORDERED_ACCESS;bd.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;bd.StructureByteStride=4;
    ComPtr<ID3D11Buffer> cs_data,om_data;hr(device->CreateBuffer(&bd,nullptr,&cs_data));hr(device->CreateBuffer(&bd,nullptr,&om_data));
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};ud.ViewDimension=D3D11_UAV_DIMENSION_BUFFER;ud.Buffer.NumElements=64;ud.Buffer.Flags=D3D11_BUFFER_UAV_FLAG_COUNTER;
    ComPtr<ID3D11UnorderedAccessView> cs,om;hr(device->CreateUnorderedAccessView(cs_data.Get(),&ud,&cs));hr(device->CreateUnorderedAccessView(om_data.Get(),&ud,&om));
    auto* cv=cs.Get();auto* ov=om.Get();const UINT ca=37,oa=53;context->CSSetUnorderedAccessViews(0,1,&cv,&ca);context->OMSetRenderTargetsAndUnorderedAccessViews(0,nullptr,nullptr,2,1,&ov,&oa);
    draw(7);auto samples=snapshot_msaa_samples(context,texture.Get());draw(9);
    SnapshotOptions cancel;UINT polls=0;cancel.cancelled=[&]{return ++polls==7;};rejects([&]{snapshot_msaa_samples(context,texture.Get(),cancel);},"cancelled");draw(11);
    ComPtr<ID3D11Buffer> bound;context->SOGetTargets(1,&bound);require(bound==stream,"MSAA readback lost SO identity");
    ComPtr<ID3D11UnorderedAccessView> cbound,obound;context->CSGetUnorderedAccessViews(0,1,&cbound);context->OMGetRenderTargetsAndUnorderedAccessViews(0,nullptr,nullptr,2,1,&obound);require(cbound==cs && obound==om,"MSAA readback lost UAV identity");
    context->SOSetTargets(0,nullptr,nullptr);offset=UINT_MAX;context->SOSetTargets(1,&target,&offset);draw(13);context->SOSetTargets(0,nullptr,nullptr);
    for(UINT i=0;i<4;++i){expected_data[(i+1)*4]=float(7+i*2);expected_data[(i+1)*4+1]=2;expected_data[(i+1)*4+2]=3;expected_data[(i+1)*4+3]=1;}
    auto data=snapshot_resource(context,stream.Get());require(data.subresources[0].bytes.size()==sizeof(expected_data) && !memcmp(data.subresources[0].bytes.data(),expected_data.data(),sizeof(expected_data)),"MSAA readback changed SO append cursor/bytes");
    require(snapshot_uav_counter(context,cs.Get()).value==37 && snapshot_uav_counter(context,om.Get()).value==53,"MSAA readback changed hidden UAV counters");
    code=compile("RWStructuredBuffer<uint> value:register(u0);[numthreads(1,1,1)]void main(){uint n=value.IncrementCounter();value[0]=n;}","cs_5_0");
    ComPtr<ID3D11ComputeShader> shader;hr(device->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&shader));context->CSSetShader(shader.Get(),nullptr,0);context->Dispatch(1,1,1);
    require(snapshot_uav_counter(context,cs.Get()).value==38 && snapshot_uav_counter(context,om.Get()).value==53,"Post-snapshot counter increment failed");
    D3D11_TEXTURE2D_DESC desc{};texture->GetDesc(&desc);verify(samples,desc);context->ClearState();context->Flush();
}
}
int main(int argc,char** argv){try{
    bool hardware=false;for(int i=1;i<argc;++i)if(std::string(argv[i])=="--hardware")hardware=true;
    layout_checks();ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    hr(D3D11CreateDevice(nullptr,hardware?D3D_DRIVER_TYPE_HARDWARE:D3D_DRIVER_TYPE_WARP,nullptr,D3D11_CREATE_DEVICE_DEBUG,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context));
    ComPtr<ID3D11InfoQueue> info;hr(device.As(&info));info->ClearStoredMessages();
    wrap_object(device.GetAddressOf(),"MSAA test device");wrap_object(context.GetAddressOf(),"MSAA test context");run(device.Get(),context.Get());hidden_state(device.Get(),context.Get());
    for(UINT64 i=0;i<info->GetNumStoredMessagesAllowedByRetrievalFilter();++i){SIZE_T size=0;hr(info->GetMessage(i,nullptr,&size));std::vector<uint8_t> bytes(size);auto* message=reinterpret_cast<D3D11_MESSAGE*>(bytes.data());hr(info->GetMessage(i,message,&size));
        if(message->Severity<=D3D11_MESSAGE_SEVERITY_WARNING)throw std::runtime_error(std::string("Debug Layer: ")+message->pDescription);}
    require(object_status().at("interface_gaps")==0,"Unexpected interface gaps");context.Reset();device.Reset();info.Reset();
    require(object_status().at("active")==0 && metadata_status().at("alive")==0,"MSAA snapshot retained native objects");
    std::cout<<Json{{"passed",true},{"checks",checks},{"cases",cases},{"hardware",hardware},{"debug_warnings_or_errors",0},{"scope","Owned color MSAA sample readback; no MSAA frame encoding"}}.dump()<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
