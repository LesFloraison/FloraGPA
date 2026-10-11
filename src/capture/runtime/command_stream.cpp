#include "command_stream.hpp"
#include "objects.hpp"
#include "capture_session.hpp"
#include "frame_start.hpp"
#include <cstring>

namespace flora::capture {
using Microsoft::WRL::ComPtr;
namespace {
struct Registry {std::mutex mutex;std::weak_ptr<FrameRecording> active;std::atomic<bool> enabled=false;};
Registry& registry(){static Registry value;return value;}
struct LastErrorScope{DWORD value=GetLastError();~LastErrorScope(){SetLastError(value);}};
std::string query_gap(IUnknown* object,const char* api,const std::array<uint32_t,5>& words){
    std::string reason="Query recording is not implemented";
    auto record=object_record(object);
    if(!record)return reason+" (untracked asynchronous object)";
    std::lock_guard lock(record->mutex);
    reason+=" (object="+std::to_string(record->id)+", kind="+record->kind;
    if((record->kind=="query" || record->kind=="query1") && record->descriptor.size()>=sizeof(D3D11_QUERY_DESC)){
        D3D11_QUERY_DESC desc{};memcpy(&desc,record->descriptor.data(),sizeof(desc));
        constexpr const char* names[]{"EVENT","OCCLUSION","TIMESTAMP","TIMESTAMP_DISJOINT","PIPELINE_STATISTICS",
            "OCCLUSION_PREDICATE","SO_STATISTICS","SO_OVERFLOW_PREDICATE","SO_STATISTICS_STREAM0",
            "SO_OVERFLOW_PREDICATE_STREAM0","SO_STATISTICS_STREAM1","SO_OVERFLOW_PREDICATE_STREAM1",
            "SO_STATISTICS_STREAM2","SO_OVERFLOW_PREDICATE_STREAM2","SO_STATISTICS_STREAM3","SO_OVERFLOW_PREDICATE_STREAM3"};
        reason+=", type="+std::string(UINT(desc.Query)<std::size(names)?names[UINT(desc.Query)]:"UNKNOWN")+
            "("+std::to_string(UINT(desc.Query))+"), misc_flags="+std::to_string(desc.MiscFlags);
        if(record->kind=="query1" && record->descriptor.size()>=sizeof(D3D11_QUERY_DESC1)){
            D3D11_QUERY_DESC1 extended{};memcpy(&extended,record->descriptor.data(),sizeof(extended));
            reason+=", context_type="+std::to_string(UINT(extended.ContextType));
        }
    }
    if(std::string_view(api).ends_with("::GetData"))
        reason+=", data_size="+std::to_string(words[0])+", getdata_flags="+std::to_string(words[1]);
    return reason+"): query intervals, result observations and completion dependencies require verified GPA encoding";
}
ObjectId retain(ContextSnapshot& snapshot,IUnknown* object){
    if(!object)return 0;
    ComPtr<IUnknown> native;
    if(FAILED(unwrap(object)->QueryInterface(IID_PPV_ARGS(&native))))throw std::runtime_error("Command operand identity unavailable");
    auto record=find_native_record(native.Get());if(!record)throw std::runtime_error("Command operand is not tracked");
    snapshot.objects.try_emplace(record->id,RetainedObject{record,native});
    ComPtr<ID3D11View> view;
    if(SUCCEEDED(native.As(&view))){ComPtr<ID3D11Resource> resource;view->GetResource(&resource);retain(snapshot,resource.Get());}
    return record->id;
}
}
struct FrameRecording::Impl {
    std::mutex mutex;
    RecordingOptions options;
    ComPtr<ID3D11DeviceContext> context;
    ObjectId context_id=0;
    DWORD thread=GetCurrentThreadId();
    std::atomic<bool> cancelled=false;
    // Generated API names are static string literals. Retain the first failing
    // call without allocation or waiting for the application/driver lock.
    std::atomic<const char*> foreign_call=nullptr,reentrant_call=nullptr;
    bool finished=false,starting=true;
    struct Mapping {size_t command;D3D11_MAPPED_SUBRESOURCE mapped{};};
    std::map<std::pair<ObjectId,UINT>,Mapping> mappings;
    std::map<ObjectId,std::pair<uint64_t,uint64_t>> query_ends; // generation, command sequence
    size_t payload_bytes=0;
    RecordedFrame frame;
    void gap(const char* api,const char* reason) noexcept {
        frame.failed=true;
        try{if(frame.gaps.size()<32)frame.gaps.push_back(std::string(api)+": "+reason);}catch(...){}
    }
};
FrameRecording::FrameRecording(ID3D11DeviceContext* context,const RecordingOptions& options):impl_(std::make_unique<Impl>()){
    if(!context)throw std::runtime_error("Recording requires a context");
    auto record=object_record(context);if(!record)throw std::runtime_error("Recording context is not tracked");
    if(unwrap(context)->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)throw std::runtime_error("Recording requires an immediate context");
    {std::lock_guard lock(record->mutex);if(!record->active_queries.empty())throw std::runtime_error("Recording cannot start inside an active query");}
    if(has_external_mapping())throw std::runtime_error("Recording cannot start with an active DXGI mapping");
    if(has_external_mapping(true))throw std::runtime_error("Recording cannot start with an active resource mapping");
    if(object_status().at("interface_gaps").get<uint64_t>())throw std::runtime_error("Recording has prior interface capability gaps");
    impl_->context=context;impl_->context_id=record->id;impl_->options=options;
}
FrameRecording::~FrameRecording()=default;
void FrameRecording::complete_start(){
    auto& p=*impl_;FrameStartOptions options;options.timeout_ms=p.options.readback_timeout_ms;
    options.cancelled=[&p]{
        if(p.options.cancelled && p.options.cancelled())p.cancelled=true;
        if(p.foreign_call.load())throw std::runtime_error("Concurrent or foreign-thread call during frame start completion");
        if(p.reentrant_call.load())throw std::runtime_error("Reentrant application call during frame start completion");
        return p.cancelled.load();
    };
    p.frame.start_completion=complete_frame_start(p.context.Get(),options);
    if(options.cancelled())throw std::runtime_error("Frame start completion cancelled");
    p.starting=false;
}
void FrameRecording::cancel() noexcept {impl_->cancelled.store(true);}
void RecordedFrame::require_complete() const {
    if(cancelled)throw std::runtime_error("Frame recording cancelled");
    if(failed)throw std::runtime_error(gaps.empty()?"Frame recording failed":gaps.front());
}
FrozenFrame freeze_frame(RecordedFrame&& frame,ObjectId output,ResourceSnapshot&& final_output){
    frame.require_complete();
    if(!final_output.record || final_output.record->id!=output || !frame.initial_resources.contains(output))
        throw std::runtime_error("Frame output lacks matching initial/final resource data");
    FrozenFrame result;result.output=output;result.start_completion=frame.start_completion;
    for(const auto& [id,held]:frame.objects){
        auto& r=*held.record;std::lock_guard lock(r.mutex);
        FrozenObject object;object.id=id;object.device=r.device;object.resource=r.resource;object.class_linkage=r.class_linkage;
        object.kind=r.kind;object.descriptor=r.descriptor;object.creation_bytes=r.creation_bytes;
        object.input_elements=r.input_elements;object.stream_elements=r.stream_elements;object.stream_strides=r.stream_strides;
        object.rasterized_stream=r.rasterized_stream;object.creation_observed=r.creation_observed;
        result.objects.emplace(id,std::move(object));
    }
    for(auto& [id,resource]:frame.initial_resources)result.initial_resources.emplace(id,ResourceContents{resource.shape,std::move(resource.subresources)});
    result.commands=std::move(frame.commands);result.final_output={final_output.shape,std::move(final_output.subresources)};
    frame.objects.clear();frame.initial_resources.clear();return result;
}
std::shared_ptr<FrameRecording> begin_recording(ID3D11DeviceContext* context,const RecordingOptions& options){
    LastErrorScope last_error;
    auto next=std::shared_ptr<FrameRecording>(new FrameRecording(context,options));
    auto& r=registry();
    {std::lock_guard lock(r.mutex);if(r.active.lock())throw std::runtime_error("Frame recording busy");r.active=next;r.enabled.store(true);}
    try{next->complete_start();}
    catch(...){std::lock_guard lock(r.mutex);if(auto active=r.active.lock();active==next){r.active.reset();r.enabled.store(false);}throw;}
    return next;
}
RecordedFrame FrameRecording::finish(){
    LastErrorScope last_error;
    auto& p=*impl_;
    if(GetCurrentThreadId()!=p.thread)throw std::runtime_error("Recording must finish on its application thread");
    std::lock_guard lock(p.mutex);
    if(p.finished)throw std::runtime_error("Frame recording already finished");
    if(auto api=p.foreign_call.load())p.gap(api,"Concurrent or foreign-thread context call is unsupported");
    if(auto api=p.reentrant_call.load())p.gap(api,"Reentrant application context execution is unsupported");
    ComPtr<ID3D11Device> device;unwrap(p.context.Get())->GetDevice(&device);
    if(FAILED(device->GetDeviceRemovedReason()))p.gap("finish","Device removed during recording");
    if(object_status().at("interface_gaps").get<uint64_t>())p.gap("finish","Interface capability gap during recording");
    if(!p.mappings.empty())p.gap("finish","Frame ends with an active resource mapping");
    p.mappings.clear(); // No mapped pointer escapes the application-thread recording.
    p.finished=true;p.frame.cancelled=p.cancelled.load();
    auto& r=registry();{std::lock_guard guard(r.mutex);if(auto active=r.active.lock();active.get()==this){r.active.reset();r.enabled.store(false);}}
    return std::move(p.frame);
}
void FrameRecording::observe(ID3D11DeviceContext* context,const char* api,CommandKind kind,const std::array<uint32_t,5>& words,IUnknown* operand,const float* floats,const UINT* integers,IUnknown* source) {
    auto& p=*impl_;
    // Never wait behind capture readback while a foreign caller may own the
    // driver's ID3D11Multithread lock. Mark failure and forward immediately.
    if(GetCurrentThreadId()!=p.thread){const char* empty=nullptr;p.foreign_call.compare_exchange_strong(empty,api);return;}
    if(native_call_depth>1 || p.starting){const char* empty=nullptr;p.reentrant_call.compare_exchange_strong(empty,api);return;}
    std::lock_guard lock(p.mutex);
    if(p.finished || p.frame.failed || p.cancelled.load() || p.foreign_call.load() || p.reentrant_call.load())return;
    try {
        auto record=object_record(context);
        if(!record || record->id!=p.context_id)throw std::runtime_error("Multiple context execution requires ordered command-list support");
        if(kind==CommandKind::read || kind==CommandKind::binding)return;
        if(kind==CommandKind::unsupported){
            const std::string_view name=api;
            if(name.ends_with("::Begin") || name.ends_with("::End") || name.ends_with("::GetData"))
                throw std::runtime_error(query_gap(operand,api,words));
            throw std::runtime_error("GPU call recording is not implemented");
        }
        if(!p.mappings.empty() && kind!=CommandKind::map)throw std::runtime_error("GPU execution while a resource is mapped is unsupported");
        if(kind==CommandKind::map && (!operand || words[1]<1 || words[1]>5 || (words[2]&~D3D11_MAP_FLAG_DO_NOT_WAIT)))throw std::runtime_error("Invalid Map arguments");
        if(kind==CommandKind::copy_resource && (!operand || !source))throw std::runtime_error("CopyResource operand is missing");
        const bool clear=kind==CommandKind::clear_rtv || kind==CommandKind::clear_dsv || kind==CommandKind::clear_uav_uint || kind==CommandKind::clear_uav_float;
        if(clear && !operand)throw std::runtime_error("Clear operand is missing");
        if(clear && (kind==CommandKind::clear_uav_uint?!integers:!floats))throw std::runtime_error("Clear value is missing");
        if(p.frame.commands.size()>=p.options.command_limit)throw std::runtime_error("Frame command limit exceeded");
        auto snapshot=snapshot_context(context);snapshot.require_supported_bindings();
        RecordedCommand command;command.kind=kind;command.api=api;command.sequence=p.frame.commands.size();command.words=words;
        if(floats)std::copy_n(floats,4,command.floats.begin());
        if(integers)std::copy_n(integers,4,command.words.begin());
        command.operand=retain(snapshot,operand);
        command.source=retain(snapshot,source);
        if(kind==CommandKind::copy_resource && command.operand==command.source)throw std::runtime_error("CopyResource operands alias");
        SnapshotOptions options;options.timeout_ms=p.options.readback_timeout_ms;options.cancelled=[&p]{if(p.options.cancelled && p.options.cancelled())p.cancelled=true;return p.cancelled.load();};
        for(const auto& [id,object]:snapshot.objects){
            if(object.record->kind!="device" && object.record->device!=snapshot.bindings.device)
                throw std::runtime_error("Command operand belongs to another device");
            ComPtr<ID3D11Resource> resource;
            if(SUCCEEDED(object.native.As(&resource)) && !p.frame.initial_resources.contains(id)){
                options.byte_limit=p.options.byte_limit-p.frame.initial_bytes-p.payload_bytes;
                auto data=snapshot_resource(context,resource.Get(),options);
                for(const auto& sub:data.subresources)p.frame.initial_bytes+=sub.bytes.size();
                p.frame.initial_resources.emplace(id,std::move(data));
            }
            ComPtr<ID3D11UnorderedAccessView> uav;
            if(SUCCEEDED(object.native.As(&uav))){
                D3D11_UNORDERED_ACCESS_VIEW_DESC desc{};uav->GetDesc(&desc);
                if(desc.ViewDimension==D3D11_UAV_DIMENSION_BUFFER && (desc.Buffer.Flags&(D3D11_BUFFER_UAV_FLAG_APPEND|D3D11_BUFFER_UAV_FLAG_COUNTER))){
                    options.byte_limit=4;command.uav_counters.emplace(id,snapshot_uav_counter(context,uav.Get(),options).value);
                }
            }
        }
        if(kind==CommandKind::copy_resource){
            const auto& destination=p.frame.initial_resources.at(command.operand);
            validate_copy_resource(destination.shape,destination.record->descriptor,p.frame.initial_resources.at(command.source).shape);
        }
        if(kind==CommandKind::map){
            const auto& shape=p.frame.initial_resources.at(command.operand).shape;
            const auto layout=resource_layout(shape);
            if(words[0]>=layout.size())throw std::runtime_error("Map subresource is out of range");
            if(shape.format>=DXGI_FORMAT_NV12 && shape.format<=DXGI_FORMAT_P016)throw std::runtime_error("Planar Map wire layout is not verified");
            if(words[1]==D3D11_MAP_WRITE_NO_OVERWRITE){
                if(shape.dimension!=D3D11_RESOURCE_DIMENSION_BUFFER)throw std::runtime_error("Texture NO_OVERWRITE is unsupported");
                ComPtr<ID3D11Resource> resource;if(FAILED(unwrap(operand)->QueryInterface(IID_PPV_ARGS(&resource))))throw std::runtime_error("Map operand is not a resource");
                options.byte_limit=p.options.byte_limit-p.frame.initial_bytes-p.payload_bytes;
                auto before=snapshot_resource(context,resource.Get(),options);command.before_data=std::move(before.subresources.at(words[0]).bytes);p.payload_bytes+=command.before_data.size();
            }
            const auto key=std::pair{command.operand,words[0]};
            if(!p.mappings.emplace(key,Impl::Mapping{p.frame.commands.size()}).second)throw std::runtime_error("Overlapping Map calls");
        }
        command.bindings=std::move(snapshot.bindings);p.frame.objects.merge(snapshot.objects);
        p.frame.commands.push_back(std::move(command));
    }catch(const std::exception& e){p.gap(api,e.what());}catch(...){p.gap(api,"Command observation failed");}
}
void FrameRecording::query(ID3D11DeviceContext* context,ID3D11Asynchronous* async,CommandKind kind,HRESULT result,const void* data,UINT size,UINT flags){
    auto& p=*impl_;const auto* api=kind==CommandKind::query_end?"ID3D11DeviceContext::End":"ID3D11DeviceContext::GetData";
    if(GetCurrentThreadId()!=p.thread){const char* empty=nullptr;p.foreign_call.compare_exchange_strong(empty,api);return;}
    if(native_call_depth>1 || p.starting){const char* empty=nullptr;p.reentrant_call.compare_exchange_strong(empty,api);return;}
    std::lock_guard lock(p.mutex);
    if(p.finished || p.frame.failed || p.cancelled || p.foreign_call.load() || p.reentrant_call.load())return;
    try{
        if(kind!=CommandKind::query_end && kind!=CommandKind::query_get_data)throw std::runtime_error("Invalid query observation kind");
        auto c=object_record(context),q=object_record(async);
        if(!c || c->id!=p.context_id)throw std::runtime_error("Multiple context query execution is unsupported");
        if(!q || q->device!=c->device)throw std::runtime_error("Query identity/device mismatch");
        D3D11_QUERY_DESC desc{};QueryObservation observation;bool supported=false;
        {
            std::lock_guard query_lock(q->mutex);
            if((q->kind=="query" || q->kind=="query1") && q->descriptor.size()>=sizeof(desc)){
                memcpy(&desc,q->descriptor.data(),sizeof(desc));supported=desc.Query==D3D11_QUERY_EVENT && desc.MiscFlags==0;
                if(q->kind=="query1"){
                    if(q->descriptor.size()!=sizeof(D3D11_QUERY_DESC1))supported=false;
                    else{D3D11_QUERY_DESC1 extended{};memcpy(&extended,q->descriptor.data(),sizeof(extended));supported=supported && extended.ContextType==D3D11_CONTEXT_TYPE_ALL;}
                }
            }
            observation.last_end=q->last_query_end;
        }
        if(!supported)throw std::runtime_error(query_gap(async,api,{size,flags}));
        if(p.frame.commands.size()>=p.options.command_limit)throw std::runtime_error("Frame command limit exceeded");
        const auto& stamp=observation.last_end;
        if(kind==CommandKind::query_end){
            if(!stamp.generation || stamp.context!=p.context_id || stamp.thread!=p.thread || !stamp.immediate)
                throw std::runtime_error("EVENT End observation does not match native history");
            observation.end_sequence=p.frame.commands.size();
        }else{
            if(result>=0 && result!=S_OK && result!=S_FALSE)throw std::runtime_error("Unexpected GetData success status");
            if(flags&~D3D11_ASYNC_GETDATA_DONOTFLUSH)throw std::runtime_error("Unsupported GetData flags");
            observation.has_output=data!=nullptr;
            if(result==S_OK){
                if((data && size!=sizeof(BOOL)) || (!data && size))throw std::runtime_error("Invalid successful EVENT GetData output size");
                if(data){
                    if(p.options.byte_limit-p.frame.initial_bytes-p.payload_bytes<sizeof(BOOL))throw std::runtime_error("Query result exceeds frame byte limit");
                    uint32_t value;memcpy(&value,data,sizeof(value));observation.value=value;
                }
            }
            if(auto end=p.query_ends.find(q->id);end!=p.query_ends.end() && end->second.first==stamp.generation)
                observation.end_sequence=end->second.second;
        }
        // These getters/identity operations issue no GPU work. In particular,
        // do not call snapshot_context/resource or poll the query internally.
        ContextSnapshot held;held.bindings.context=p.context_id;held.bindings.device=c->device;
        retain(held,context);retain(held,async);ComPtr<ID3D11Device> device;unwrap(context)->GetDevice(&device);retain(held,device.Get());
        RecordedCommand command;command.kind=kind;command.api=api;command.sequence=p.frame.commands.size();
        command.operand=q->id;command.words={size,flags};command.result=result;command.completed=true;
        command.query=observation;command.bindings=std::move(held.bindings);
        p.frame.commands.push_back(std::move(command));p.frame.objects.merge(held.objects);
        if(observation.value)p.payload_bytes+=sizeof(BOOL);
        if(kind==CommandKind::query_end)p.query_ends[q->id]={stamp.generation,p.frame.commands.back().sequence};
    }catch(const std::exception& e){p.gap(api,e.what());}catch(...){p.gap(api,"Query observation failed");}
}
void FrameRecording::mapped(ID3D11DeviceContext* context,ID3D11Resource* resource,UINT subresource,HRESULT result,const D3D11_MAPPED_SUBRESOURCE* mapped){
    auto& p=*impl_;constexpr auto api="ID3D11DeviceContext::Map";
    if(GetCurrentThreadId()!=p.thread){const char* empty=nullptr;p.foreign_call.compare_exchange_strong(empty,api);return;}
    if(native_call_depth>1 || p.starting){const char* empty=nullptr;p.reentrant_call.compare_exchange_strong(empty,api);return;}
    std::lock_guard lock(p.mutex);if(p.finished || p.frame.failed || p.cancelled || p.foreign_call.load() || p.reentrant_call.load())return;
    try{
        auto c=object_record(context),r=object_record(resource);
        if(!c || c->id!=p.context_id || !r)throw std::runtime_error("Map context/resource identity mismatch");
        auto found=p.mappings.find({r->id,subresource});if(found==p.mappings.end())throw std::runtime_error("Map result has no recorded call");
        auto& command=p.frame.commands.at(found->second.command);command.result=result;
        if(FAILED(result)){p.payload_bytes-=command.before_data.size();command.before_data.clear();command.completed=true;p.mappings.erase(found);return;}
        if(result!=S_OK || !mapped || !mapped->pData)throw std::runtime_error("Successful Map without CPU storage is unsupported");
        if(command.words[2] && command.words[1]>=D3D11_MAP_WRITE_DISCARD)throw std::runtime_error("Invalid successful Map flags/type");
        found->second.mapped=*mapped;
    }catch(const std::exception& e){p.gap(api,e.what());}catch(...){p.gap(api,"Map result observation failed");}
}
void FrameRecording::unmap(ID3D11DeviceContext* context,ID3D11Resource* resource,UINT subresource){
    auto& p=*impl_;constexpr auto api="ID3D11DeviceContext::Unmap";
    if(GetCurrentThreadId()!=p.thread){const char* empty=nullptr;p.foreign_call.compare_exchange_strong(empty,api);return;}
    if(native_call_depth>1 || p.starting){const char* empty=nullptr;p.reentrant_call.compare_exchange_strong(empty,api);return;}
    std::lock_guard lock(p.mutex);if(p.finished || p.frame.failed || p.cancelled || p.foreign_call.load() || p.reentrant_call.load()){p.mappings.clear();return;}
    try{
        auto c=object_record(context),r=object_record(resource);
        if(!c || c->id!=p.context_id || !r)throw std::runtime_error("Unmap context/resource identity mismatch");
        auto found=p.mappings.find({r->id,subresource});if(found==p.mappings.end())throw std::runtime_error("Unmap has no recorded successful Map");
        if(p.frame.commands.size()>=p.options.command_limit)throw std::runtime_error("Frame command limit exceeded");
        auto& command=p.frame.commands.at(found->second.command);const auto& mapped=found->second.mapped;
        auto sub=resource_layout(p.frame.initial_resources.at(r->id).shape).at(subresource);
        const size_t bytes=sub.slice_bytes*sub.depth;
        if(bytes>p.options.byte_limit-p.frame.initial_bytes-p.payload_bytes)throw std::runtime_error("Mapped data byte limit exceeded");
        // Copy before native Unmap invalidates the application's returned pointer.
        // Padding is excluded; only checked logical rows/slices enter the CPU model.
        copy_pitched(sub,mapped.pData,mapped.RowPitch,mapped.DepthPitch);
        command.data=std::move(sub.bytes);command.completed=true;p.payload_bytes+=bytes;
        RecordedCommand end;end.kind=CommandKind::unmap;end.api=api;end.sequence=p.frame.commands.size();end.operand=r->id;end.words[0]=subresource;
        end.bindings.context=command.bindings.context;end.bindings.device=command.bindings.device;end.completed=true;
        p.frame.commands.push_back(std::move(end));p.mappings.erase(found);
    }catch(const std::exception& e){p.mappings.clear();p.gap(api,e.what());}catch(...){p.mappings.clear();p.gap(api,"Mapped data capture failed");}
}
void FrameRecording::reject_external(const char* api) {
    auto& p=*impl_;
    if(GetCurrentThreadId()!=p.thread){const char* empty=nullptr;p.foreign_call.compare_exchange_strong(empty,api);return;}
    if(p.starting){const char* empty=nullptr;p.reentrant_call.compare_exchange_strong(empty,api);return;}
    if(native_call_depth>1){const char* empty=nullptr;p.reentrant_call.compare_exchange_strong(empty,api);return;}
    std::lock_guard lock(p.mutex);if(!p.finished)p.gap(api,"External resource mutation or synchronization is not recorded");
}
void record_map_result(ID3D11DeviceContext* context,ID3D11Resource* resource,UINT subresource,HRESULT result,const D3D11_MAPPED_SUBRESOURCE* mapped) noexcept {
    auto& r=registry();if(!r.enabled.load(std::memory_order_relaxed))return;LastErrorScope error;
    try{std::shared_ptr<FrameRecording> active;{std::lock_guard lock(r.mutex);active=r.active.lock();}if(active)active->mapped(context,resource,subresource,result,mapped);}
    catch(...){object_gap("Map",__uuidof(ID3D11DeviceContext),"Map observer failed");}
}
void record_unmap(ID3D11DeviceContext* context,ID3D11Resource* resource,UINT subresource) noexcept {
    capture_context_activity();auto& r=registry();if(!r.enabled.load(std::memory_order_relaxed))return;LastErrorScope error;
    try{std::shared_ptr<FrameRecording> active;{std::lock_guard lock(r.mutex);active=r.active.lock();}if(active)active->unmap(context,resource,subresource);}
    catch(...){object_gap("Unmap",__uuidof(ID3D11DeviceContext),"Unmap observer failed");}
}
void record_external_call(const char* api) noexcept {
    capture_context_activity();
    auto& r=registry();if(!r.enabled.load(std::memory_order_relaxed))return;
    LastErrorScope last_error;
    try{std::shared_ptr<FrameRecording> active;{std::lock_guard lock(r.mutex);active=r.active.lock();}if(active)active->reject_external(api);}
    catch(...){object_gap(api,__uuidof(IUnknown),"External frame observer failed");}
}
void record_query_observation(ID3D11DeviceContext* context,ID3D11Asynchronous* async,CommandKind kind,HRESULT result,const void* data,UINT size,UINT flags) noexcept {
    capture_context_activity();auto& r=registry();if(!r.enabled.load(std::memory_order_relaxed))return;
    LastErrorScope last_error;
    try{std::shared_ptr<FrameRecording> active;{std::lock_guard lock(r.mutex);active=r.active.lock();}
        if(active)active->query(context,async,kind,result,data,size,flags);
    }catch(...){object_gap("Query observation",__uuidof(ID3D11DeviceContext),"Frame observer failed");}
}
void record_context_call(ID3D11DeviceContext* context,const char* api,CommandKind kind,std::array<uint32_t,5> words,IUnknown* operand,const float* floats,const UINT* integers,IUnknown* source) noexcept {
    capture_context_activity();
    auto& r=registry();if(!r.enabled.load(std::memory_order_relaxed))return;
    LastErrorScope last_error;
    try {
        std::shared_ptr<FrameRecording> active;{std::lock_guard lock(r.mutex);active=r.active.lock();}
        if(active)active->observe(context,api,kind,words,operand,floats,integers,source);
    }catch(...){object_gap(api,__uuidof(ID3D11DeviceContext),"Frame observer failed");}
}
}
