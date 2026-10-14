#include "command_stream.hpp"
#include "objects.hpp"
#include "capture_session.hpp"
#include "frame_start.hpp"
#include "deferred_history.hpp"
#include "deferred_resources.hpp"
#include "deferred_lowering.hpp"
#include "msaa_readback.hpp"
#include <cstring>

namespace flora::capture {
using Microsoft::WRL::ComPtr;
namespace {
struct Registry {std::mutex mutex;std::weak_ptr<FrameRecording> active;std::atomic<bool> enabled=false;};
Registry& registry(){static Registry value;return value;}
struct LastErrorScope{DWORD value=GetLastError();~LastErrorScope(){SetLastError(value);}};
bool compilation_context(ID3D11DeviceContext* context) {
    const auto record=object_record(context);
    if(!record)return false; // Unknown identity must still fail in the frame observer.
    // Context kind/descriptor are immutable after wrapper publication. Do not
    // wait for mutable recording metadata before the foreign-thread guard.
    ContextRecordDesc descriptor{};
    if(record->kind!="context" || record->descriptor.size()!=sizeof descriptor)return false;
    memcpy(&descriptor,record->descriptor.data(),sizeof descriptor);
    return descriptor.type==D3D11_DEVICE_CONTEXT_DEFERRED;
}
void require_unmapped(const std::shared_ptr<ObjectRecord>& record){
    std::lock_guard lock(record->mutex);
    if(!record->mapped_subresources.empty()){
        const auto [context,subresource]=record->mapped_subresources.begin()->first;
        throw std::runtime_error("Command references an application-mapped resource (resource="+std::to_string(record->id)+", context="+std::to_string(context)+", subresource="+std::to_string(subresource)+")");
    }
}
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
    struct BoundaryMapping {MappingStamp stamp;ResourceSnapshot initial;};
    std::map<ObjectId,BoundaryMapping> boundary_mappings;
    std::map<ObjectId,std::pair<uint64_t,uint64_t>> query_ends; // generation, command sequence
    size_t payload_bytes=0;
    size_t msaa_planes=0;
    uint64_t deferred_executions=0;
    RecordedFrame frame;
    void snapshot(ID3D11DeviceContext* current,ID3D11Resource* resource,ObjectId id,SnapshotOptions readback){
        if(frame.initial_resources.contains(id) || frame.initial_msaa.contains(id))return;
        ComPtr<ID3D11Texture2D> texture;
        if(SUCCEEDED(resource->QueryInterface(IID_PPV_ARGS(&texture)))){
            D3D11_TEXTURE2D_DESC desc{};texture->GetDesc(&desc);
            if(desc.SampleDesc.Count>1){
                // Keep the live recording profile inside the independently
                // validated encoder profile before executing any readback work.
                const bool depth=msaa_initialization_depth_format(desc.Format);
                const UINT output_bind=depth?D3D11_BIND_DEPTH_STENCIL:D3D11_BIND_RENDER_TARGET;
                if((!depth && !msaa_initialization_color_format(desc.Format)) ||
                   desc.SampleDesc.Quality || (desc.SampleDesc.Count!=2 && desc.SampleDesc.Count!=4) ||
                   desc.Usage!=D3D11_USAGE_DEFAULT || desc.CPUAccessFlags || desc.MiscFlags ||
                   (depth && !(desc.BindFlags&D3D11_BIND_DEPTH_STENCIL)) ||
                   (desc.BindFlags&~(output_bind|D3D11_BIND_SHADER_RESOURCE)))
                    throw std::runtime_error("MSAA recording initialization profile unsupported (resource="+std::to_string(id)+", format="+std::to_string(UINT(desc.Format))+", samples="+std::to_string(desc.SampleDesc.Count)+")");
                const auto count=size_t(desc.ArraySize)*desc.SampleDesc.Count;
                if(count>options.msaa_plane_limit-msaa_planes)throw std::runtime_error("MSAA recording plane limit exceeded");
                auto data=snapshot_msaa_samples(current,texture.Get(),readback);
                const auto layout=validate_msaa_initialization(data,desc,readback.byte_limit,options.msaa_plane_limit-msaa_planes);
                frame.initial_bytes+=layout.bytes;msaa_planes+=layout.planes;
                frame.initial_msaa.emplace(id,std::move(data));return;
            }
        }
        auto data=snapshot_resource(current,resource,readback);
        for(const auto& sub:data.subresources)frame.initial_bytes+=sub.bytes.size();
        frame.initial_resources.emplace(id,std::move(data));
    }
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
    // An untouched resource may remain CPU-mapped across this frame. Each
    // snapshot still refuses mapped GPU dependencies. Supported same-thread
    // WRITE_DISCARD buffers receive an owned CPU boundary snapshot at start.
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
    for(const auto& record:active_mapping_records()){
        if(options.cancelled())throw std::runtime_error("Boundary Map snapshot cancelled");
        std::lock_guard lock(record->mutex);
        if(!record->alive || record->device!=p.frame.start_completion->device || record->kind!="buffer" || record->descriptor.size()!=sizeof(D3D11_BUFFER_DESC))continue;
        auto found=record->mapped_subresources.find({p.context_id,0});if(found==record->mapped_subresources.end())continue;
        const auto& live=found->second;
        // Unsupported or foreign mappings remain untouched. Their in-frame
        // use/Unmap still fails with the original provenance diagnostic.
        if(live.thread!=p.thread || live.type!=D3D11_MAP_WRITE_DISCARD || live.flags || live.result!=S_OK || !live.storage.pData)continue;
        D3D11_BUFFER_DESC desc{};memcpy(&desc,record->descriptor.data(),sizeof desc);
        if(desc.Usage!=D3D11_USAGE_DYNAMIC || desc.CPUAccessFlags!=D3D11_CPU_ACCESS_WRITE || desc.MiscFlags || desc.StructureByteStride ||
           !desc.BindFlags || (desc.BindFlags&~(D3D11_BIND_VERTEX_BUFFER|D3D11_BIND_INDEX_BUFFER|D3D11_BIND_CONSTANT_BUFFER|D3D11_BIND_SHADER_RESOURCE)))continue;
        ResourceSnapshot initial;initial.record=record;initial.shape.dimension=D3D11_RESOURCE_DIMENSION_BUFFER;initial.shape.width=desc.ByteWidth;
        initial.subresources=resource_layout(initial.shape,p.options.byte_limit-p.payload_bytes);
        auto& sub=initial.subresources.at(0);copy_pitched(sub,live.storage.pData,live.storage.RowPitch,live.storage.DepthPitch);
        p.payload_bytes+=sub.bytes.size();p.boundary_mappings.emplace(record->id,Impl::BoundaryMapping{live,std::move(initial)});
    }
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
    if(!final_output.record || final_output.record->id!=output || !frame.initial_resources.contains(output)){
        throw std::runtime_error("Frame output lacks matching initial/final resource data (output "+std::to_string(output)+", initial resources "+std::to_string(frame.initial_resources.size())+")");
    }
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
    result.initial_msaa=std::move(frame.initial_msaa);
    result.initial_maps=std::move(frame.initial_maps);
    result.final_maps=std::move(frame.final_maps);
    result.objects.merge(frame.deferred_definitions);
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
    if(!p.frame.failed && !p.cancelled){try{
        for(const auto& [key,mapping]:p.mappings){
            auto& command=p.frame.commands.at(mapping.command);auto record=p.frame.initial_resources.at(key.first).record;
            std::lock_guard resource_lock(record->mutex);
            auto live=record->mapped_subresources.find({p.context_id,key.second});
            if(command.kind!=CommandKind::map || command.words[0] || command.words[1]!=D3D11_MAP_WRITE_DISCARD || command.words[2] || command.result!=S_OK ||
               record->kind!="buffer" || record->descriptor.size()!=sizeof(D3D11_BUFFER_DESC) || live==record->mapped_subresources.end() ||
               live->second.thread!=p.thread || !live->second.storage.pData || live->second.storage.pData!=mapping.mapped.pData)
                throw std::runtime_error("Frame ends with an active resource mapping outside the verified boundary profile");
            D3D11_BUFFER_DESC d{};memcpy(&d,record->descriptor.data(),sizeof d);
            if(d.Usage!=D3D11_USAGE_DYNAMIC || d.CPUAccessFlags!=D3D11_CPU_ACCESS_WRITE || d.MiscFlags || d.StructureByteStride || !d.BindFlags ||
                (d.BindFlags&~(D3D11_BIND_VERTEX_BUFFER|D3D11_BIND_INDEX_BUFFER|D3D11_BIND_CONSTANT_BUFFER|D3D11_BIND_SHADER_RESOURCE)))
                throw std::runtime_error("Frame ends with an active resource mapping outside the verified storage profile");
            auto sub=resource_layout(p.frame.initial_resources.at(key.first).shape,p.options.byte_limit-p.frame.initial_bytes-p.payload_bytes).at(0);
            copy_pitched(sub,live->second.storage.pData,live->second.storage.RowPitch,live->second.storage.DepthPitch);
            command.data=std::move(sub.bytes);command.completed=true;p.payload_bytes+=command.data.size();
            p.frame.final_maps.push_back({key.first,p.context_id,record->device,live->second.generation,command.sequence,p.thread});
        }
    }catch(const std::exception& e){p.gap("finish",e.what());}}
    p.mappings.clear(); // No mapped pointer escapes the application-thread recording.
    p.boundary_mappings.clear();
    p.finished=true;p.frame.cancelled=p.cancelled.load();
    auto& r=registry();{std::lock_guard guard(r.mutex);if(auto active=r.active.lock();active.get()==this){r.active.reset();r.enabled.store(false);}}
    return std::move(p.frame);
}
void FrameRecording::observe(ID3D11DeviceContext* context,const char* api,CommandKind kind,const std::array<uint32_t,5>& words,IUnknown* operand,const float* floats,const UINT* integers,IUnknown* source,const UpdateSource* update,const CopyRegion* region) {
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
            if(name.ends_with("::ExecuteCommandList")){
                auto list_record=object_record(operand);std::shared_ptr<const DeferredCommandList> list;
                if(list_record){std::lock_guard guard(list_record->mutex);list=list_record->command_list;}
                if(!list || list->device!=record->device)throw std::runtime_error("CommandList compilation metadata/device unavailable");
                if(p.deferred_executions==UINT64_MAX)throw std::runtime_error("Deferred execution ordinal overflow");
                DeferredOrigin origin{list_record->id,list->context,list->generation,++p.deferred_executions,0,words[0]!=0};
                auto expanded=lower_deferred(*list,origin,p.context_id,p.options.command_limit-p.frame.commands.size(),p.options.byte_limit-p.frame.initial_bytes-p.payload_bytes,true);
                const auto remaining=p.options.byte_limit-p.frame.initial_bytes-p.payload_bytes-expanded.payload_bytes;
                if(expanded.initial_counter_uses.size()>remaining/sizeof(UINT))throw std::runtime_error("CommandList counter snapshot byte limit exceeded");
                expanded.payload_bytes+=expanded.initial_counter_uses.size()*sizeof(UINT);
                auto leases=command_list_resources(operand);
                auto multisampled=[&](ObjectId id){
                    if(!id)return false;
                    if(!leases || !leases->contains(id))throw std::runtime_error("CommandList mutation resource lease missing");
                    ComPtr<ID3D11Resource> resource;
                    if(FAILED(leases->at(id).As(&resource)))throw std::runtime_error("CommandList mutation resource type mismatch");
                    return resource_shape(resource.Get()).samples>1;
                };
                // Flattening does not make an unsupported resource mutation
                // representable. Refuse before any initial GPU readback.
                for(const auto& command:expanded.commands){
                    const auto action=command.kind;
                    if((action==CommandKind::copy_resource || action==CommandKind::copy_region ||
                        action==CommandKind::update_subresource || action==CommandKind::map || action==CommandKind::unmap) &&
                       (multisampled(command.operand) || multisampled(command.source)))
                        throw std::runtime_error("Deferred MSAA mutation command is not verified");
                }
                ContextSnapshot held;retain(held,context);ComPtr<ID3D11Device> device;unwrap(context)->GetDevice(&device);retain(held,device.Get());
                SnapshotOptions options;options.timeout_ms=p.options.readback_timeout_ms;options.cancelled=[&p]{if(p.options.cancelled&&p.options.cancelled())p.cancelled=true;return p.cancelled.load();};
                // Sample at this immediate execution position, never at Finish
                // or from creation data. Earlier recorded writes already own a
                // frame-start snapshot and must not be replaced on later runs.
                for(auto id:expanded.resources){
                    if(!leases || !leases->contains(id))throw std::runtime_error("CommandList resource lease missing: "+std::to_string(id));
                    ComPtr<ID3D11Resource> resource;if(FAILED(leases->at(id).As(&resource)))throw std::runtime_error("CommandList resource lease type mismatch");
                    if(retain(held,resource.Get())!=id)throw std::runtime_error("CommandList resource identity changed");
                    require_unmapped(held.objects.at(id).record);
                    options.byte_limit=p.options.byte_limit-p.frame.initial_bytes-p.payload_bytes-expanded.payload_bytes;
                    p.snapshot(context,resource.Get(),id,options);
                }
                std::map<ObjectId,uint32_t> counters;
                for(const auto& [id,index]:expanded.initial_counter_uses){
                    (void)index;if(!leases || !leases->contains(id))throw std::runtime_error("CommandList counter view lease missing: "+std::to_string(id));
                    ComPtr<ID3D11UnorderedAccessView> view;if(FAILED(leases->at(id).As(&view)))throw std::runtime_error("CommandList counter view lease type mismatch");
                    if(retain(held,view.Get())!=id)throw std::runtime_error("CommandList counter view identity changed");
                    options.byte_limit=sizeof(UINT);const auto counter=snapshot_uav_counter(context,view.Get(),options);
                    if(counter.view->id!=id)throw std::runtime_error("CommandList counter snapshot identity changed");
                    counters.emplace(id,counter.value);
                }
                complete_deferred_counters(expanded,counters);
                if(options.cancelled())throw std::runtime_error("CommandList resource snapshot cancelled");
                p.frame.objects.merge(held.objects);p.frame.deferred_definitions.merge(expanded.definitions);p.payload_bytes+=expanded.payload_bytes;
                for(auto& command:expanded.commands){command.sequence=p.frame.commands.size();p.frame.commands.push_back(std::move(command));}
                return;
            }
            if(name.ends_with("::Begin") || name.ends_with("::End") || name.ends_with("::GetData"))
                throw std::runtime_error(query_gap(operand,api,words));
            throw std::runtime_error("GPU call recording is not implemented");
        }
        if(indirect_argument_bytes(kind)){
            ComPtr<ID3D11Buffer> buffer;
            if(!operand || FAILED(unwrap(operand)->QueryInterface(IID_PPV_ARGS(&buffer))))throw std::runtime_error("Indirect argument buffer missing");
            D3D11_BUFFER_DESC desc{};buffer->GetDesc(&desc);validate_indirect_arguments(desc,kind,words[0]);
        }
        if(kind==CommandKind::map && (!operand || words[1]<1 || words[1]>5 || (words[2]&~D3D11_MAP_FLAG_DO_NOT_WAIT)))throw std::runtime_error("Invalid Map arguments");
        if((kind==CommandKind::copy_structure_count || kind==CommandKind::copy_resource || kind==CommandKind::copy_region) && (!operand || !source))throw std::runtime_error("CopyResource operand is missing");
        if(kind==CommandKind::copy_region && !region)throw std::runtime_error("CopySubresourceRegion coordinates missing");
        if(kind==CommandKind::resolve && (!operand || !source))throw std::runtime_error("Resolve operand is missing");
        const bool clear=kind==CommandKind::clear_rtv || kind==CommandKind::clear_dsv || kind==CommandKind::clear_uav_uint || kind==CommandKind::clear_uav_float;
        if(kind==CommandKind::update_subresource && (!operand || !update || !update->data || update->flags))throw std::runtime_error("UpdateSubresource requires nonnull data and zero copy flags");
        if(clear && !operand)throw std::runtime_error("Clear operand is missing");
        if(clear && (kind==CommandKind::clear_uav_uint?!integers:!floats))throw std::runtime_error("Clear value is missing");
        if(p.frame.commands.size()>=p.options.command_limit)throw std::runtime_error("Frame command limit exceeded");
        ContextSnapshot snapshot;
        if(kind==CommandKind::map || kind==CommandKind::resolve || kind==CommandKind::copy_structure_count){
            // Map touches its operand, not the currently bound graphics or
            // compute pipeline. Other bound resources may still be mapped.
            snapshot.bindings.context=p.context_id;snapshot.bindings.device=record->device;
            retain(snapshot,context);ComPtr<ID3D11Device> device;unwrap(context)->GetDevice(&device);retain(snapshot,device.Get());
            if(kind==CommandKind::copy_structure_count){
                ComPtr<ID3D11Predicate> predicate;BOOL value=FALSE;unwrap(context)->GetPredication(&predicate,&value);
                if(predicate)throw std::runtime_error("Predicated CopyStructureCount is not verified");
                ComPtr<ID3D11Buffer> destination,source_buffer;ComPtr<ID3D11UnorderedAccessView> view;ComPtr<ID3D11Resource> resource;
                if(FAILED(unwrap(operand)->QueryInterface(IID_PPV_ARGS(&destination))) || FAILED(unwrap(source)->QueryInterface(IID_PPV_ARGS(&view))))throw std::runtime_error("CopyStructureCount operand type invalid");
                view->GetResource(&resource);if(FAILED(resource.As(&source_buffer)))throw std::runtime_error("CopyStructureCount source is not a buffer");
                D3D11_BUFFER_DESC d{},s{};D3D11_UNORDERED_ACCESS_VIEW_DESC v{};destination->GetDesc(&d);source_buffer->GetDesc(&s);view->GetDesc(&v);
                if(destination.Get()==source_buffer.Get())throw std::runtime_error("CopyStructureCount same-resource copy is not verified");
                validate_copy_structure_count(d,v,s,words[0]);
            }
            if(kind==CommandKind::resolve){
                ComPtr<ID3D11Predicate> predicate;BOOL value=FALSE;unwrap(context)->GetPredication(&predicate,&value);
                if(predicate)throw std::runtime_error("Predicated Resolve is not verified");
                UINT support=0;if(FAILED(device->CheckFormatSupport(DXGI_FORMAT(words[2]),&support)) || !(support&D3D11_FORMAT_SUPPORT_MULTISAMPLE_RESOLVE))
                    throw std::runtime_error("Resolve format is unsupported by the device");
            }
        }else{snapshot=snapshot_context(context);snapshot.require_supported_bindings();}
        RecordedCommand command;command.kind=kind;command.api=api;command.sequence=p.frame.commands.size();command.words=words;
        if(kind==CommandKind::update_subresource && update->box){command.update_box.emplace();static_assert(sizeof(ResourceBox)==sizeof(D3D11_BOX));memcpy(command.update_box->data(),update->box,sizeof(D3D11_BOX));}
        if(kind==CommandKind::copy_region){command.copy_region=*region;command.completed=true;}
        if(floats)std::copy_n(floats,4,command.floats.begin());
        if(integers)std::copy_n(integers,4,command.words.begin());
        command.operand=retain(snapshot,operand);
        command.source=retain(snapshot,source);
        if(kind==CommandKind::resolve || kind==CommandKind::copy_structure_count)command.completed=true;
        if(kind==CommandKind::copy_resource && command.operand==command.source)throw std::runtime_error("CopyResource operands alias");
        SnapshotOptions options;options.timeout_ms=p.options.readback_timeout_ms;options.cancelled=[&p]{if(p.options.cancelled && p.options.cancelled())p.cancelled=true;return p.cancelled.load();};
        for(const auto& [id,object]:snapshot.objects){
            // Validate on every use, even if an earlier command already saved
            // this resource's initial data. Other mapped resources may coexist.
            require_unmapped(object.record);
            if(object.record->kind!="device" && object.record->device!=snapshot.bindings.device)
                throw std::runtime_error("Command operand belongs to another device");
            ComPtr<ID3D11Resource> resource;
            if(SUCCEEDED(object.native.As(&resource))){
                options.byte_limit=p.options.byte_limit-p.frame.initial_bytes-p.payload_bytes;
                p.snapshot(context,resource.Get(),id,options);
            }
            ComPtr<ID3D11UnorderedAccessView> uav;
            if(SUCCEEDED(object.native.As(&uav))){
                D3D11_UNORDERED_ACCESS_VIEW_DESC desc{};uav->GetDesc(&desc);
                if(desc.ViewDimension==D3D11_UAV_DIMENSION_BUFFER && (desc.Buffer.Flags&(D3D11_BUFFER_UAV_FLAG_APPEND|D3D11_BUFFER_UAV_FLAG_COUNTER))){
                    options.byte_limit=4;command.uav_counters.emplace(id,snapshot_uav_counter(context,uav.Get(),options).value);
                }
            }
        }
        if((kind==CommandKind::copy_resource || kind==CommandKind::copy_region || kind==CommandKind::update_subresource || kind==CommandKind::map) &&
           (p.frame.initial_msaa.contains(command.operand) || p.frame.initial_msaa.contains(command.source)))
            throw std::runtime_error("MSAA recording mutation command is not verified");
        if(kind==CommandKind::copy_resource){
            const auto& destination=p.frame.initial_resources.at(command.operand);
            validate_copy_resource(destination.shape,destination.record->descriptor,p.frame.initial_resources.at(command.source).shape);
        }
        if(kind==CommandKind::resolve){
            if(!p.frame.initial_msaa.contains(command.source) || !p.frame.initial_resources.contains(command.operand))
                throw std::runtime_error("Resolve requires preserved MSAA source and ordinary destination");
            const auto& destination=p.frame.initial_resources.at(command.operand);
            const auto& initial=p.frame.initial_msaa.at(command.source);
            validate_resolve(destination.shape,destination.record->descriptor,initial.shape,snapshot.objects.at(command.source).record->descriptor,
                words[0],words[1],DXGI_FORMAT(words[2]),command.operand==command.source);
        }
        if(kind==CommandKind::copy_region){
            const auto& destination=p.frame.initial_resources.at(command.operand);
            const auto& source_data=p.frame.initial_resources.at(command.source);
            validate_copy_region(destination.shape,destination.record->descriptor,source_data.shape,source_data.record->descriptor,*command.copy_region,command.operand==command.source);
        }
        if(kind==CommandKind::update_subresource){
            const auto& destination=p.frame.initial_resources.at(command.operand);
            auto layout=update_layout(destination.shape,destination.record->descriptor,words[0],command.update_box?&*command.update_box:nullptr);
            if(layout.slice_bytes*layout.depth>p.options.byte_limit-p.frame.initial_bytes-p.payload_bytes)throw std::runtime_error("Update payload byte limit exceeded");
            copy_pitched(layout,update->data,update->row_pitch,update->depth_pitch);
            command.data=std::move(layout.bytes);p.payload_bytes+=command.data.size();command.completed=true;
            command.words={words[0],update->row_pitch,update->depth_pitch,UINT(update->extended),update->flags};
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
        auto found=p.mappings.find({r->id,subresource});if(found==p.mappings.end()){
            if(auto boundary=p.boundary_mappings.find(r->id);boundary!=p.boundary_mappings.end() && subresource==0){
                if(p.frame.commands.size()>=p.options.command_limit)throw std::runtime_error("Frame command limit exceeded");
                auto& saved=boundary->second;std::lock_guard resource_lock(r->mutex);
                auto live=r->mapped_subresources.find({c->id,subresource});
                if(live==r->mapped_subresources.end() || live->second.generation!=saved.stamp.generation || live->second.thread!=p.thread || !live->second.storage.pData)
                    throw std::runtime_error("Boundary Map lifetime/generation changed");
                if(p.frame.initial_resources.contains(r->id))throw std::runtime_error("Boundary Map resource already used");
                auto sub=resource_layout(saved.initial.shape,p.options.byte_limit-p.frame.initial_bytes-p.payload_bytes).at(0);
                copy_pitched(sub,live->second.storage.pData,live->second.storage.RowPitch,live->second.storage.DepthPitch);
                InitialMap initial{r->id,c->id,c->device,saved.stamp.generation,saved.stamp.thread,std::move(sub.bytes)};
                p.frame.initial_bytes+=initial.data.size(); // Boundary bytes move out of the pending payload budget.
                p.frame.initial_maps.push_back(std::move(initial));
                p.frame.initial_resources.emplace(r->id,std::move(saved.initial));
                p.frame.objects.try_emplace(r->id,RetainedObject{r,{}});
                RecordedCommand end;end.kind=CommandKind::unmap;end.api=api;end.sequence=p.frame.commands.size();end.operand=r->id;
                end.bindings.context=c->id;end.bindings.device=c->device;end.completed=true;
                ContextSnapshot held;retain(held,context);ComPtr<ID3D11Device> device;unwrap(context)->GetDevice(&device);retain(held,device.Get());
                p.frame.objects.merge(held.objects);p.frame.commands.push_back(std::move(end));p.boundary_mappings.erase(boundary);return;
            }
            std::string reason="Unmap has no recorded successful Map (resource="+std::to_string(r->id)+", context="+std::to_string(c->id)+", subresource="+std::to_string(subresource);
            std::lock_guard resource_lock(r->mutex);
            if(auto history=r->mapped_subresources.find({c->id,subresource});history!=r->mapped_subresources.end()){
                const auto& stamp=history->second;
                reason+=", type="+std::to_string(UINT(stamp.type))+", flags="+std::to_string(stamp.flags)+", generation="+std::to_string(stamp.generation)+", map_thread="+std::to_string(stamp.thread)+", unmap_thread="+std::to_string(GetCurrentThreadId());
            }else reason+=", mapping_history=absent";
            throw std::runtime_error(reason+")");
        }
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
    remember_deferred_map_result(context,resource,subresource,result,mapped);
    auto& r=registry();if(!r.enabled.load(std::memory_order_relaxed))return;LastErrorScope error;
    try{if(compilation_context(context))return;std::shared_ptr<FrameRecording> active;{std::lock_guard lock(r.mutex);active=r.active.lock();}if(active)active->mapped(context,resource,subresource,result,mapped);}
    catch(...){object_gap("Map",__uuidof(ID3D11DeviceContext),"Map observer failed");}
}
void record_unmap(ID3D11DeviceContext* context,ID3D11Resource* resource,UINT subresource) noexcept {
    remember_deferred_unmap(context,resource,subresource);
    LastErrorScope error;
    try{if(compilation_context(context))return;capture_context_activity();auto& r=registry();if(!r.enabled.load(std::memory_order_relaxed))return;std::shared_ptr<FrameRecording> active;{std::lock_guard lock(r.mutex);active=r.active.lock();}if(active)active->unmap(context,resource,subresource);}
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
    LastErrorScope last_error;
    try{if(compilation_context(context))return;capture_context_activity();auto& r=registry();if(!r.enabled.load(std::memory_order_relaxed))return;std::shared_ptr<FrameRecording> active;{std::lock_guard lock(r.mutex);active=r.active.lock();}
        if(active)active->query(context,async,kind,result,data,size,flags);
    }catch(...){object_gap("Query observation",__uuidof(ID3D11DeviceContext),"Frame observer failed");}
}
void record_context_call(ID3D11DeviceContext* context,const char* api,CommandKind kind,std::array<uint32_t,5> words,IUnknown* operand,const float* floats,const UINT* integers,IUnknown* source,const UpdateSource* update,const CopyRegion* region) noexcept {
    observe_deferred_call(context,api,kind,words,operand,floats,integers,source,update,region);
    LastErrorScope last_error;
    try {
        // Compilation has its own bounded CPU journal and does not submit GPU
        // work. Only immediate ExecuteCommandList establishes frame order.
        // This also permits worker compilation during a Present boundary.
        if(compilation_context(context))return;
        capture_context_activity();
        auto& r=registry();if(!r.enabled.load(std::memory_order_relaxed))return;
        std::shared_ptr<FrameRecording> active;{std::lock_guard lock(r.mutex);active=r.active.lock();}
        if(active)active->observe(context,api,kind,words,operand,floats,integers,source,update,region);
    }catch(...){object_gap(api,__uuidof(ID3D11DeviceContext),"Frame observer failed");}
}
}
