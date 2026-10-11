#include "command_stream.hpp"
#include "objects.hpp"
#include "capture_session.hpp"
#include <cstring>

namespace flora::capture {
using Microsoft::WRL::ComPtr;
namespace {
struct Registry {std::mutex mutex;std::weak_ptr<FrameRecording> active;std::atomic<bool> enabled=false;};
Registry& registry(){static Registry value;return value;}
struct LastErrorScope{DWORD value=GetLastError();~LastErrorScope(){SetLastError(value);}};
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
    bool finished=false;
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
    if(object_status().at("interface_gaps").get<uint64_t>())throw std::runtime_error("Recording has prior interface capability gaps");
    impl_->context=context;impl_->context_id=record->id;impl_->options=options;
}
FrameRecording::~FrameRecording()=default;
void FrameRecording::cancel() noexcept {impl_->cancelled.store(true);}
void RecordedFrame::require_complete() const {
    if(cancelled)throw std::runtime_error("Frame recording cancelled");
    if(failed)throw std::runtime_error(gaps.empty()?"Frame recording failed":gaps.front());
}
FrozenFrame freeze_frame(RecordedFrame&& frame,ObjectId output,ResourceSnapshot&& final_output){
    frame.require_complete();
    if(!final_output.record || final_output.record->id!=output || !frame.initial_resources.contains(output))
        throw std::runtime_error("Frame output lacks matching initial/final resource data");
    FrozenFrame result;result.output=output;
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
    auto& r=registry();std::lock_guard lock(r.mutex);
    if(r.active.lock())throw std::runtime_error("Frame recording busy");
    r.active=next;r.enabled.store(true);return next;
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
    p.finished=true;p.frame.cancelled=p.cancelled.load();
    auto& r=registry();{std::lock_guard guard(r.mutex);if(auto active=r.active.lock();active.get()==this){r.active.reset();r.enabled.store(false);}}
    return std::move(p.frame);
}
void FrameRecording::observe(ID3D11DeviceContext* context,const char* api,CommandKind kind,const std::array<uint32_t,5>& words,IUnknown* operand,const float* floats,const UINT* integers) {
    auto& p=*impl_;
    // Never wait behind capture readback while a foreign caller may own the
    // driver's ID3D11Multithread lock. Mark failure and forward immediately.
    if(GetCurrentThreadId()!=p.thread){const char* empty=nullptr;p.foreign_call.compare_exchange_strong(empty,api);return;}
    if(native_call_depth>1){const char* empty=nullptr;p.reentrant_call.compare_exchange_strong(empty,api);return;}
    std::lock_guard lock(p.mutex);
    if(p.finished || p.frame.failed || p.cancelled.load())return;
    try {
        auto record=object_record(context);
        if(!record || record->id!=p.context_id)throw std::runtime_error("Multiple context execution requires ordered command-list support");
        if(kind==CommandKind::read || kind==CommandKind::binding)return;
        if(kind==CommandKind::unsupported)throw std::runtime_error("GPU call recording is not implemented");
        const bool clear=kind==CommandKind::clear_rtv || kind==CommandKind::clear_dsv || kind==CommandKind::clear_uav_uint || kind==CommandKind::clear_uav_float;
        if(clear && !operand)throw std::runtime_error("Clear operand is missing");
        if(clear && (kind==CommandKind::clear_uav_uint?!integers:!floats))throw std::runtime_error("Clear value is missing");
        if(p.frame.commands.size()>=p.options.command_limit)throw std::runtime_error("Frame command limit exceeded");
        auto snapshot=snapshot_context(context);snapshot.require_supported_bindings();
        RecordedCommand command;command.kind=kind;command.api=api;command.sequence=p.frame.commands.size();command.words=words;
        if(floats)std::copy_n(floats,4,command.floats.begin());
        if(integers)std::copy_n(integers,4,command.words.begin());
        command.operand=retain(snapshot,operand);
        SnapshotOptions options;options.timeout_ms=p.options.readback_timeout_ms;options.cancelled=[&p]{return p.cancelled.load();};
        for(const auto& [id,object]:snapshot.objects){
            ComPtr<ID3D11Resource> resource;
            if(SUCCEEDED(object.native.As(&resource)) && !p.frame.initial_resources.contains(id)){
                options.byte_limit=p.options.byte_limit-p.frame.initial_bytes;
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
        command.bindings=std::move(snapshot.bindings);p.frame.objects.merge(snapshot.objects);
        p.frame.commands.push_back(std::move(command));
    }catch(const std::exception& e){p.gap(api,e.what());}catch(...){p.gap(api,"Command observation failed");}
}
void FrameRecording::reject_external(const char* api) {
    auto& p=*impl_;
    if(GetCurrentThreadId()!=p.thread){const char* empty=nullptr;p.foreign_call.compare_exchange_strong(empty,api);return;}
    if(native_call_depth>1){const char* empty=nullptr;p.reentrant_call.compare_exchange_strong(empty,api);return;}
    std::lock_guard lock(p.mutex);if(!p.finished)p.gap(api,"External resource mutation or synchronization is not recorded");
}
void record_external_call(const char* api) noexcept {
    capture_context_activity();
    auto& r=registry();if(!r.enabled.load(std::memory_order_relaxed))return;
    LastErrorScope last_error;
    try{std::shared_ptr<FrameRecording> active;{std::lock_guard lock(r.mutex);active=r.active.lock();}if(active)active->reject_external(api);}
    catch(...){object_gap(api,__uuidof(IUnknown),"External frame observer failed");}
}
void record_context_call(ID3D11DeviceContext* context,const char* api,CommandKind kind,std::array<uint32_t,5> words,IUnknown* operand,const float* floats,const UINT* integers) noexcept {
    capture_context_activity();
    auto& r=registry();if(!r.enabled.load(std::memory_order_relaxed))return;
    LastErrorScope last_error;
    try {
        std::shared_ptr<FrameRecording> active;{std::lock_guard lock(r.mutex);active=r.active.lock();}
        if(active)active->observe(context,api,kind,words,operand,floats,integers);
    }catch(...){object_gap(api,__uuidof(ID3D11DeviceContext),"Frame observer failed");}
}
}
