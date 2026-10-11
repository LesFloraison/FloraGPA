#include "command_stream.hpp"
#include "objects.hpp"
#include <d3dcompiler.h>
#include <iostream>
#include <thread>
#include <cstring>
#ifdef FLORA_ENCODE_PROBE
#include "encoder.hpp"
#include <fstream>
#endif
using namespace flora::capture;
using Microsoft::WRL::ComPtr;
namespace {
void require(bool value,const char* reason){if(!value)throw std::runtime_error(reason);}
void check(HRESULT hr){require(SUCCEEDED(hr),"Native command oracle HRESULT failed");}
template<class F>void rejects(F f,const char* reason){try{f();}catch(const std::exception& e){require(std::string(e.what()).find(reason)!=std::string::npos,"Wrong recording rejection reason");return;}throw std::runtime_error("Missing recording rejection");}
ComPtr<ID3DBlob> compile(const char* source,const char* profile){ComPtr<ID3DBlob> bytes,errors;check(D3DCompile(source,strlen(source),"command-stream",nullptr,nullptr,"main",profile,0,0,&bytes,&errors));return bytes;}
struct Fixture {
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Texture2D> texture;ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11Buffer> source,destination,indices;
    ComPtr<ID3D11ShaderResourceView> srv;ComPtr<ID3D11UnorderedAccessView> uav;
    ObjectId texture_id=0,source_id=0,destination_id=0;
    explicit Fixture(bool hardware,bool compute_target=false){
        check(D3D11CreateDevice(nullptr,hardware?D3D_DRIVER_TYPE_HARDWARE:D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context));
        wrap_object(device.GetAddressOf(),"test::Device");wrap_object(context.GetAddressOf(),"test::Context");
        D3D11_TEXTURE2D_DESC td{};td.Width=td.Height=8;td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;td.BindFlags=D3D11_BIND_RENDER_TARGET|(compute_target?D3D11_BIND_UNORDERED_ACCESS:0);
        check(device->CreateTexture2D(&td,nullptr,&texture));check(device->CreateRenderTargetView(texture.Get(),nullptr,&rtv));texture_id=object_record(texture.Get())->id;
        const float blue[]{0,0,1,1};context->ClearRenderTargetView(rtv.Get(),blue);
        D3D11_BUFFER_DESC bd{};bd.ByteWidth=16;bd.BindFlags=D3D11_BIND_SHADER_RESOURCE;UINT initial[]{1,2,3,4};D3D11_SUBRESOURCE_DATA data{initial,0,0};
        check(device->CreateBuffer(&bd,&data,&source));source_id=object_record(source.Get())->id;
        const UINT updated[]{17,17,17,17};context->UpdateSubresource(source.Get(),0,nullptr,updated,0,0);
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};sd.Format=DXGI_FORMAT_R32_UINT;sd.ViewDimension=D3D11_SRV_DIMENSION_BUFFER;sd.Buffer.NumElements=4;check(device->CreateShaderResourceView(source.Get(),&sd,&srv));
        bd.BindFlags=D3D11_BIND_UNORDERED_ACCESS;check(device->CreateBuffer(&bd,&data,&destination));destination_id=object_record(destination.Get())->id;
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};ud.Format=DXGI_FORMAT_R32_UINT;ud.ViewDimension=D3D11_UAV_DIMENSION_BUFFER;ud.Buffer.NumElements=4;check(device->CreateUnorderedAccessView(destination.Get(),&ud,&uav));
        bd.ByteWidth=12;bd.BindFlags=D3D11_BIND_INDEX_BUFFER;UINT order[]{1,2,3};data.pSysMem=order;check(device->CreateBuffer(&bd,&data,&indices));context->IASetIndexBuffer(indices.Get(),DXGI_FORMAT_R32_UINT,0);
        auto code=compile("float4 main(uint v:SV_VertexID):SV_Position{return float4(v==2?3:-1,v==1?3:-1,.5,1);}","vs_5_0");ComPtr<ID3D11VertexShader> vs;check(device->CreateVertexShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&vs));context->VSSetShader(vs.Get(),nullptr,0);
        code=compile("float4 main():SV_Target{return float4(1,0,0,1);}","ps_5_0");ComPtr<ID3D11PixelShader> ps;check(device->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&ps));context->PSSetShader(ps.Get(),nullptr,0);
        code=compile("RWBuffer<uint> d:register(u0);Buffer<uint> s:register(t0);[numthreads(1,1,1)]void main(uint3 id:SV_DispatchThreadID){d[id.x]+=s[id.x]+id.x*7;}","cs_5_0");ComPtr<ID3D11ComputeShader> cs;check(device->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&cs));context->CSSetShader(cs.Get(),nullptr,0);
        auto* rt=rtv.Get();context->OMSetRenderTargets(1,&rt,nullptr);D3D11_VIEWPORT vp{0,0,8,8,0,1};context->RSSetViewports(1,&vp);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        auto* sv=srv.Get();auto* uv=uav.Get();context->CSSetShaderResources(0,1,&sv);context->CSSetUnorderedAccessViews(0,1,&uv,nullptr);
    }
    ~Fixture(){context->ClearState();context->Flush();}
};
struct OracleOutput {std::vector<uint8_t> image,buffer;};
std::vector<uint8_t> read(ID3D11Device* device,ID3D11DeviceContext* context,ID3D11Resource* resource){
    ComPtr<ID3D11Resource> staging;ComPtr<ID3D11Buffer> buffer;
    UINT bytes=0,rows=1;
    if(SUCCEEDED(resource->QueryInterface(IID_PPV_ARGS(&buffer)))){D3D11_BUFFER_DESC d{};buffer->GetDesc(&d);bytes=d.ByteWidth;d.BindFlags=d.MiscFlags=d.StructureByteStride=0;d.Usage=D3D11_USAGE_STAGING;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;ComPtr<ID3D11Buffer> target;check(device->CreateBuffer(&d,nullptr,&target));check(target.As(&staging));}
    else{ComPtr<ID3D11Texture2D> texture;check(resource->QueryInterface(IID_PPV_ARGS(&texture)));D3D11_TEXTURE2D_DESC d{};texture->GetDesc(&d);bytes=d.Width*4;rows=d.Height;d.BindFlags=d.MiscFlags=0;d.Usage=D3D11_USAGE_STAGING;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;ComPtr<ID3D11Texture2D> target;check(device->CreateTexture2D(&d,nullptr,&target));check(target.As(&staging));}
    context->CopyResource(staging.Get(),resource);D3D11_MAPPED_SUBRESOURCE mapped{};check(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped));std::vector<uint8_t> data(size_t(bytes)*rows);
    for(UINT row=0;row<rows;++row)memcpy(data.data()+row*bytes,static_cast<const uint8_t*>(mapped.pData)+row*mapped.RowPitch,bytes);context->Unmap(staging.Get(),0);return data;
}
template<class T>T desc(const ObjectRecord& object){require(object.descriptor.size()>=sizeof(T),"Missing creation descriptor");T value;memcpy(&value,object.descriptor.data(),sizeof(value));return value;}
// Independent, deliberately fixture-scoped consumer: creates a fresh WARP
// device using CPU model data only. No source native pointer is consulted.
OracleOutput execute(const RecordedFrame& frame,ObjectId image,ObjectId output,bool omit_draws=false,bool omit_dispatch=false){
    frame.require_complete();ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;check(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context));
    std::map<ObjectId,ComPtr<IUnknown>> objects;
    auto put=[&](ObjectId id,auto& object){check(object.As(&objects[id]));};
    for(const auto& [id,initial]:frame.initial_resources){
        std::vector<D3D11_SUBRESOURCE_DATA> data;for(const auto& sub:initial.subresources)data.push_back({sub.bytes.data(),UINT(sub.row_bytes),UINT(sub.slice_bytes)});
        if(initial.shape.dimension==D3D11_RESOURCE_DIMENSION_BUFFER){auto d=desc<D3D11_BUFFER_DESC>(*initial.record);ComPtr<ID3D11Buffer> object;check(device->CreateBuffer(&d,data.data(),&object));put(id,object);}
        else{require(initial.shape.dimension==D3D11_RESOURCE_DIMENSION_TEXTURE2D,"Unexpected fixture resource");auto d=desc<D3D11_TEXTURE2D_DESC>(*initial.record);ComPtr<ID3D11Texture2D> object;check(device->CreateTexture2D(&d,data.data(),&object));put(id,object);}
    }
    for(const auto& [id,held]:frame.objects){const auto& r=*held.record;
        if(r.kind=="vertex_shader"){ComPtr<ID3D11VertexShader> v;check(device->CreateVertexShader(r.creation_bytes.data(),r.creation_bytes.size(),nullptr,&v));put(id,v);}
        else if(r.kind=="pixel_shader"){ComPtr<ID3D11PixelShader> v;check(device->CreatePixelShader(r.creation_bytes.data(),r.creation_bytes.size(),nullptr,&v));put(id,v);}
        else if(r.kind=="compute_shader"){ComPtr<ID3D11ComputeShader> v;check(device->CreateComputeShader(r.creation_bytes.data(),r.creation_bytes.size(),nullptr,&v));put(id,v);}
        else if(r.resource){ComPtr<ID3D11Resource> resource;check(objects.at(r.resource).As(&resource));
            if(r.kind=="rtv1"){auto d=desc<D3D11_RENDER_TARGET_VIEW_DESC>(r);ComPtr<ID3D11RenderTargetView> v;check(device->CreateRenderTargetView(resource.Get(),&d,&v));put(id,v);}
            else if(r.kind=="srv1"){auto d=desc<D3D11_SHADER_RESOURCE_VIEW_DESC>(r);ComPtr<ID3D11ShaderResourceView> v;check(device->CreateShaderResourceView(resource.Get(),&d,&v));put(id,v);}
            else if(r.kind=="uav1"){auto d=desc<D3D11_UNORDERED_ACCESS_VIEW_DESC>(r);ComPtr<ID3D11UnorderedAccessView> v;check(device->CreateUnorderedAccessView(resource.Get(),&d,&v));put(id,v);}
            else throw std::runtime_error("Unexpected fixture view kind");
        }
    }
    auto get=[&]<class T>(ObjectId id){ComPtr<T> object;if(id)check(objects.at(id).As(&object));return object;};
    for(const auto& command:frame.commands){const auto& s=command.bindings;const auto& w=command.words;
        context->ClearState();auto vs=get.template operator()<ID3D11VertexShader>(s.shaders[size_t(ShaderStage::vertex)].shader);auto ps=get.template operator()<ID3D11PixelShader>(s.shaders[size_t(ShaderStage::pixel)].shader);auto cs=get.template operator()<ID3D11ComputeShader>(s.shaders[size_t(ShaderStage::compute)].shader);
        context->VSSetShader(vs.Get(),nullptr,0);context->PSSetShader(ps.Get(),nullptr,0);context->CSSetShader(cs.Get(),nullptr,0);context->RSSetViewports(UINT(s.viewports.size()),s.viewports.data());context->IASetPrimitiveTopology(s.topology);
        auto indices=get.template operator()<ID3D11Buffer>(s.index_buffer);context->IASetIndexBuffer(indices.Get(),s.index_format,s.index_offset);
        auto rtv=get.template operator()<ID3D11RenderTargetView>(s.render_targets[0]);auto* rt=rtv.Get();context->OMSetRenderTargets(1,&rt,nullptr);
        auto srv=get.template operator()<ID3D11ShaderResourceView>(s.shaders[size_t(ShaderStage::compute)].resources[0]);auto uav=get.template operator()<ID3D11UnorderedAccessView>(s.cs_uavs[0]);auto* sv=srv.Get();auto* uv=uav.Get();context->CSSetShaderResources(0,1,&sv);context->CSSetUnorderedAccessViews(0,1,&uv,nullptr);
        switch(command.kind){
        case CommandKind::clear_rtv:{auto v=get.template operator()<ID3D11RenderTargetView>(command.operand);context->ClearRenderTargetView(v.Get(),command.floats.data());break;}
        case CommandKind::clear_uav_uint:{auto v=get.template operator()<ID3D11UnorderedAccessView>(command.operand);context->ClearUnorderedAccessViewUint(v.Get(),w.data());break;}
        case CommandKind::draw:if(!omit_draws)context->Draw(w[0],w[1]);break;
        case CommandKind::draw_indexed:if(!omit_draws)context->DrawIndexed(w[0],w[1],int32_t(w[2]));break;
        case CommandKind::draw_instanced:if(!omit_draws)context->DrawInstanced(w[0],w[1],w[2],w[3]);break;
        case CommandKind::draw_indexed_instanced:if(!omit_draws)context->DrawIndexedInstanced(w[0],w[1],w[2],int32_t(w[3]),w[4]);break;
        case CommandKind::dispatch:if(!omit_dispatch)context->Dispatch(w[0],w[1],w[2]);break;
        default:throw std::runtime_error("Unexpected fixture command");
        }
    }
    auto texture=get.template operator()<ID3D11Resource>(image);auto buffer=get.template operator()<ID3D11Resource>(output);
    return {read(device.Get(),context.Get(),texture.Get()),read(device.Get(),context.Get(),buffer.Get())};
}
void oracle(const OracleOutput& output,bool draw=true,bool dispatch=true){
    require(output.image.size()==256 && output.buffer.size()==16,"Wrong output shape");
    for(size_t i=0;i<256;i+=4){const uint8_t expected[]{0,uint8_t(draw?255:0),0,255};require(!memcmp(output.image.data()+i,expected,4),"Command image differs from CPU oracle");}
    const UINT values[]{dispatch?19u:2u,dispatch?26u:2u,dispatch?33u:2u,dispatch?40u:2u};require(!memcmp(output.buffer.data(),values,sizeof(values)),"Command compute output differs from CPU oracle");
}
void record_and_recreate(bool hardware){
    RecordedFrame frame;ObjectId image=0,output=0,source=0;
    {
        Fixture f(hardware);image=f.texture_id;output=f.destination_id;source=f.source_id;
        auto recording=begin_recording(f.context.Get());rejects([&]{begin_recording(f.context.Get());},"busy");
        float black[]{0,0,0,1};SetLastError(0x12349876);f.context->ClearRenderTargetView(f.rtv.Get(),black);require(GetLastError()==0x12349876,"Recorder changed native LastError");black[0]=1;
        UINT clear[]{2,3,4,5};f.context->ClearUnorderedAccessViewUint(f.uav.Get(),clear);clear[0]=999;
        f.context->Draw(3,0);f.context->DrawIndexed(3,0,-1);f.context->DrawInstanced(3,2,0,0);f.context->DrawIndexedInstanced(3,2,0,-1,0);
        const auto code=compile("float4 main():SV_Target{return float4(0,1,0,1);}","ps_5_0");ComPtr<ID3D11PixelShader> green;check(f.device->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&green));f.context->PSSetShader(green.Get(),nullptr,0);f.context->Draw(3,0);f.context->Dispatch(4,1,1);
        frame=recording->finish();frame.require_complete();rejects([&]{recording->finish();},"already finished");
        oracle({read(unwrap(f.device.Get()),unwrap(f.context.Get()),unwrap(f.texture.Get())),read(unwrap(f.device.Get()),unwrap(f.context.Get()),unwrap(f.destination.Get()))});
        require(frame.commands.size()==8,"Missing/duplicated application commands or internal readback entered stream");
        for(size_t i=0;i<frame.commands.size();++i)require(frame.commands[i].sequence==i,"Command ordering changed");
        require(frame.commands[0].floats[0]==0 && frame.commands[1].words[0]==2,"Command kept caller-owned clear pointers");
        require(int32_t(frame.commands[3].words[2])==-1 && int32_t(frame.commands[5].words[3])==-1,"Signed Draw argument lost");
        require(frame.commands[2].bindings.shaders[size_t(ShaderStage::pixel)].shader!=frame.commands[6].bindings.shaders[size_t(ShaderStage::pixel)].shader,"Shader binding changes were not captured");
        const auto& before=frame.initial_resources.at(image).subresources[0].bytes;
        for(size_t i=0;i<before.size();i+=4)require(before[i]==0 && before[i+1]==0 && before[i+2]==255 && before[i+3]==255,"Final image substituted for initial backbuffer");
        const UINT expected[]{17,17,17,17};require(!memcmp(frame.initial_resources.at(source).subresources[0].bytes.data(),expected,sizeof(expected)),"Creation bytes substituted for current initial buffer");
        f.context->ClearState();for(auto& [id,held]:frame.objects){(void)id;held.native.Reset();}
    }
    require(object_status().at("active")==0 && metadata_status().at("alive")==0,"Original GPU objects survived CPU model handoff");
    oracle(execute(frame,image,output));oracle(execute(frame,image,output,true),false,true);oracle(execute(frame,image,output,false,true),true,false);
}
void failures(bool hardware){
    Fixture f(hardware);auto run=[&](auto action,const char* reason,RecordingOptions options=RecordingOptions{}){
        auto recording=begin_recording(f.context.Get(),options);action(recording);auto frame=recording->finish();rejects([&]{frame.require_complete();},reason);
    };
    run([&](auto){f.context->CopySubresourceRegion(f.destination.Get(),0,0,0,0,f.source.Get(),0,nullptr);},"CopySubresourceRegion");
    const auto copied=read(unwrap(f.device.Get()),unwrap(f.context.Get()),unwrap(f.destination.Get()));const UINT expected[]{17,17,17,17};require(!memcmp(copied.data(),expected,sizeof(expected)),"Rejected recorder suppressed native CopyResource");
    run([&](auto recording){recording->cancel();f.context->Draw(3,0);},"cancelled");
    RecordingOptions limit;limit.byte_limit=1;run([&](auto){f.context->Draw(3,0);},"byte limit",limit);
    limit={};limit.command_limit=0;run([&](auto){f.context->Draw(3,0);},"command limit",limit);
    run([&](auto){std::thread worker([&]{D3D11_PRIMITIVE_TOPOLOGY value;f.context->IAGetPrimitiveTopology(&value);});worker.join();},"foreign-thread");
    run([&](auto){NativeCall nested;f.context->Draw(3,0);},"Reentrant");
    ComPtr<ID3D11DeviceContext> deferred;check(f.device->CreateDeferredContext(0,&deferred));run([&](auto){deferred->ClearState();},"Multiple context");
    D3D11_QUERY_DESC q{D3D11_QUERY_OCCLUSION,0};ComPtr<ID3D11Query> query;check(f.device->CreateQuery(&q,&query));f.context->Begin(query.Get());rejects([&]{begin_recording(f.context.Get());},"active query");f.context->End(query.Get());
    D3D11_TEXTURE2D_DESC td{};td.Width=td.Height=4;td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;td.Usage=D3D11_USAGE_STAGING;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ|D3D11_CPU_ACCESS_WRITE;
    ComPtr<ID3D11Texture2D> texture;check(f.device->CreateTexture2D(&td,nullptr,&texture));ComPtr<IDXGISurface> surface;check(texture.As(&surface));DXGI_MAPPED_RECT mapped{};
    check(surface->Map(&mapped,DXGI_MAP_WRITE));rejects([&]{begin_recording(f.context.Get());},"DXGI mapping");check(surface->Unmap());
    run([&](auto){check(surface->Map(&mapped,DXGI_MAP_WRITE));memset(mapped.pBits,0,4*4);check(surface->Unmap());},"IDXGISurface::Map");
    auto dropped=begin_recording(f.context.Get());dropped.reset();auto retry=begin_recording(f.context.Get());f.context->Draw(3,0);retry->finish().require_complete();
}
void query_boundaries(bool hardware){
    Fixture f(hardware);
    for(UINT type=0;type<16;++type){
        D3D11_QUERY_DESC desc{D3D11_QUERY(type),0};ComPtr<ID3D11Query> query;
        if(type==5 || type==7){ComPtr<ID3D11Predicate> predicate;check(f.device->CreatePredicate(&desc,&predicate));check(predicate.As(&query));}
        else check(f.device->CreateQuery(&desc,&query));
        const auto identity=object_record(query.Get())->id;
        auto recording=begin_recording(f.context.Get());
        // EVENT and TIMESTAMP are End-only. Other query types require Begin.
        if(type!=0 && type!=2)f.context->Begin(query.Get());
        f.context->End(query.Get());
        const auto frame=recording->finish();
        rejects([&]{frame.require_complete();},"Query recording is not implemented");
        require(frame.commands.empty() && frame.gaps.size()==1,"Rejected query escaped into the command model");
        const auto& gap=frame.gaps.front();
        require(gap.find(type==0 || type==2?"::End:":"::Begin:")!=std::string::npos &&
            gap.find("object="+std::to_string(identity)+",")!=std::string::npos &&
            gap.find("("+std::to_string(type)+"), misc_flags=0")!=std::string::npos,
            "Query failure omitted its API, identity or descriptor");
    }
    D3D11_QUERY_DESC desc{D3D11_QUERY_EVENT,0};ComPtr<ID3D11Query> event;check(f.device->CreateQuery(&desc,&event));
    f.context->End(event.Get());
    const auto deadline=GetTickCount64()+5000;
    while(unwrap(f.context.Get())->GetData(unwrap(event.Get()),nullptr,0,0)==S_FALSE){
        require(GetTickCount64()<deadline,"Native event query did not complete");Sleep(1);
    }
    // The End occurred before recording: GetData alone must invalidate the
    // frame rather than silently lose the application's completion dependency.
    for(bool status_only:{false,true}){
        auto recording=begin_recording(f.context.Get());BOOL ready=FALSE;
        const auto result=f.context->GetData(event.Get(),status_only?nullptr:&ready,status_only?0:sizeof(ready),D3D11_ASYNC_GETDATA_DONOTFLUSH);
        require(result==S_OK && (status_only || ready==TRUE),"Query rejection changed the native result/output");
        const auto frame=recording->finish();rejects([&]{frame.require_complete();},"ID3D11DeviceContext::GetData");
        require(frame.gaps.front().find(status_only?"data_size=0, getdata_flags=1":"data_size=4, getdata_flags=1")!=std::string::npos,
            "GetData failure omitted arguments");
    }
    auto recovery=begin_recording(f.context.Get());f.context->Draw(3,0);recovery->finish().require_complete();
}
void counters(bool hardware){
    Fixture f(hardware);D3D11_BUFFER_DESC d{};d.ByteWidth=64;d.StructureByteStride=4;d.BindFlags=D3D11_BIND_UNORDERED_ACCESS;d.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    UINT zeros[16]{};D3D11_SUBRESOURCE_DATA initial{zeros,0,0};ComPtr<ID3D11Buffer> buffer;check(f.device->CreateBuffer(&d,&initial,&buffer));
    D3D11_UNORDERED_ACCESS_VIEW_DESC v{};v.ViewDimension=D3D11_UAV_DIMENSION_BUFFER;v.Buffer.NumElements=16;v.Buffer.Flags=D3D11_BUFFER_UAV_FLAG_COUNTER;
    ComPtr<ID3D11UnorderedAccessView> view;check(f.device->CreateUnorderedAccessView(buffer.Get(),&v,&view));const auto id=object_record(view.Get())->id;
    auto code=compile("RWStructuredBuffer<uint> b:register(u0);[numthreads(1,1,1)]void main(){uint i=b.IncrementCounter();b[i]=19;}","cs_5_0");ComPtr<ID3D11ComputeShader> shader;check(f.device->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&shader));
    auto* uv=view.Get();UINT count=3;f.context->CSSetUnorderedAccessViews(0,1,&uv,&count);f.context->CSSetShader(shader.Get(),nullptr,0);
    auto recording=begin_recording(f.context.Get());f.context->Dispatch(2,1,1);
    count=UINT_MAX;f.context->CSSetUnorderedAccessViews(0,1,&uv,&count);f.context->Dispatch(1,1,1);
    count=1;f.context->CSSetUnorderedAccessViews(0,1,&uv,&count);f.context->Dispatch(1,1,1);
    const auto frame=recording->finish();frame.require_complete();require(frame.commands.size()==3,"Missing counter dispatch");
    require(frame.commands[0].uav_counters.at(id)==3 && frame.commands[1].uav_counters.at(id)==5 && frame.commands[2].uav_counters.at(id)==1,"Recorder guessed KEEP/initial values instead of sampling per-command counters");
    require(snapshot_uav_counter(f.context.Get(),view.Get()).value==2,"Recorder changed application counter execution");
}
void explicit_clears(bool hardware){
    Fixture f(hardware);D3D11_TEXTURE2D_DESC d{};d.Width=d.Height=4;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;d.Format=DXGI_FORMAT_D32_FLOAT;d.BindFlags=D3D11_BIND_DEPTH_STENCIL;
    ComPtr<ID3D11Texture2D> depth;check(f.device->CreateTexture2D(&d,nullptr,&depth));ComPtr<ID3D11DepthStencilView> dsv;check(f.device->CreateDepthStencilView(depth.Get(),nullptr,&dsv));f.context->ClearDepthStencilView(dsv.Get(),D3D11_CLEAR_DEPTH,1,0);
    d.Format=DXGI_FORMAT_R32_FLOAT;d.BindFlags=D3D11_BIND_UNORDERED_ACCESS;float zeros[16]{};D3D11_SUBRESOURCE_DATA initial{zeros,16,0};ComPtr<ID3D11Texture2D> target;check(f.device->CreateTexture2D(&d,&initial,&target));ComPtr<ID3D11UnorderedAccessView> uav;check(f.device->CreateUnorderedAccessView(target.Get(),nullptr,&uav));
    auto recording=begin_recording(f.context.Get());f.context->ClearDepthStencilView(dsv.Get(),D3D11_CLEAR_DEPTH,.25f,57);float values[]{.5f,.25f,0,1};f.context->ClearUnorderedAccessViewFloat(uav.Get(),values);values[0]=.75f;
    const auto frame=recording->finish();frame.require_complete();require(frame.commands.size()==2,"Explicit clear commands missing");
    require(frame.commands[0].kind==CommandKind::clear_dsv && frame.commands[0].words[0]==D3D11_CLEAR_DEPTH && frame.commands[0].words[1]==57 && frame.commands[0].floats[0]==.25f,"Depth/stencil arguments lost");
    require(frame.commands[1].kind==CommandKind::clear_uav_float && frame.commands[1].floats[0]==.5f,"Float UAV clear retained caller array");
    require(frame.initial_resources.contains(object_record(depth.Get())->id) && frame.initial_resources.contains(object_record(target.Get())->id),"Unbound explicit clear operands omitted from initial resources");
    const auto depth_bytes=read(unwrap(f.device.Get()),unwrap(f.context.Get()),unwrap(depth.Get()));const auto target_bytes=read(unwrap(f.device.Get()),unwrap(f.context.Get()),unwrap(target.Get()));
    for(size_t i=0;i<64;i+=4){float a,b;memcpy(&a,depth_bytes.data()+i,4);memcpy(&b,target_bytes.data()+i,4);require(a==.25f && b==.5f,"Recorder changed native depth/float clear result");}
}
}
#ifndef FLORA_ENCODE_PROBE
int main(int argc,char** argv){try{
    const bool hardware=argc==2 && std::string(argv[1])=="--hardware";record_and_recreate(hardware);explicit_clears(hardware);counters(hardware);failures(hardware);query_boundaries(hardware);
    require(object_status().at("active")==0 && metadata_status().at("alive")==0 && object_status().at("interface_gaps")==0,"Recorder lifetime or capability leak");
    std::cout<<Json({{"passed",true},{"hardware",hardware},{"commands",8},{"fresh_device_reconstruction",true},{"negative_controls",2},{"scope","hook-observed model; no GPA file or Present boundary"}}).dump()<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
#else
FrozenFrame observed_frame(const std::wstring& mode){
    const bool compute=mode==L"dispatch" || mode==L"dispatch-negative";
    require(compute || mode==L"clear" || mode==L"draw" || mode==L"negative","Unknown encoder probe mode");FrozenFrame frozen;
    {
        Fixture f(true,compute);auto recording=begin_recording(f.context.Get());const float black[]{0,0,0,1};f.context->ClearRenderTargetView(f.rtv.Get(),black);
        if(compute){
            auto code=compile("RWTexture2D<float4> image:register(u0);[numthreads(1,1,1)]void main(uint3 id:SV_DispatchThreadID){image[id.xy]=float4(0,1,0,1);}","cs_5_0");
            ComPtr<ID3D11ComputeShader> cs;check(f.device->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&cs));ComPtr<ID3D11UnorderedAccessView> uav;check(f.device->CreateUnorderedAccessView(f.texture.Get(),nullptr,&uav));
            f.context->OMSetRenderTargets(0,nullptr,nullptr);f.context->CSSetShader(cs.Get(),nullptr,0);auto* view=uav.Get();f.context->CSSetUnorderedAccessViews(0,1,&view,nullptr);f.context->Dispatch(8,8,1);
        }else if(mode!=L"clear")f.context->Draw(3,0);
        auto frame=recording->finish();auto final_output=snapshot_resource(f.context.Get(),f.texture.Get());frozen=freeze_frame(std::move(frame),f.texture_id,std::move(final_output));
    }
    require(object_status().at("active")==0 && metadata_status().at("alive")==0,"Encoder input retained original GPU objects");
    if(mode==L"negative" || mode==L"dispatch-negative")frozen.commands.pop_back();
    for(size_t i=0;i<frozen.commands.size();++i)frozen.commands[i].sequence=i;
    return frozen;
}
int wmain(int argc,wchar_t** argv){try{
    require(argc==3,"Expected output path and mode");const std::filesystem::path output=argv[1];const std::wstring mode=argv[2];
    if(mode==L"self-test"){
        require(!std::filesystem::exists(output),"Encoder test directory must be new");std::filesystem::create_directories(output);
        auto frame=observed_frame(L"draw");encode_frame(frame,output/L"valid.gpa_frame");rejects([&]{encode_frame(frame,output/L"valid.gpa_frame");},"exists");
        auto reject=[&](FrozenFrame bad,const char* reason,const wchar_t* file){const auto path=output/file;rejects([&]{encode_frame(bad,path);},reason);require(!std::filesystem::exists(path),"Encoder published a rejected frame");};
        auto bad=frame;bad.commands.back().bindings.index_buffer=UINT64_MAX;reject(std::move(bad),"reference",L"reference.gpa_frame");
        bad=frame;bad.objects.begin()->second.id=UINT64_MAX;reject(std::move(bad),"identity",L"identity.gpa_frame");
        bad=frame;bad.initial_resources.at(frame.output).subresources[0].bytes.pop_back();reject(std::move(bad),"length",L"truncated.gpa_frame");
        bad=frame;bad.commands.back().sequence=99;reject(std::move(bad),"sequence",L"sequence.gpa_frame");
        bad=frame;bad.commands.back().bindings.resource_min_lod[frame.output]=1;reject(std::move(bad),"LOD",L"lod.gpa_frame");
        bad=frame;bad.commands.back().bindings.vertices[0]=frame.output;reject(std::move(bad),"wrong kind",L"vertex-kind.gpa_frame");
        bad=frame;bad.commands.back().bindings.shaders[size_t(ShaderStage::pixel)].shader=bad.commands.back().bindings.shaders[size_t(ShaderStage::vertex)].shader;reject(std::move(bad),"wrong kind",L"shader-stage.gpa_frame");
        bad=frame;bad.commands.front().operand=frame.output;reject(std::move(bad),"wrong kind",L"clear-kind.gpa_frame");
        bad=frame;bad.initial_resources[frame.commands.front().operand]=frame.final_output;reject(std::move(bad),"wrong kind",L"initial-kind.gpa_frame");
        for(const auto [effect,count]:{std::pair{DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL,1u},std::pair{DXGI_SWAP_EFFECT_FLIP_DISCARD,17u},std::pair{DXGI_SWAP_EFFECT(2),2u}}){
            bad=frame;Presentation presentation;auto& d=presentation.descriptor;d.BufferDesc.Width=d.BufferDesc.Height=8;d.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;d.SampleDesc.Count=1;d.BufferCount=count;d.SwapEffect=effect;bad.presentation=presentation;
            reject(std::move(bad),"buffer count",(L"bad-presentation-"+std::to_wstring(effect)+L".gpa_frame").c_str());
        }
        for(unsigned invalid=0;invalid<5;++invalid){
            bad=frame;Presentation presentation;auto& d=presentation.descriptor;d.BufferDesc.Width=d.BufferDesc.Height=8;d.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;d.SampleDesc.Count=1;d.BufferCount=2;d.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;d.Windowed=TRUE;d.Flags=DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;presentation.flags=DXGI_PRESENT_ALLOW_TEARING;
            if(invalid==0)d.Flags=0;else if(invalid==1)presentation.sync_interval=1;else if(invalid==2)d.Windowed=FALSE;else if(invalid==3)d.SwapEffect=DXGI_SWAP_EFFECT_SEQUENTIAL;else presentation.flags|=DXGI_PRESENT_DO_NOT_SEQUENCE;
            bad.presentation=presentation;reject(std::move(bad),invalid==4?"Present flags":"tearing",(L"bad-tearing-"+std::to_wstring(invalid)+L".gpa_frame").c_str());
        }
        EncodeOptions cancelled;cancelled.cancelled=[] {return true;};rejects([&]{encode_frame(frame,output/L"cancel.gpa_frame",cancelled);},"cancelled");require(!std::filesystem::exists(output/L"cancel.gpa_frame"),"Cancelled output published");
        bool saw_partial=false;EncodeOptions interrupted;interrupted.cancelled=[&]{for(const auto& entry:std::filesystem::directory_iterator(output))if(entry.path().extension()==L".partial"){saw_partial=true;return true;}return false;};
        rejects([&]{encode_frame(frame,output/L"interrupted.gpa_frame",interrupted);},"cancelled");require(saw_partial && !std::filesystem::exists(output/L"interrupted.gpa_frame"),"Interrupted writer published output");
        for(const auto& entry:std::filesystem::directory_iterator(output))require(entry.path().extension()!=L".partial","Encoder leaked a partial transaction");
        std::cout<<"Encoder validation/failure gates passed; no end-to-end capture claimed\n";
    }else{
        const auto frame=observed_frame(mode);encode_frame(frame,output);std::cout<<Json({{"observed_commands",frame.commands.size()},{"initial_resources",frame.initial_resources.size()},{"objects",frame.objects.size()},{"source_gpu_objects_alive",0},{"scope","Observed model encoded; no Present/CLI integration"}}).dump()<<'\n';
    }
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
#endif
