#include "command_stream.hpp"
#include "objects.hpp"
#include "encoder.hpp"
#include <d3dcompiler.h>
#include <d3d11sdklayers.h>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
using namespace flora::capture;
using Microsoft::WRL::ComPtr;
namespace {
void require(bool v,const char* message){if(!v)throw std::runtime_error(message);}
void check(HRESULT hr){require(SUCCEEDED(hr),"Native boundary mapping fixture failed");}
ComPtr<ID3DBlob> shader(const char* source,const char* profile){ComPtr<ID3DBlob> code,error;check(D3DCompile(source,strlen(source),"boundary-map",nullptr,nullptr,"main",profile,0,0,&code,&error));return code;}
template<class T>std::vector<uint8_t> bytes(const T& data){auto* p=reinterpret_cast<const uint8_t*>(&data);return {p,p+sizeof(data)};}
struct Fixture {
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;ComPtr<ID3D11InfoQueue> debug;
    ComPtr<ID3D11Texture2D> output;ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11Buffer> color,vertices,indices;ComPtr<ID3D11ShaderResourceView> srv;
    D3D11_MAPPED_SUBRESOURCE c{},v{},i{};
    const float blue[4]{0,0,1,1},red[4]{1,0,0,1},green[4]{0,1,0,1};
    const float initial_vertices[12]{-1,-1,.25f,1,-1,3,.25f,1,3,-1,.25f,1};
    const float final_vertices[12]{-1,-1,.5f,1,-1,3,.5f,1,3,-1,.5f,1};
    const uint32_t initial_indices[3]{0,1,2},final_indices[3]{2,1,0};
    Fixture(bool hardware,bool use_srv){
        check(D3D11CreateDevice(nullptr,hardware?D3D_DRIVER_TYPE_HARDWARE:D3D_DRIVER_TYPE_WARP,nullptr,D3D11_CREATE_DEVICE_DEBUG,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context));
        check(device.As(&debug));wrap_object(device.GetAddressOf(),"test::Device");wrap_object(context.GetAddressOf(),"test::Context");
        auto buffer=[&](UINT size,UINT bind){D3D11_BUFFER_DESC d{size,D3D11_USAGE_DYNAMIC,bind,D3D11_CPU_ACCESS_WRITE,0,0};ComPtr<ID3D11Buffer> b;check(device->CreateBuffer(&d,nullptr,&b));return b;};
        color=buffer(16,use_srv?D3D11_BIND_SHADER_RESOURCE:D3D11_BIND_CONSTANT_BUFFER);vertices=buffer(48,D3D11_BIND_VERTEX_BUFFER);indices=buffer(12,D3D11_BIND_INDEX_BUFFER);
        if(use_srv){D3D11_SHADER_RESOURCE_VIEW_DESC d{};d.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;d.ViewDimension=D3D11_SRV_DIMENSION_BUFFER;d.Buffer.NumElements=1;check(device->CreateShaderResourceView(color.Get(),&d,&srv));}
        auto vs=shader("float4 main(float4 p:POSITION):SV_Position{return p;}","vs_5_0");ComPtr<ID3D11VertexShader> vertex;check(device->CreateVertexShader(vs->GetBufferPointer(),vs->GetBufferSize(),nullptr,&vertex));context->VSSetShader(vertex.Get(),nullptr,0);
        D3D11_INPUT_ELEMENT_DESC element{"POSITION",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0};ComPtr<ID3D11InputLayout> layout;check(device->CreateInputLayout(&element,1,vs->GetBufferPointer(),vs->GetBufferSize(),&layout));context->IASetInputLayout(layout.Get());
        auto ps=shader(use_srv?"Buffer<float4> c:register(t0);float4 main():SV_Target{return c.Load(0);}":"cbuffer C:register(b0){float4 c;}float4 main():SV_Target{return c;}","ps_5_0");ComPtr<ID3D11PixelShader> pixel;check(device->CreatePixelShader(ps->GetBufferPointer(),ps->GetBufferSize(),nullptr,&pixel));context->PSSetShader(pixel.Get(),nullptr,0);
        D3D11_RASTERIZER_DESC rd{};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=TRUE;ComPtr<ID3D11RasterizerState> rs;check(device->CreateRasterizerState(&rd,&rs));context->RSSetState(rs.Get());
        D3D11_TEXTURE2D_DESC td{8,8,1,1,DXGI_FORMAT_R8G8B8A8_UNORM,{1,0},D3D11_USAGE_DEFAULT,D3D11_BIND_RENDER_TARGET,0,0};check(device->CreateTexture2D(&td,nullptr,&output));check(device->CreateRenderTargetView(output.Get(),nullptr,&rtv));auto* rt=rtv.Get();context->OMSetRenderTargets(1,&rt,nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    }
    void map(){check(context->Map(color.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&c));check(context->Map(vertices.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&v));check(context->Map(indices.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&i));memcpy(c.pData,blue,16);memcpy(v.pData,initial_vertices,48);memcpy(i.pData,initial_indices,12);}
    void writes(){memcpy(c.pData,red,16);memcpy(v.pData,final_vertices,48);memcpy(i.pData,final_indices,12);}
    void unmap(){context->Unmap(vertices.Get(),0);context->Unmap(color.Get(),0);context->Unmap(indices.Get(),0);}
    void bind(){auto* b=vertices.Get();UINT stride=16,offset=0;context->IASetVertexBuffers(0,1,&b,&stride,&offset);context->IASetIndexBuffer(indices.Get(),DXGI_FORMAT_R32_UINT,0);if(srv){auto* s=srv.Get();context->PSSetShaderResources(0,1,&s);}else{b=color.Get();context->PSSetConstantBuffers(0,1,&b);}}
    void draw(float x,float width){D3D11_VIEWPORT vp{x,0,width,8,0,1};context->RSSetViewports(1,&vp);context->DrawIndexed(3,0,0);}
    void clean_debug(){for(UINT64 n=0;n<debug->GetNumStoredMessagesAllowedByRetrievalFilter();++n){SIZE_T size=0;debug->GetMessage(n,nullptr,&size);std::vector<uint8_t> data(size);auto* m=reinterpret_cast<D3D11_MESSAGE*>(data.data());check(debug->GetMessage(n,m,&size));require(m->Severity>D3D11_MESSAGE_SEVERITY_WARNING,m->pDescription);}}
    ~Fixture(){context->ClearState();context->Flush();}
};
FrozenFrame capture(bool hardware,bool srv,bool tail){FrozenFrame frozen;{
    Fixture f(hardware,srv);f.map();const auto color=object_record(f.color.Get())->id,vertices=object_record(f.vertices.Get())->id,indices=object_record(f.indices.Get())->id;
    auto recording=begin_recording(f.context.Get());f.writes();const float black[4]{0,0,0,1};f.context->ClearRenderTargetView(f.rtv.Get(),black);
    f.context->Unmap(f.vertices.Get(),0);f.context->ClearRenderTargetView(f.rtv.Get(),black);f.context->Unmap(f.color.Get(),0);f.context->Unmap(f.indices.Get(),0);f.bind();f.draw(0,4);
    D3D11_MAPPED_SUBRESOURCE next{};check(f.context->Map(f.color.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&next));memcpy(next.pData,f.green,16);f.context->Unmap(f.color.Get(),0);f.draw(4,4);
    if(tail){f.map();f.writes();}
    auto frame=recording->finish();frame.require_complete();require(frame.initial_maps.size()==3 && frame.commands.size()==(tail?12:9) && frame.final_maps.size()==(tail?3:0),"Boundary Map event placement lost");
    if(tail){
        require(metadata_status().at("active_mapping_count")==3,"Capture ended application mappings");
        for(const auto& m:frame.final_maps)require(frame.commands.at(m.command_sequence).data==(m.resource==color?bytes(f.red):m.resource==vertices?bytes(f.final_vertices):bytes(f.final_indices)),"Frame-end Map bytes mismatch");
        f.unmap();
    }
    require(frame.initial_resources.at(color).subresources[0].bytes==bytes(f.blue) && frame.initial_resources.at(vertices).subresources[0].bytes==bytes(f.initial_vertices) && frame.initial_resources.at(indices).subresources[0].bytes==bytes(f.initial_indices),"Boundary bytes replaced by final writes");
    for(const auto& m:frame.initial_maps)require(m.generation==1 && m.thread==GetCurrentThreadId() && m.data==(m.resource==color?bytes(f.red):m.resource==vertices?bytes(f.final_vertices):bytes(f.final_indices)),"Boundary write ownership/provenance mismatch");
    require(metadata_status().at("active_mapping_count")==0,"Native Unmap did not remove borrowed storage");
    auto final=snapshot_resource(f.context.Get(),f.output.Get());const auto output=final.record->id;frozen=freeze_frame(std::move(frame),output,std::move(final));
    for(UINT y=0;y<8;++y)for(UINT x=0;x<8;++x){const uint8_t expected[]{uint8_t(x<4?255:0),uint8_t(x<4?0:255),0,255};require(!memcmp(frozen.final_output.subresources[0].bytes.data()+(y*8+x)*4,expected,4),"Native output differs from CPU oracle");}
    require(snapshot_resource(f.context.Get(),f.vertices.Get()).subresources[0].bytes==bytes(f.final_vertices) && snapshot_resource(f.context.Get(),f.indices.Get()).subresources[0].bytes==bytes(f.final_indices),"Capture changed native buffer writes");f.clean_debug();
}require(object_status().at("active")==0 && metadata_status().at("alive")==0,"Boundary frame retained native owners");return frozen;}
void refusals(bool hardware){
    Fixture f(hardware,false);f.map();RecordingOptions tiny;tiny.byte_limit=1;bool rejected=false;try{begin_recording(f.context.Get(),tiny);}catch(...){rejected=true;}require(rejected,"Boundary snapshot ignored byte limit");require(!memcmp(f.c.pData,f.blue,16),"Rejected boundary changed application bytes");
    auto r=begin_recording(f.context.Get());r->cancel();f.writes();f.unmap();auto frame=r->finish();require(frame.cancelled,"Boundary cancellation lost");r.reset();
    // Reuse the same resources after cancellation; copied pending bytes cannot
    // authorize a later generation or survive a frame-local lifetime.
    f.map();r=begin_recording(f.context.Get());{auto record=object_record(f.color.Get());std::lock_guard lock(record->mutex);++record->mapped_subresources.begin()->second.generation;}
    f.writes();f.unmap();frame=r->finish();require(frame.failed && frame.gaps.front().find("generation")!=std::string::npos,"Boundary generation mismatch accepted");r.reset();
    f.map();RecordingOptions limited;limited.byte_limit=340;r=begin_recording(f.context.Get(),limited);const float black[4]{0,0,0,1};f.context->ClearRenderTargetView(f.rtv.Get(),black);f.writes();f.unmap();frame=r->finish();require(frame.failed && frame.gaps.front().find("byte limit")!=std::string::npos,"Boundary Unmap payload ignored byte budget");r.reset();
    f.map();r=begin_recording(f.context.Get());record_context_call(f.context.Get(),"test::mapped dependency",CommandKind::map,{0,D3D11_MAP_READ},f.color.Get());f.writes();f.unmap();frame=r->finish();require(frame.failed && frame.gaps.front().find("application-mapped")!=std::string::npos,"Boundary snapshot authorized mapped GPU dependency");r.reset();
    f.map();r=begin_recording(f.context.Get());f.context->ClearRenderTargetView(f.rtv.Get(),black);frame=r->finish();frame.require_complete();require(frame.initial_maps.empty() && frame.initial_resources.size()==1 && !memcmp(f.c.pData,f.blue,16),"Untouched boundary mapping leaked into frame or changed bytes");r.reset();f.unmap();
    std::thread foreign([&]{check(f.context->Map(f.color.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&f.c));memcpy(f.c.pData,f.blue,16);});foreign.join();r=begin_recording(f.context.Get());f.context->Unmap(f.color.Get(),0);frame=r->finish();require(frame.failed && frame.gaps.front().find("map_thread=")!=std::string::npos,"Foreign boundary mapping accepted");r.reset();
    RecordingOptions end_limit;end_limit.byte_limit=30;r=begin_recording(f.context.Get(),end_limit);check(f.context->Map(f.color.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&f.c));memcpy(f.c.pData,f.red,16);frame=r->finish();require(frame.failed && frame.gaps.front().find("byte limit")!=std::string::npos && !memcmp(f.c.pData,f.red,16),"Frame-end budget failure changed mapped bytes or was accepted");r.reset();f.context->Unmap(f.color.Get(),0);
    // Detaching the final public wrapper must invalidate its borrowed address,
    // even while an independently retained native interface keeps it alive.
    auto record=object_record(f.color.Get());r=begin_recording(f.context.Get());check(f.context->Map(f.color.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&f.c));ComPtr<ID3D11Buffer> native=unwrap(f.color.Get());f.color.Reset();
    {std::lock_guard lock(record->mutex);require(!record->mapped_subresources.begin()->second.storage.pData,"Detached wrapper retained mapped pointer");}
    frame=r->finish();require(frame.failed && frame.gaps.front().find("active resource mapping")!=std::string::npos,"Frame-end borrowed address survived wrapper detach");r.reset();unwrap(f.context.Get())->Unmap(native.Get(),0);native.Reset();frame={};f.clean_debug();
}
}
int wmain(int argc,wchar_t** argv){try{
    const bool hardware=argc>1 && std::wstring(argv[1])==L"--hardware";const auto out=argc>2?std::filesystem::path(argv[2]):std::filesystem::temp_directory_path()/(L"flora-boundary-"+std::to_wstring(GetCurrentProcessId()));require(!std::filesystem::exists(out),"Output directory exists");std::filesystem::create_directories(out);
    refusals(hardware);Json files=Json::array();unsigned rejected=0;
    for(bool srv:{false,true})for(bool tail:{false,true}){const auto original=capture(hardware,srv,tail);
        for(unsigned bad=0;bad<(tail?13u:8u);++bad){auto f=original;if(bad==0)f.initial_maps[0].data.pop_back();if(bad==1)f.initial_maps[0].generation=0;if(bad==2)++f.initial_maps[0].thread;if(bad==3)++f.initial_maps[0].context;if(bad==4)f.initial_maps.push_back(f.initial_maps[0]);if(bad==5)f.initial_resources.erase(f.initial_maps[0].resource);if(bad==6)f.commands.erase(f.commands.begin()+1);if(bad==7){auto& d=f.objects.at(f.initial_maps[0].resource).descriptor;D3D11_BUFFER_DESC desc{};memcpy(&desc,d.data(),sizeof desc);desc.Usage=D3D11_USAGE_DEFAULT;memcpy(d.data(),&desc,sizeof desc);}if(bad==8)f.final_maps.clear();if(bad==9)f.final_maps[0].command_sequence=0;if(bad==10)f.final_maps.push_back(f.final_maps[0]);if(bad==11)++f.final_maps[0].thread;if(bad==12)f.final_maps[0].command_sequence=6;for(size_t n=0;n<f.commands.size();++n)f.commands[n].sequence=n;
            const auto path=out/(L"bad-"+std::to_wstring(srv)+L"-"+std::to_wstring(tail)+L"-"+std::to_wstring(bad)+L".gpa_frame");bool refused=false;try{encode_frame(f,path);}catch(...){refused=true;}require(refused && !std::filesystem::exists(path),"Malformed boundary frame published");++rejected;}
        for(bool negative:{false,true}){auto f=original;if(negative){std::set<ObjectId> pending;for(auto& m:f.initial_maps)pending.insert(m.resource);f.initial_maps.clear();std::erase_if(f.commands,[&](const auto& c){return c.kind==CommandKind::unmap && pending.erase(c.operand);});for(size_t n=0;n<f.commands.size();++n)f.commands[n].sequence=n;}
            for(auto& m:f.final_maps){const auto last=std::find_if(f.commands.rbegin(),f.commands.rend(),[&](const auto& c){return c.kind==CommandKind::map && c.operand==m.resource;});require(last!=f.commands.rend(),"Final Map fixture missing call");m.command_sequence=last->sequence;}
            const auto name=std::string(srv?"srv":"constant")+(tail?"-open-end":"")+(negative?"-missing-initial-maps":"-complete");encode_frame(f,out/(name+".gpa_frame"));files.push_back({{"file",name+".gpa_frame"},{"negative",negative},{"tail",tail}});}
    }
    std::ofstream(out/"manifest.json")<<files.dump(2);std::cout<<Json{{"passed",true},{"hardware",hardware},{"files",files.size()},{"encoder_refusals",rejected},{"source_gpu_objects_alive",object_status().at("active")},{"debug_warnings_or_errors",0}}.dump()<<'\n';if(argc<=2)std::filesystem::remove_all(out);return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
