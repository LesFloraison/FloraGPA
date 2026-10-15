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
    Fixture(bool hardware,D3D_FEATURE_LEVEL level=D3D_FEATURE_LEVEL(0)){check(D3D11CreateDevice(nullptr,hardware?D3D_DRIVER_TYPE_HARDWARE:D3D_DRIVER_TYPE_WARP,nullptr,0,level?&level:nullptr,level?1:0,D3D11_SDK_VERSION,&device,nullptr,&context));wrap_object(device.GetAddressOf(),"test::Device");wrap_object(context.GetAddressOf(),"test::Context");}
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
    recording.reset();options={};options.serialized_context_threads=true;recording=begin_recording(f.context.Get(),options);
    std::thread unprotected([&]{record_query_observation(f.context.Get(),event.Get(),CommandKind::query_get_data,S_OK,nullptr,0,0);});unprotected.join();
    auto raw=recording->finish();rejects([&]{raw.require_complete();},"foreign-thread");
    recording.reset();recording=begin_recording(f.context.Get());{NativeCall nested;f.context->GetData(event.Get(),nullptr,0,0);}auto reentrant=recording->finish();rejects([&]{reentrant.require_complete();},"Reentrant");
    recording.reset();recording=begin_recording(f.context.Get());recording->cancel();record_query_observation(f.context.Get(),event.Get(),CommandKind::query_get_data,S_OK,inaccessible,4,0);auto cancelled=recording->finish();rejects([&]{cancelled.require_complete();},"cancelled");
}
void serialized_maps(bool hardware){
    Fixture f(hardware);D3D11_BUFFER_DESC desc{16,D3D11_USAGE_DYNAMIC,D3D11_BIND_CONSTANT_BUFFER,D3D11_CPU_ACCESS_WRITE,0,0};
    UINT initial[4]{};D3D11_SUBRESOURCE_DATA data{initial,0,0};ComPtr<ID3D11Buffer> buffers[2];
    for(auto& buffer:buffers)check(f.device->CreateBuffer(&desc,&data,&buffer));
    RecordingOptions options;options.serialized_context_threads=true;auto recording=begin_recording(f.context.Get(),options);
    std::exception_ptr errors[2];std::thread workers[2];
    for(unsigned i=0;i<2;++i)workers[i]=std::thread([&,i]{try{
        D3D11_MAPPED_SUBRESOURCE mapped{};check(f.context->Map(buffers[i].Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped));
        memset(mapped.pData,int(0x31+i),16);f.context->Unmap(buffers[i].Get(),0);
    }catch(...){errors[i]=std::current_exception();}});
    for(auto& worker:workers)worker.join();for(const auto& error:errors)if(error)std::rethrow_exception(error);
    const auto frame=recording->finish();frame.require_complete();require(frame.commands.size()==4,"Serialized Map commands missing");
    unsigned maps=0;for(const auto& c:frame.commands)if(c.kind==CommandKind::map){
        const unsigned i=c.operand==object_record(buffers[0].Get())->id?0:1;
        require(c.completed && c.data==std::vector<uint8_t>(16,uint8_t(0x31+i)),"Worker Map bytes were not owned before Unmap");++maps;
    }
    require(maps==2 && !has_external_mapping(true),"Worker mappings did not close");
}
void frame_start(bool hardware){
    std::cerr<<"frame start: completion\n";
    Fixture f(hardware);auto a=f.event(),b=f.event();
    D3D11_BUFFER_DESC desc{16,D3D11_USAGE_DEFAULT,0,0,0,0};UINT words[]{2,3,5,7};D3D11_SUBRESOURCE_DATA data{words,0,0};ComPtr<ID3D11Buffer> source,staging;
    check(f.device->CreateBuffer(&desc,&data,&source));desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;check(f.device->CreateBuffer(&desc,nullptr,&staging));
    f.context->CopyResource(staging.Get(),source.Get());f.context->End(a.Get());f.context->End(b.Get());f.context->End(a.Get());
    require(object_record(a.Get())->last_query_end.context_serial==3 && object_record(b.Get())->last_query_end.context_serial==2,"End serial not ordered across query identities");
    D3D11_VIEWPORT viewport{1,2,8,9,0,1};f.context->RSSetViewports(1,&viewport);
    const auto alive=metadata_status().at("alive"),active=object_status().at("active");SetLastError(0x123456);
    auto recording=begin_recording(f.context.Get());require(GetLastError()==0x123456,"Frame start changed LastError");
    require(alive==metadata_status().at("alive") && active==object_status().at("active"),"Private EVENT entered object tracking");
    BOOL ready=FALSE;require(f.context->GetData(a.Get(),&ready,4,D3D11_ASYNC_GETDATA_DONOTFLUSH)==S_OK && ready,"Frame start failed to drain earlier EVENT");
    UINT count=1;D3D11_VIEWPORT actual{};f.context->RSGetViewports(&count,&actual);require(count==1 && !memcmp(&viewport,&actual,sizeof actual),"Frame start mutated bindings");
    unsigned busyCalls=0;RecordingOptions busy;busy.cancelled=[&]{++busyCalls;return false;};rejects([&]{begin_recording(f.context.Get(),busy);},"busy");require(!busyCalls,"Busy recording started a second GPU wait");
    const auto frame=recording->finish();frame.require_complete();require(frame.start_completion && frame.start_completion->last_end_serial==3 && frame.start_completion->value==1 && frame.start_completion->result==S_OK,"Actual completion evidence missing");
    require(frame.commands.size()==1,"Private completion entered application commands");
    D3D11_MAPPED_SUBRESOURCE mapped{};check(unwrap(f.context.Get())->Map(unwrap(staging.Get()),0,D3D11_MAP_READ,0,&mapped));require(!memcmp(mapped.pData,words,sizeof words),"Frame start changed copied bytes");unwrap(f.context.Get())->Unmap(unwrap(staging.Get()),0);
    recording.reset();
    auto recover=[&]{auto r=begin_recording(f.context.Get());const auto recovered=r->finish();recovered.require_complete();require(recovered.commands.empty() && recovered.start_completion.has_value(),"Startup failure poisoned next recording");};
    std::cerr<<"frame start: timeout/cancel\n";
    RecordingOptions options;options.readback_timeout_ms=0;rejects([&]{begin_recording(f.context.Get(),options);},"timed out");recover();
    for(unsigned when:{1u,3u}){unsigned calls=0;options={};options.cancelled=[&]{return ++calls>=when;};rejects([&]{begin_recording(f.context.Get(),options);},"cancelled");require(calls==when,"Cancellation checkpoint skipped");recover();}
    std::cerr<<"frame start: foreign\n";
    options={};bool called=false;options.cancelled=[&]{if(!called){called=true;std::thread worker([&]{f.context->Flush();});worker.join();}return false;};
    rejects([&]{begin_recording(f.context.Get(),options);},"foreign-thread");recover();
    std::cerr<<"frame start: reentrant\n";
    options={};called=false;options.cancelled=[&]{if(!called){called=true;f.context->Flush();}return false;};
    rejects([&]{begin_recording(f.context.Get(),options);},"Reentrant");recover();
    ComPtr<ID3D11DeviceContext> deferred;check(f.device->CreateDeferredContext(0,&deferred));rejects([&]{begin_recording(deferred.Get());},"immediate context");recover();
    D3D11_QUERY_DESC pd{D3D11_QUERY_OCCLUSION_PREDICATE,0};ComPtr<ID3D11Predicate> predicate;check(f.device->CreatePredicate(&pd,&predicate));
    f.context->Begin(predicate.Get());f.context->End(predicate.Get());f.context->SetPredication(predicate.Get(),TRUE);
    rejects([&]{begin_recording(f.context.Get());},"predication");ComPtr<ID3D11Predicate> actualPredicate;BOOL predicateValue=FALSE;
    f.context->GetPredication(&actualPredicate,&predicateValue);require(actualPredicate==predicate && predicateValue==TRUE,"Rejected frame start changed predication");
    f.context->SetPredication(nullptr,FALSE);recover();
}
FrozenFrame timing(bool hardware,bool threaded=false,bool higher=false,bool empty_prefix=false,bool work_before_end=false){
    Fixture f(hardware,higher?D3D_FEATURE_LEVEL_12_1:D3D_FEATURE_LEVEL(0));D3D11_QUERY_DESC q{D3D11_QUERY_TIMESTAMP_DISJOINT,0};
    ComPtr<ID3D11Query> previous,active,stamp;check(f.device->CreateQuery(&q,&previous));check(f.device->CreateQuery(&q,&active));
    q.Query=D3D11_QUERY_TIMESTAMP;check(f.device->CreateQuery(&q,&stamp));
    f.context->Begin(previous.Get());f.context->End(stamp.Get());f.context->End(previous.Get());
    D3D11_TEXTURE2D_DESC d{};d.Width=d.Height=4;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;d.Format=DXGI_FORMAT_R8G8B8A8_UNORM;d.BindFlags=D3D11_BIND_RENDER_TARGET;
    UINT blue[16];std::fill_n(blue,16,0xffff0000u);D3D11_SUBRESOURCE_DATA init{blue,16,64};ComPtr<ID3D11Texture2D> texture;
    check(f.device->CreateTexture2D(&d,&init,&texture));ComPtr<ID3D11RenderTargetView> view;check(f.device->CreateRenderTargetView(texture.Get(),nullptr,&view));
    f.context->Begin(active.Get());RecordingOptions options;options.serialized_context_threads=threaded;auto recording=begin_recording(f.context.Get(),options);
    UINT64 ticks=0;D3D11_QUERY_DATA_TIMESTAMP_DISJOINT frequency{};
    const auto issue=[&]{
    require(f.context->GetData(stamp.Get(),&ticks,sizeof ticks,0)==S_OK,"Frame boundary did not complete earlier timestamp");
    require(f.context->GetData(previous.Get(),&frequency,sizeof frequency,0)==S_OK && frequency.Frequency,"Earlier timing interval not completed");
    const float green[]{0,1,0,1};
    if(empty_prefix){
        if(work_before_end)f.context->ClearRenderTargetView(view.Get(),green);
        f.context->End(stamp.Get());f.wait(stamp.Get());UINT64 current=0;
        require(f.context->GetData(stamp.Get(),&current,sizeof current,0)==S_OK,"Empty-prefix timestamp did not complete");
    }
    if(!work_before_end)f.context->ClearRenderTargetView(view.Get(),green);
    f.context->End(active.Get());f.context->Begin(active.Get());
    };
    if(threaded){std::exception_ptr failure;std::thread worker([&]{try{issue();}catch(...){failure=std::current_exception();}});worker.join();if(failure)std::rethrow_exception(failure);}else issue();
    auto frame=recording->finish();frame.require_complete();f.context->End(active.Get());
    require(frame.commands.size()==(empty_prefix?7u:5u) && frame.commands[0].query->value==uint32_t(ticks) && frame.commands[1].query->value==uint32_t(frequency.Frequency),"Legacy timing observation changed application output");
    auto final=snapshot_resource(f.context.Get(),texture.Get());const auto id=final.record->id;
    auto result=freeze_frame(std::move(frame),id,std::move(final));
    const auto analysis=analyze_event_dependencies(result);
    require(analysis.ends==(empty_prefix?2u:1u) && analysis.observations.size()==(empty_prefix?3u:2u) && analysis.observations[0].scope==EventObservationScope::preframe_completed && analysis.observations[1].scope==EventObservationScope::preframe_completed,"Timing boundary completion proof lost");
    if(threaded){auto unproven=result;unproven.start_completion->serialized_context_threads=false;rejects([&]{analyze_event_dependencies(unproven);},"End frame start order mismatch");}
    return result;
}
FrozenFrame mips(bool hardware){
    Fixture f(hardware);D3D11_TEXTURE2D_DESC d{};d.Width=d.Height=4;d.MipLevels=3;d.ArraySize=d.SampleDesc.Count=1;
    d.Format=DXGI_FORMAT_R8G8B8A8_UNORM;d.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;d.MiscFlags=D3D11_RESOURCE_MISC_GENERATE_MIPS;
    ComPtr<ID3D11Texture2D> source,output;check(f.device->CreateTexture2D(&d,nullptr,&source));
    ComPtr<ID3D11ShaderResourceView> view;check(f.device->CreateShaderResourceView(source.Get(),nullptr,&view));
    UINT blue[16],green[16];std::fill_n(blue,16,0xffff0000u);std::fill_n(green,16,0xff00ff00u);
    f.context->UpdateSubresource(source.Get(),0,nullptr,blue,16,64);f.context->GenerateMips(view.Get());
    d.Width=d.Height=d.MipLevels=1;d.MiscFlags=0;d.BindFlags=D3D11_BIND_RENDER_TARGET;
    D3D11_SUBRESOURCE_DATA initial{blue,4,4};check(f.device->CreateTexture2D(&d,&initial,&output));
    auto recording=begin_recording(f.context.Get());
    f.context->UpdateSubresource(source.Get(),0,nullptr,green,16,64);f.context->GenerateMips(view.Get());
    f.context->CopySubresourceRegion(output.Get(),0,0,0,0,source.Get(),2,nullptr);
    auto frame=recording->finish();frame.require_complete();
    require(frame.commands.size()==3 && frame.commands[1].kind==CommandKind::generate_mips,"GenerateMips command missing");
    auto final=snapshot_resource(f.context.Get(),output.Get());const auto id=final.record->id;
    require(final.subresources[0].bytes.size()==4 && !memcmp(final.subresources[0].bytes.data(),green,4),"Native mip generation failed");
    return freeze_frame(std::move(frame),id,std::move(final));
}
FrozenFrame frozen(bool preframe=false,bool extended=false){
    FrozenFrame result;
    {
        Fixture f(true);auto event=f.event(extended);D3D11_TEXTURE2D_DESC d{};d.Width=d.Height=4;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;d.Format=DXGI_FORMAT_R8G8B8A8_UNORM;d.BindFlags=D3D11_BIND_RENDER_TARGET;
        UINT blue[16];std::fill_n(blue,16,0xffff0000u);D3D11_SUBRESOURCE_DATA data{blue,16,64};ComPtr<ID3D11Texture2D> texture;check(f.device->CreateTexture2D(&d,&data,&texture));ComPtr<ID3D11RenderTargetView> view;check(f.device->CreateRenderTargetView(texture.Get(),nullptr,&view));
        D3D11_BUFFER_DESC bd{16,D3D11_USAGE_DEFAULT,0,0,0,0};UINT sourceWords[]{7,11,13,17},zeroWords[4]{};D3D11_SUBRESOURCE_DATA sourceData{sourceWords,0,0},zeroData{zeroWords,0,0};ComPtr<ID3D11Buffer> source,staging;check(f.device->CreateBuffer(&bd,&sourceData,&source));bd.Usage=D3D11_USAGE_STAGING;bd.CPUAccessFlags=D3D11_CPU_ACCESS_READ;check(f.device->CreateBuffer(&bd,&zeroData,&staging));
        if(preframe)f.context->End(event.Get());
        auto recording=begin_recording(f.context.Get());BOOL ready=FALSE;
        if(preframe)check(f.context->GetData(event.Get(),&ready,4,0));
        f.context->CopyResource(staging.Get(),source.Get());
        if(!preframe){f.context->End(event.Get());f.wait(event.Get());check(f.context->GetData(event.Get(),&ready,4,0));}
        const float green[]{0,1,0,1};f.context->ClearRenderTargetView(view.Get(),green);if(preframe)f.context->End(event.Get());auto frame=recording->finish();auto final=snapshot_resource(f.context.Get(),texture.Get());const auto id=final.record->id;result=freeze_frame(std::move(frame),id,std::move(final));
        const auto copied=snapshot_resource(f.context.Get(),staging.Get());require(copied.subresources[0].bytes.size()==sizeof sourceWords && !memcmp(copied.subresources[0].bytes.data(),sourceWords,sizeof sourceWords),"Native copy/query ordering changed application bytes");
    }
    require(object_status().at("active")==0 && metadata_status().at("alive")==0,"Frozen query model retained native objects");
    if(preframe){require(result.commands[0].query->value==TRUE && !result.commands[0].query->end_sequence,"Pre-frame provenance lost");return result;}
    require(result.commands[2].query->value==TRUE && result.commands[2].query->end_sequence==1,"Frozen query result/history changed");
    require(result.start_completion && result.start_completion->result==S_OK && result.start_completion->value==1,"Frozen frame lost owned completion evidence");
    const auto dependencies=analyze_event_dependencies(result);
    require(dependencies.ends==1 && dependencies.observations.size()==1 && dependencies.observations[0].scope==EventObservationScope::recorded_prefix && dependencies.observations[0].prefix->uncovered_commands==0,"Frozen End included a later Clear or lost its scope");
    const auto& copies=dependencies.observations[0].prefix->copies;require(copies.size()==1 && copies[0].sequence==0 && copies[0].source==result.commands[0].source && copies[0].destination==result.commands[0].operand,"Frozen native copy dependency lost");return result;
}
}
int wmain(int argc,wchar_t** argv){try{
    if(argc==3 && std::wstring(argv[2]).starts_with(L"mips")){
        auto frame=mips(true);
        if(std::wstring(argv[2])==L"mips-negative")std::erase_if(frame.commands,[](const auto& c){return c.kind==CommandKind::generate_mips;});
        for(size_t i=0;i<frame.commands.size();++i)frame.commands[i].sequence=i;
        encode_frame(frame,argv[1]);return 0;
    }
    if(argc==3 && std::wstring(argv[2]).starts_with(L"timing")){
        const std::wstring mode=argv[2];const bool prefix=mode.find(L"prefix")!=std::wstring::npos;
        auto frame=timing(true,mode.find(L"threaded")!=std::wstring::npos,mode.find(L"fl12")!=std::wstring::npos,mode.find(L"empty")!=std::wstring::npos || prefix,prefix);
        if(mode.ends_with(L"negative")){
            std::erase_if(frame.commands,[](const auto& c){return c.kind==CommandKind::clear_rtv;});
            std::map<size_t,size_t> sequences;for(size_t i=0;i<frame.commands.size();++i)sequences[frame.commands[i].sequence]=i;
            for(size_t i=0;i<frame.commands.size();++i){auto& c=frame.commands[i];c.sequence=i;if(c.query && c.query->end_sequence)c.query->end_sequence=sequences.at(*c.query->end_sequence);}
        }
        encode_frame(frame,argv[1]);return 0;
    }
    if(argc==3 && std::wstring(argv[2])==L"self-test"){
        const std::filesystem::path out=argv[1];require(!std::filesystem::exists(out),"Query output exists");std::filesystem::create_directories(out);const auto frame=frozen();
        const auto reason="EVENT query serialization is not verified; frame not published (ID3D11DeviceContext::End, object="+std::to_string(frame.commands[1].operand)+", sequence=1)";
        auto unproven=frame;unproven.start_completion.reset();rejects([&]{encode_frame(unproven,out/L"query.gpa_frame");},reason.c_str());
        const auto observed=frozen(true);
        for(unsigned i=0;i<5;++i){auto bad=observed;const char* diagnostic="EVENT query serialization is not verified";
            if(i==0)bad.start_completion.reset();
            if(i==1){bad.start_completion->value=0;diagnostic="Invalid frame start completion";}
            if(i==2){bad.commands[0].result=S_FALSE;bad.commands[0].query->value.reset();diagnostic="contradicts frame start";}
            if(i==3 || i==4){auto later=bad.commands.back();later.sequence=bad.commands.size();later.kind=CommandKind::query_get_data;later.api="ID3D11DeviceContext::GetData";later.result=i==3?S_FALSE:S_OK;later.words={4,0};later.query->has_output=true;if(i==4){later.query->value=1;later.query->end_sequence.reset();later.query->last_end={};diagnostic="GetData lost its in-frame End association";}bad.commands.push_back(std::move(later));}
            rejects([&]{encode_frame(bad,out/(std::to_wstring(i)+L".gpa_frame"));},diagnostic);
        }
        require(std::filesystem::is_empty(out),"Unsupported query published/leaked a file");std::cout<<"Six EVENT publication guards and owned pre-frame observations passed\n";
    }else if(argc==3){const std::wstring mode=argv[2];auto frame=frozen(mode.find(L"completion")==std::wstring::npos,mode.find(L"1")!=std::wstring::npos);if(mode.ends_with(L"negative")){std::erase_if(frame.commands,[](const auto& c){return c.kind==CommandKind::clear_rtv;});for(size_t i=0;i<frame.commands.size();++i){frame.commands[i].sequence=i;if(frame.commands[i].kind==CommandKind::query_end)frame.commands[i].query->end_sequence=i;}}encode_frame(frame,argv[1]);std::cout<<"{\"passed\":true,\"source_gpu_objects_alive\":0}\n";
    }else{const bool hardware=argc==2 && std::wstring(argv[1])==L"--hardware";std::cerr<<"observations\n";observations(hardware);std::cerr<<"failures\n";failures(hardware);frame_start(hardware);timing(hardware);serialized_maps(hardware);require(object_status().at("active")==0 && metadata_status().at("alive")==0 && object_status().at("interface_gaps")==0,"Query tracking leaked native objects or capabilities");std::cout<<"EVENT and timing observations, frame start completion and failure recovery passed\n";}
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
