#include "command_stream.hpp"
#include "deferred_history.hpp"
#include "deferred_resources.hpp"
#include "deferred_lowering.hpp"
#include "objects.hpp"
#include "encoder.hpp"
#include <d3dcompiler.h>
#include <d3d11sdklayers.h>
#include <filesystem>
#include <iostream>
#include <thread>
using namespace flora::capture;
using Microsoft::WRL::ComPtr;
namespace {
bool debug=false;
void require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
void check(HRESULT hr){if(FAILED(hr))throw std::runtime_error("Deferred execution native HRESULT failed");}
ObjectId id(IUnknown* object){return object_record(object)->id;}
ComPtr<ID3DBlob> compile(const char* text,const char* profile){ComPtr<ID3DBlob> code,error;auto hr=D3DCompile(text,strlen(text),"deferred-execution",nullptr,nullptr,"main",profile,0,0,&code,&error);if(FAILED(hr)&&error)std::cerr<<static_cast<const char*>(error->GetBufferPointer());check(hr);return code;}
void audit(ID3D11Device* device){if(!debug)return;ComPtr<ID3D11InfoQueue> q;check(unwrap(device)->QueryInterface(IID_PPV_ARGS(&q)));for(UINT64 i=0;i<q->GetNumStoredMessagesAllowedByRetrievalFilter();++i){SIZE_T n=0;check(q->GetMessage(i,nullptr,&n));std::vector<uint8_t> bytes(n);auto* m=reinterpret_cast<D3D11_MESSAGE*>(bytes.data());check(q->GetMessage(i,m,&n));if(m->Severity<=D3D11_MESSAGE_SEVERITY_WARNING){std::cerr<<m->pDescription<<'\n';throw std::runtime_error("Unexpected deferred execution Debug Layer diagnostic");}}}
void pixels(const ResourceSnapshot& image,const uint8_t* color){require(image.subresources[0].bytes.size()==256,"Unexpected output extent");for(size_t at=0;at<256;at+=4)require(!memcmp(image.subresources[0].bytes.data()+at,color,4),"Native deferred execution differs from independent pixels");}
struct Fixture {
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> immediate,c;
    ComPtr<ID3D11Texture2D> texture;ComPtr<ID3D11RenderTargetView> target;
    Fixture(bool hardware){check(D3D11CreateDevice(nullptr,hardware?D3D_DRIVER_TYPE_HARDWARE:D3D_DRIVER_TYPE_WARP,nullptr,debug?D3D11_CREATE_DEVICE_DEBUG:0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&immediate));wrap_object(device.GetAddressOf(),"Execution::Device");wrap_object(immediate.GetAddressOf(),"Execution::Immediate");check(device->CreateDeferredContext(0,&c));D3D11_TEXTURE2D_DESC d{8,8,1,1,DXGI_FORMAT_R8G8B8A8_UNORM,{1,0},D3D11_USAGE_DEFAULT,D3D11_BIND_RENDER_TARGET,0,0};check(device->CreateTexture2D(&d,nullptr,&texture));check(device->CreateRenderTargetView(texture.Get(),nullptr,&target));const float black[]{0,0,0,1};immediate->ClearRenderTargetView(target.Get(),black);}
    ~Fixture(){immediate->ClearState();}
    ComPtr<ID3D11CommandList> finish(BOOL restore=FALSE){ComPtr<ID3D11CommandList> list;check(c->FinishCommandList(restore,&list));return list;}
};
std::vector<FrozenFrame> execution(bool hardware){std::vector<FrozenFrame> frames;std::shared_ptr<const DeferredCommandList> cpuHistory;
    {Fixture f(hardware);
    D3D11_BUFFER_DESC bd{4,D3D11_USAGE_DEFAULT,D3D11_BIND_UNORDERED_ACCESS|D3D11_BIND_SHADER_RESOURCE,0,0,0};UINT nine=9;D3D11_SUBRESOURCE_DATA initial{&nine,0,0};ComPtr<ID3D11Buffer> buffer;check(f.device->CreateBuffer(&bd,&initial,&buffer));const auto bufferId=id(buffer.Get());
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};ud.Format=DXGI_FORMAT_R32_UINT;ud.ViewDimension=D3D11_UAV_DIMENSION_BUFFER;ud.Buffer.NumElements=1;ComPtr<ID3D11UnorderedAccessView> uav;check(f.device->CreateUnorderedAccessView(buffer.Get(),&ud,&uav));D3D11_SHADER_RESOURCE_VIEW_DESC sd{};sd.Format=DXGI_FORMAT_R32_UINT;sd.ViewDimension=D3D11_SRV_DIMENSION_BUFFER;sd.Buffer.NumElements=1;ComPtr<ID3D11ShaderResourceView> srv;check(f.device->CreateShaderResourceView(buffer.Get(),&sd,&srv));
    auto csCode=compile("RWBuffer<uint> value:register(u0);[numthreads(1,1,1)]void main(){value[0]=value[0]+1;}","cs_5_0");ComPtr<ID3D11ComputeShader> cs;check(f.device->CreateComputeShader(csCode->GetBufferPointer(),csCode->GetBufferSize(),nullptr,&cs));auto* u=uav.Get();f.c->CSSetShader(cs.Get(),nullptr,0);f.c->CSSetUnorderedAccessViews(0,1,&u,nullptr);f.c->Dispatch(1,1,1);auto compute=f.finish();
    auto vsCode=compile("float4 main(uint v:SV_VertexID):SV_Position{return float4(v==2?3:-1,v==1?3:-1,.5,1);}","vs_5_0");auto psCode=compile("Buffer<uint> value:register(t0);float4 main():SV_Target{return value[0]==3?float4(1,0,0,1):value[0]==4?float4(0,1,0,1):float4(0,0,1,1);}","ps_5_0");ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11PixelShader> ps;check(f.device->CreateVertexShader(vsCode->GetBufferPointer(),vsCode->GetBufferSize(),nullptr,&vs));check(f.device->CreatePixelShader(psCode->GetBufferPointer(),psCode->GetBufferSize(),nullptr,&ps));auto* s=srv.Get();auto* target=f.target.Get();D3D11_VIEWPORT viewport{0,0,8,8,0,1};f.c->VSSetShader(vs.Get(),nullptr,0);f.c->PSSetShader(ps.Get(),nullptr,0);f.c->PSSetShaderResources(0,1,&s);f.c->OMSetRenderTargets(1,&target,nullptr);f.c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);f.c->RSSetViewports(1,&viewport);f.c->Draw(3,0);auto draw=f.finish();
    // GPU contents change after compilation; creation bytes and compile-time
    // bytes must never become the captured frame's initial buffer value.
    const UINT one=1;f.immediate->UpdateSubresource(buffer.Get(),0,nullptr,&one,0,0);buffer.Reset();srv.Reset();uav.Reset();cs.Reset();vs.Reset();ps.Reset();f.c.Reset();
    cpuHistory=object_record(compute.Get())->command_list;auto leases=command_list_resources(compute.Get());require(leases&&leases->contains(bufferId),"Resource was not leased across wrapper destruction");leases.reset();
    for(UINT frameNumber=0;frameNumber<2;++frameNumber){
        auto recording=begin_recording(f.immediate.Get());
        if(!frameNumber){f.immediate->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);f.immediate->ExecuteCommandList(draw.Get(),TRUE);D3D11_PRIMITIVE_TOPOLOGY topology{};f.immediate->IAGetPrimitiveTopology(&topology);require(topology==D3D11_PRIMITIVE_TOPOLOGY_LINELIST,"Capture changed Execute(TRUE) restoration");}
        f.immediate->ExecuteCommandList(compute.Get(),FALSE);D3D11_PRIMITIVE_TOPOLOGY topology{};f.immediate->IAGetPrimitiveTopology(&topology);require(topology==D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED,"Capture changed Execute(FALSE) reset");
        if(!frameNumber)f.immediate->ExecuteCommandList(compute.Get(),TRUE);
        f.immediate->ExecuteCommandList(draw.Get(),TRUE);auto captured=recording->finish();captured.require_complete();
        require(captured.commands.size()==(frameNumber?2:4),"Execution expansion deduplicated/reordered commands");
        for(size_t i=0;i<captured.commands.size();++i){const auto& command=captured.commands[i];require(command.deferred&&command.deferred->execution_ordinal==i+1&&command.bindings.context==id(f.immediate.Get()),"Expanded command provenance/context lost");}
        if(!frameNumber)require(captured.commands[0].deferred->list==id(draw.Get())&&captured.commands[1].deferred->list==id(compute.Get())&&captured.commands[2].deferred->list==id(compute.Get()),"Execution order follows compilation or deduplicates repeats");
        UINT initialValue=0;memcpy(&initialValue,captured.initial_resources.at(bufferId).subresources[0].bytes.data(),4);require(initialValue==(frameNumber?3u:1u),"Frame initial data came from creation/Finish or a prior execution");
        auto nativeLeases=command_list_resources(compute.Get());ComPtr<ID3D11Resource> nativeResource;check(nativeLeases->at(bufferId).As(&nativeResource));auto finalBuffer=snapshot_resource(f.immediate.Get(),nativeResource.Get());UINT finalValue=0;memcpy(&finalValue,finalBuffer.subresources[0].bytes.data(),4);require(finalValue==(frameNumber?4u:3u),"Repeated native execution result changed");
        const uint8_t red[]{255,0,0,255},green[]{0,255,0,255};auto finalImage=snapshot_resource(f.immediate.Get(),f.texture.Get());pixels(finalImage,frameNumber?green:red);frames.push_back(freeze_frame(std::move(captured),id(f.texture.Get()),std::move(finalImage)));
    }
    audit(f.device.Get());}
    require(object_status().at("active")==0&&metadata_status().at("alive")==0,"Native lease cycle or CPU model COM ownership");require(cpuHistory&&cpuHistory->calls.back().state,"CPU history did not survive native lease destruction");return frames;
}
void faults(bool hardware){Fixture f(hardware);const float red[]{1,0,0,1};f.c->ClearRenderTargetView(f.target.Get(),red);auto list=f.finish();auto source=object_record(list.Get())->command_list;
    auto rejects=[&](const char* reason,RecordingOptions options={}){auto recording=begin_recording(f.immediate.Get(),options);f.immediate->ExecuteCommandList(list.Get(),FALSE);auto frame=recording->finish();bool failed=false;try{frame.require_complete();}catch(const std::exception& e){failed=std::string(e.what()).find(reason)!=std::string::npos;}require(failed,"Deferred failure did not produce the expected located gap");const uint8_t color[]{255,0,0,255};pixels(snapshot_resource(f.immediate.Get(),f.texture.Get()),color);};
    RecordingOptions budget;budget.byte_limit=1;rejects("byte limit",budget);budget={};budget.command_limit=0;rejects("command limit",budget);
    auto broken=std::make_shared<DeferredCommandList>(*source);broken->definitions.erase(id(f.texture.Get()));object_record(list.Get())->command_list=broken;rejects("definition missing");object_record(list.Get())->command_list=source;
    broken=std::make_shared<DeferredCommandList>(*source);broken->calls.back().kind=CommandKind::map;object_record(list.Get())->command_list=broken;rejects("Map rename");object_record(list.Get())->command_list=source;
    f.c->ExecuteCommandList(list.Get(),FALSE);auto nested=f.finish();auto record=begin_recording(f.immediate.Get());f.immediate->ExecuteCommandList(nested.Get(),FALSE);auto merged=record->finish();merged.require_complete();require(merged.commands.size()==1&&merged.commands[0].merged_origins.size()==1,"Merged Clear provenance missing");
    record=begin_recording(f.immediate.Get());record->cancel();f.immediate->ExecuteCommandList(list.Get(),FALSE);require(record->finish().cancelled,"Deferred cancellation result lost");
    record=begin_recording(f.immediate.Get());std::thread worker([&]{f.immediate->ExecuteCommandList(list.Get(),FALSE);});worker.join();require(record->finish().failed,"Foreign immediate execution silently accepted");
    // The shared lease can safely outlive its source list during readback, and
    // releases independently of long-lived CPU compilation evidence.
    auto held=command_list_resources(list.Get());require(held&&held->contains(id(f.texture.Get())),"Scoped execution lease missing");list.Reset();require(held->at(id(f.texture.Get())).Get()!=nullptr,"Scoped lease expired with list");held.reset();audit(f.device.Get());
}
}
int wmain(int argc,wchar_t** argv){try{bool hardware=false;std::filesystem::path out;for(int i=1;i<argc;++i){const std::wstring arg=argv[i];if(arg==L"--hardware")hardware=true;else if(arg==L"--debug")debug=true;else out=arg;}auto frames=execution(hardware);faults(hardware);require(object_status().at("active")==0&&metadata_status().at("alive")==0,"Deferred execution fixture leaked native objects");
    if(!out.empty()){std::filesystem::create_directories(out);encode_frame(frames[0],out/L"repeated.gpa_frame");encode_frame(frames[1],out/L"subsequent.gpa_frame");auto negative=frames[0];auto first=std::find_if(negative.commands.begin(),negative.commands.end(),[](const auto& c){return c.kind==CommandKind::dispatch;});negative.commands.erase(first);for(size_t i=0;i<negative.commands.size();++i)negative.commands[i].sequence=i;encode_frame(negative,out/L"missing-dispatch.gpa_frame");negative=frames[0];std::erase_if(negative.commands,[](const auto& c){return c.kind==CommandKind::draw;});for(size_t i=0;i<negative.commands.size();++i)negative.commands[i].sequence=i;encode_frame(negative,out/L"missing-draws.gpa_frame");}
    std::cout<<"Deferred execution snapshots, repeated order, native leases and explicit failures passed; source objects alive=0\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
