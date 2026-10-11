#include "command_stream.hpp"
#include "objects.hpp"
#include "encoder.hpp"
#include "query_dependencies.hpp"
#include <algorithm>
#include <cstring>
#include <iostream>
#include <thread>
using namespace flora::capture;
using Microsoft::WRL::ComPtr;
namespace {
void require(bool v,const char* reason){if(!v)throw std::runtime_error(reason);}
void check(HRESULT hr){require(SUCCEEDED(hr),"Native query fixture failed");}
template<class F>void rejects(F&& f,const char* reason){try{f();}catch(const std::exception& e){if(std::string(e.what()).find(reason)!=std::string::npos)return;throw;}throw std::runtime_error("Expected query failure was absent");}
struct Fixture {
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    Fixture(bool hardware){check(D3D11CreateDevice(nullptr,hardware?D3D_DRIVER_TYPE_HARDWARE:D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context));wrap_object(device.GetAddressOf(),"test::Device");wrap_object(context.GetAddressOf(),"test::Context");}
    ComPtr<ID3D11Query> event(bool extended=false){
        ComPtr<ID3D11Query> value;
        if(extended){ComPtr<ID3D11Device3> device3;check(device.As(&device3));D3D11_QUERY_DESC1 desc{D3D11_QUERY_EVENT,0,D3D11_CONTEXT_TYPE_ALL};ComPtr<ID3D11Query1> query1;check(device3->CreateQuery1(&desc,&query1));check(query1.As(&value));}
        else{D3D11_QUERY_DESC desc{D3D11_QUERY_EVENT,0};check(device->CreateQuery(&desc,&value));}
        return value;
    }
    void wait(ID3D11Query* event){const auto until=GetTickCount64()+5000;for(;;){const auto result=unwrap(context.Get())->GetData(unwrap(event),nullptr,0,0);check(result);if(result==S_OK)return;require(GetTickCount64()<until,"EVENT completion timed out");Sleep(1);}}
    ~Fixture(){context->ClearState();context->Flush();}
};
void observations(bool hardware){
    for(bool extended:{false,true}){
    Fixture f(hardware);auto event=f.event(extended);const auto id=object_record(event.Get())->id,context=object_record(f.context.Get())->id;
    auto recording=begin_recording(f.context.Get());
    BOOL untouched=0x1234;
    const auto unissued=f.context->GetData(event.Get(),&untouched,sizeof untouched,D3D11_ASYNC_GETDATA_DONOTFLUSH);
    f.context->End(event.Get());f.wait(event.Get());
    BOOL ready=FALSE;const auto completed=f.context->GetData(event.Get(),&ready,sizeof ready,D3D11_ASYNC_GETDATA_DONOTFLUSH);
    require(completed==S_OK && ready==TRUE,"Native EVENT did not report completion");ready=FALSE;
    f.context->End(event.Get());f.wait(event.Get());
    check(f.context->GetData(event.Get(),nullptr,0,0));
    const auto frame=recording->finish();frame.require_complete();
    require(frame.commands.size()==5 && frame.initial_resources.empty(),"Query observations issued snapshots or lost calls");
    const auto& first=frame.commands[0];require(first.result==unissued && first.query->last_end.generation==0 && !first.query->end_sequence,"Unobserved End history was invented");
    require(first.query->value==(unissued==S_OK?std::optional<uint32_t>{uint32_t(untouched)}:std::optional<uint32_t>{}),"Pending output became a ready result");
    for(const auto& command:frame.commands)require(command.operand==id && command.completed && command.bindings.context==context,"Query identity/context/observation lost");
    const auto& a=*frame.commands[2].query;const auto& b=*frame.commands[4].query;
    require(a.last_end.generation==1 && a.end_sequence==1 && a.value==TRUE && a.has_output,"First interval or owned BOOL lost");
    require(b.last_end.generation==2 && b.end_sequence==3 && !b.value && !b.has_output,"Reused query matched an old interval");
    require(a.last_end.context==context && a.last_end.thread==GetCurrentThreadId() && a.last_end.immediate,"End issuer provenance lost");
    auto pre=begin_recording(f.context.Get());check(f.context->GetData(event.Get(),nullptr,0,0));const auto before=pre->finish();before.require_complete();
    require(before.commands[0].query->last_end.generation==2 && !before.commands[0].query->end_sequence,"Pre-frame End was given an in-frame sequence");
    auto replacement=f.event(extended);require(object_record(replacement.Get())->id!=id && object_record(replacement.Get())->last_query_end.generation==0,"New query inherited old history");
    // Record-only deferred End is not proof that a command list was executed.
    ComPtr<ID3D11DeviceContext> deferred;check(f.device->CreateDeferredContext(0,&deferred));deferred->End(replacement.Get());
    const auto stamp=object_record(replacement.Get())->last_query_end;
    require(stamp.generation==1 && !stamp.immediate && stamp.context==object_record(deferred.Get())->id,"Deferred End was mistaken for immediate execution");
    }
}
void failures(bool hardware){
    Fixture f(hardware);auto event=f.event();f.context->End(event.Get());f.wait(event.Get());
    // Inject the native return into the post-call observer; invalid output is
    // deliberately never passed to D3D. Pending/failed callbacks must not read it.
    auto recording=begin_recording(f.context.Get());
    const auto* inaccessible=reinterpret_cast<const void*>(uintptr_t(1));
    record_query_observation(f.context.Get(),event.Get(),CommandKind::query_get_data,S_FALSE,inaccessible,4,1);
    record_query_observation(f.context.Get(),event.Get(),CommandKind::query_get_data,E_INVALIDARG,inaccessible,77,0);
    const auto injected=recording->finish();injected.require_complete();require(injected.commands.size()==2,"Pending/failure observations omitted");
    for(const auto& c:injected.commands)require(c.query->has_output && !c.query->value,"Unavailable query output was dereferenced or fabricated");
    recording.reset();recording=begin_recording(f.context.Get());BOOL value=0x123456;
    const auto result=f.context->GetData(event.Get(),&value,1,0);require(FAILED(result),"Invalid native GetData size unexpectedly succeeded");
    const auto failed=recording->finish();failed.require_complete();require(failed.commands.size()==1 && failed.commands[0].result==result && !failed.commands[0].query->value,"Native failed query output was consumed");
    recording.reset();RecordingOptions options;options.byte_limit=3;recording=begin_recording(f.context.Get(),options);value=FALSE;
    check(f.context->GetData(event.Get(),&value,4,0));require(value==TRUE,"Budget rejection changed native query result");auto budget=recording->finish();rejects([&]{budget.require_complete();},"byte limit");
    recording.reset();options={};options.command_limit=0;recording=begin_recording(f.context.Get(),options);f.context->End(event.Get());auto limit=recording->finish();rejects([&]{limit.require_complete();},"command limit");
    recording.reset();recording=begin_recording(f.context.Get());std::thread worker([&]{f.context->GetData(event.Get(),nullptr,0,0);});worker.join();auto foreign=recording->finish();rejects([&]{foreign.require_complete();},"foreign-thread");
    recording.reset();recording=begin_recording(f.context.Get());{NativeCall nested;f.context->GetData(event.Get(),nullptr,0,0);}auto reentrant=recording->finish();rejects([&]{reentrant.require_complete();},"Reentrant");
    recording.reset();recording=begin_recording(f.context.Get());recording->cancel();record_query_observation(f.context.Get(),event.Get(),CommandKind::query_get_data,S_OK,inaccessible,4,0);auto cancelled=recording->finish();rejects([&]{cancelled.require_complete();},"cancelled");
}
FrozenFrame frozen(){
    FrozenFrame result;
    {
        Fixture f(true);auto event=f.event();D3D11_TEXTURE2D_DESC d{};d.Width=d.Height=4;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;d.Format=DXGI_FORMAT_R8G8B8A8_UNORM;d.BindFlags=D3D11_BIND_RENDER_TARGET;
        UINT blue[16];std::fill_n(blue,16,0xffff0000u);D3D11_SUBRESOURCE_DATA data{blue,16,64};ComPtr<ID3D11Texture2D> texture;check(f.device->CreateTexture2D(&d,&data,&texture));ComPtr<ID3D11RenderTargetView> view;check(f.device->CreateRenderTargetView(texture.Get(),nullptr,&view));
        D3D11_BUFFER_DESC bd{16,D3D11_USAGE_DEFAULT,0,0,0,0};UINT sourceWords[]{7,11,13,17},zeroWords[4]{};D3D11_SUBRESOURCE_DATA sourceData{sourceWords,0,0},zeroData{zeroWords,0,0};ComPtr<ID3D11Buffer> source,staging;check(f.device->CreateBuffer(&bd,&sourceData,&source));bd.Usage=D3D11_USAGE_STAGING;bd.CPUAccessFlags=D3D11_CPU_ACCESS_READ;check(f.device->CreateBuffer(&bd,&zeroData,&staging));
        auto recording=begin_recording(f.context.Get());f.context->CopyResource(staging.Get(),source.Get());f.context->End(event.Get());f.wait(event.Get());BOOL ready=FALSE;check(f.context->GetData(event.Get(),&ready,4,0));
        const float green[]{0,1,0,1};f.context->ClearRenderTargetView(view.Get(),green);auto frame=recording->finish();auto final=snapshot_resource(f.context.Get(),texture.Get());const auto id=final.record->id;result=freeze_frame(std::move(frame),id,std::move(final));
        const auto copied=snapshot_resource(f.context.Get(),staging.Get());require(copied.subresources[0].bytes.size()==sizeof sourceWords && !memcmp(copied.subresources[0].bytes.data(),sourceWords,sizeof sourceWords),"Native copy/query ordering changed application bytes");
    }
    require(object_status().at("active")==0 && metadata_status().at("alive")==0,"Frozen query model retained native objects");
    require(result.commands[2].query->value==TRUE && result.commands[2].query->end_sequence==1,"Frozen query result/history changed");
    const auto dependencies=analyze_event_dependencies(result);
    require(dependencies.ends==1 && dependencies.observations.size()==1 && dependencies.observations[0].scope==EventObservationScope::recorded_prefix && dependencies.observations[0].prefix->uncovered_commands==0,"Frozen End included a later Clear or lost its scope");
    const auto& copies=dependencies.observations[0].prefix->copies;require(copies.size()==1 && copies[0].sequence==0 && copies[0].source==result.commands[0].source && copies[0].destination==result.commands[0].operand,"Frozen native copy dependency lost");return result;
}
}
int wmain(int argc,wchar_t** argv){try{
    if(argc==3 && std::wstring(argv[2])==L"self-test"){
        const std::filesystem::path out=argv[1];require(!std::filesystem::exists(out),"Query output exists");std::filesystem::create_directories(out);const auto frame=frozen();
        const auto reason="EVENT query serialization is not verified; frame not published (ID3D11DeviceContext::End, object="+std::to_string(frame.commands[1].operand)+", sequence=1)";
        rejects([&]{encode_frame(frame,out/L"query.gpa_frame");},reason.c_str());require(std::filesystem::is_empty(out),"Unsupported query published/leaked a file");std::cout<<"Owned EVENT freeze and located encoder rejection passed\n";
    }else{const bool hardware=argc==2 && std::wstring(argv[1])==L"--hardware";observations(hardware);failures(hardware);require(object_status().at("active")==0 && metadata_status().at("alive")==0 && object_status().at("interface_gaps")==0,"Query tracking leaked native objects or capabilities");std::cout<<"EVENT generations, output ownership, pre-frame history and failure boundaries passed\n";}
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
