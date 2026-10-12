#include "deferred_history.hpp"
#include "command_stream.hpp"
#include "objects.hpp"
#include <cstring>
namespace flora::capture {
namespace {
bool deferred(const ObjectRecord& r){ContextRecordDesc d{};if(r.kind!="context" || r.descriptor.size()!=sizeof d)return false;memcpy(&d,r.descriptor.data(),sizeof d);return d.type==D3D11_DEVICE_CONTEXT_DEFERRED;}
DeferredHistory& history(ObjectRecord& r){if(!r.deferred_history){r.deferred_history=std::make_shared<DeferredHistory>();r.deferred_history->pending.context=r.id;r.deferred_history->pending.device=r.device;}return *r.deferred_history;}
void gap(DeferredCommandList& batch,const std::string& reason){if(batch.gaps.size()<16)batch.gaps.push_back(reason);}
ObjectId identity(IUnknown* value){if(!value)return 0;const auto r=object_record(value);if(!r)throw std::runtime_error("Untracked deferred operand");return r->id;}
struct LastError{DWORD value=GetLastError();~LastError(){SetLastError(value);}};
template<class Desc> void dynamic_mapping(const ObjectRecord& r){
    if(r.descriptor.size()<sizeof(Desc))throw std::runtime_error("Deferred Map descriptor is incomplete");
    Desc d{};memcpy(&d,r.descriptor.data(),sizeof d);
    if(d.Usage!=D3D11_USAGE_DYNAMIC || !(d.CPUAccessFlags&D3D11_CPU_ACCESS_WRITE))throw std::runtime_error("Deferred Map requires CPU-writable dynamic storage");
}
SubresourceData mapped_layout(ID3D11Resource* resource,const ObjectRecord& r,UINT subresource){
    const auto s=resource_shape(unwrap(resource));
    switch(s.dimension){
    case D3D11_RESOURCE_DIMENSION_BUFFER:dynamic_mapping<D3D11_BUFFER_DESC>(r);break;
    case D3D11_RESOURCE_DIMENSION_TEXTURE1D:dynamic_mapping<D3D11_TEXTURE1D_DESC>(r);break;
    case D3D11_RESOURCE_DIMENSION_TEXTURE2D:dynamic_mapping<D3D11_TEXTURE2D_DESC>(r);break;
    case D3D11_RESOURCE_DIMENSION_TEXTURE3D:dynamic_mapping<D3D11_TEXTURE3D_DESC>(r);break;
    default:throw std::runtime_error("Deferred Map dimension unsupported");
    }
    return resource_layout(s,DeferredHistory::byte_limit).at(subresource);
}
}
ObjectId DeferredBindingBuilder::object_id(IUnknown* value){
    const auto r=object_record(value);if(!r || r->device!=device_)throw std::runtime_error("Deferred binding identity/device mismatch");return r->id;
}
void record_deferred_binding_impl(ID3D11DeviceContext* context,const char* api,const void* input,void(*build)(const void*,DeferredBindingBuilder&)) noexcept{
    LastError error;
    try{
        auto r=object_record(context);if(!r || !deferred(*r))return;
        std::lock_guard lock(r->mutex);auto& h=history(*r);auto& b=h.pending;
        try{
            if(native_call_depth>1 || b.recording_thread!=GetCurrentThreadId())throw std::runtime_error("Binding recording thread/reentrancy mismatch");
            if(!h.binding_pending)throw std::runtime_error("Binding has no preceding observation");
            auto& call=b.calls.at(*h.binding_pending);
            if(call.api!=api || call.kind!=CommandKind::binding || call.thread!=GetCurrentThreadId() || call.binding)throw std::runtime_error("Binding observation was interleaved or omitted");
            h.binding_pending.reset();
            if(b.owned_bytes>DeferredHistory::byte_limit)throw std::runtime_error("Deferred binding byte limit exceeded");
            DeferredBindingBuilder builder(r->device,DeferredHistory::byte_limit-b.owned_bytes);build(input,builder);
            b.owned_bytes+=builder.result.owned_bytes;call.binding=std::move(builder.result);
        }catch(const std::exception& e){gap(b,std::string(api)+": "+e.what());}
    }catch(...){object_gap(api,__uuidof(ID3D11DeviceContext),"Deferred binding metadata unavailable");}
}
void observe_deferred_call(ID3D11DeviceContext* context,const char* api,CommandKind kind,const std::array<uint32_t,5>& words,IUnknown* operand,const float* floats,const UINT* integers,IUnknown* source,const UpdateSource* update,const CopyRegion* region) noexcept{
    LastError error;std::shared_ptr<ObjectRecord> r;
    try{
        r=object_record(context);if(!r || !deferred(*r))return;
        const std::string_view name=api;
        if(name.ends_with("::FinishCommandList") || (kind==CommandKind::read && !name.ends_with("::End")))return;
        std::lock_guard lock(r->mutex);auto& h=history(*r);auto& b=h.pending;
        h.binding_pending.reset();
        if(kind==CommandKind::map && h.mappings.erase({identity(operand),words[0]}))gap(b,"Map: overlapping mapping discarded without reading old storage");
        if(native_call_depth>1){gap(b,std::string(api)+": reentrant deferred call");return;}
        if(!b.recording_thread)b.recording_thread=GetCurrentThreadId();
        else if(b.recording_thread!=GetCurrentThreadId())gap(b,std::string(api)+": recording thread changed; synchronization unproven");
        if(b.calls.size()>=DeferredHistory::call_limit){gap(b,std::string(api)+": deferred call limit exceeded");return;}
        DeferredCall c;c.api=api;c.kind=kind;c.sequence=b.calls.size();c.thread=GetCurrentThreadId();c.words=words;c.operand=identity(operand);c.source=identity(source);
        if(name.ends_with("::SetResourceMinLOD"))gap(b,std::string(api)+": SetResourceMinLOD is immediate-context only");
        if(floats)std::copy_n(floats,4,c.floats.begin());if(integers)std::copy_n(integers,4,c.words.begin());
        if(region)c.copy_region=*region;
        try{
            if(kind==CommandKind::update_subresource){
                if(!operand || !update || !update->data || update->flags)throw std::runtime_error("Deferred Update requires data and zero flags");
                if(update->box){c.update_box.emplace();memcpy(c.update_box->data(),update->box,sizeof(D3D11_BOX));}
                auto object=object_record(operand);if(!object || object->device!=r->device)throw std::runtime_error("Deferred Update device mismatch");
                Microsoft::WRL::ComPtr<ID3D11Resource> native;if(FAILED(unwrap(operand)->QueryInterface(IID_PPV_ARGS(&native))))throw std::runtime_error("Deferred Update operand is not a resource");
                const auto shape=resource_shape(native.Get());const auto layout=update_layout(shape,object->descriptor,words[0],c.update_box?&*c.update_box:nullptr);
                if(layout.slice_bytes*layout.depth>DeferredHistory::byte_limit-b.owned_bytes)throw std::runtime_error("Deferred payload byte limit exceeded");
                auto data=layout;copy_pitched(data,update->data,update->row_pitch,update->depth_pitch);c.data=std::move(data.bytes);b.owned_bytes+=c.data.size();c.words={words[0],update->row_pitch,update->depth_pitch,UINT(update->extended),update->flags};
            }else if(kind!=CommandKind::binding && kind!=CommandKind::map && kind!=CommandKind::clear_rtv && kind!=CommandKind::clear_dsv && kind!=CommandKind::clear_uav_uint && kind!=CommandKind::clear_uav_float && kind!=CommandKind::copy_resource && kind!=CommandKind::copy_region)
                throw std::runtime_error("Deferred state, mapping, query or command arguments require further tracking");
        }catch(const std::exception& e){gap(b,std::string(api)+": "+e.what());}
        if(kind==CommandKind::map){
            c.result=E_PENDING;c.completed=false;
            const auto key=std::pair{c.operand,words[0]};
            h.mappings.emplace(key,DeferredHistory::Mapping{b.calls.size(),GetCurrentThreadId()});
        }
        b.calls.push_back(std::move(c));
        if(kind==CommandKind::binding)h.binding_pending=b.calls.size()-1;
    }catch(...){object_gap(api,__uuidof(ID3D11DeviceContext),"Deferred call metadata unavailable");}
}
void remember_deferred_map_result(ID3D11DeviceContext* context,ID3D11Resource* resource,UINT subresource,HRESULT result,const D3D11_MAPPED_SUBRESOURCE* mapped) noexcept{
    LastError error;
    try{
        auto r=object_record(context);if(!r || !deferred(*r))return;
        auto object=object_record(resource);std::lock_guard lock(r->mutex);auto& h=history(*r);auto& b=h.pending;
        const auto key=std::pair{object?object->id:0,subresource};auto it=h.mappings.find(key);
        if(it==h.mappings.end()){gap(b,"Map: result has no pending call");return;}
        try{
            auto& c=b.calls.at(it->second.command);c.result=result;
            // Failed Map must never inspect the unspecified output structure.
            if(FAILED(result)){c.completed=true;h.mappings.erase(it);return;}
            if(it->second.thread!=GetCurrentThreadId() || native_call_depth>1)throw std::runtime_error("Map result thread/reentrancy mismatch");
            if(!object || object->device!=r->device)throw std::runtime_error("Map resource/device mismatch");
            if(result!=S_OK || !mapped || !mapped->pData)throw std::runtime_error("Successful Map has no supported CPU storage");
            if(c.words[2] || (c.words[1]!=D3D11_MAP_WRITE_DISCARD && c.words[1]!=D3D11_MAP_WRITE_NO_OVERWRITE))throw std::runtime_error("Deferred Map type/flags unsupported");
            it->second.layout=mapped_layout(resource,*object,subresource);
            it->second.mapped=*mapped;c.words[3]=mapped->RowPitch;c.words[4]=mapped->DepthPitch;
            if(c.words[1]==D3D11_MAP_WRITE_NO_OVERWRITE)gap(b,"Map: NO_OVERWRITE bytes are observations only; written-range and rename dependencies remain unresolved");
        }catch(const std::exception& e){h.mappings.erase(it);gap(b,std::string("Map: ")+e.what());}
    }catch(...){object_gap("Map",__uuidof(ID3D11DeviceContext),"Deferred Map result metadata unavailable");}
}
void remember_deferred_unmap(ID3D11DeviceContext* context,ID3D11Resource* resource,UINT subresource) noexcept{
    LastError error;
    try{
        auto r=object_record(context);if(!r || !deferred(*r))return;
        auto object=object_record(resource);std::lock_guard lock(r->mutex);auto& h=history(*r);auto& b=h.pending;
        auto node=h.mappings.extract({object?object->id:0,subresource});
        try{
            if(node.empty())throw std::runtime_error("Unmap has no successful live Map");
            // Drop borrowed storage from the registry on every path, before the
            // native Unmap executes, including budget and thread failures.
            auto& m=node.mapped();auto& c=b.calls.at(m.command);
            if(m.thread!=GetCurrentThreadId() || native_call_depth>1)throw std::runtime_error("Unmap thread/reentrancy mismatch");
            if(b.calls.size()>=DeferredHistory::call_limit)throw std::runtime_error("Deferred call limit exceeded");
            const auto bytes=m.layout.slice_bytes*m.layout.depth;
            if(b.owned_bytes>DeferredHistory::byte_limit || bytes>DeferredHistory::byte_limit-b.owned_bytes)throw std::runtime_error("Deferred payload byte limit exceeded");
            copy_pitched(m.layout,m.mapped.pData,m.mapped.RowPitch,m.mapped.DepthPitch);
            c.data=std::move(m.layout.bytes);b.owned_bytes+=c.data.size();c.completed=true;
            DeferredCall end;end.api="ID3D11DeviceContext::Unmap";end.kind=CommandKind::unmap;end.sequence=b.calls.size();end.operand=object->id;end.words[0]=subresource;b.calls.push_back(std::move(end));
        }catch(const std::exception& e){gap(b,std::string("Unmap: ")+e.what());}
    }catch(...){object_gap("Unmap",__uuidof(ID3D11DeviceContext),"Deferred Unmap metadata unavailable");}
}
void remember_deferred_finish(ID3D11DeviceContext* context,HRESULT result,BOOL restore,ID3D11CommandList* list) noexcept{
    LastError error;
    try{
        auto r=object_record(context);if(!r || !deferred(*r))return;
        auto output=list?object_record(list):nullptr;std::shared_ptr<const DeferredCommandList> completed;
        {std::lock_guard lock(r->mutex);auto& h=history(*r);if(FAILED(result)){++h.failed_finishes;return;}
            if(h.finishes==UINT64_MAX)throw std::runtime_error("Deferred generation overflow");
            for(const auto& call:h.pending.calls)if(call.kind==CommandKind::binding && !call.binding){gap(h.pending,call.api+": binding arguments unavailable");break;}
            h.binding_pending.reset();
            if(!h.mappings.empty()){gap(h.pending,"FinishCommandList: unfinished mappings; storage was not read");h.mappings.clear();}
            h.pending.generation=++h.finishes;h.pending.restore_deferred_state=restore!=FALSE;
            completed=std::make_shared<const DeferredCommandList>(std::move(h.pending));h.pending={};h.pending.context=r->id;h.pending.device=r->device;h.pending.starts_default=restore==FALSE;
            if(restore)gap(h.pending,"Deferred state inherited across FinishCommandList is not yet captured");
        }
        // Successful Finish with null output discards a batch, just as native.
        if(!list)return;
        if(!output || output->kind!="command_list" || output->device!=r->device)throw std::runtime_error("Finished list identity/device mismatch");
        std::lock_guard lock(output->mutex);output->command_list=std::move(completed);output->creation_observed=true;output->creation_api="ID3D11DeviceContext::FinishCommandList";
    }catch(...){object_gap("FinishCommandList",__uuidof(ID3D11CommandList),"Deferred list metadata unavailable");}
}
void remember_command_list_execution(ID3D11DeviceContext* context,ID3D11CommandList* list,BOOL restore) noexcept{
    LastError error;
    try{auto r=object_record(context);auto object=object_record(list);if(!r || !object)throw std::runtime_error("Command list execution identity missing");
        CommandListExecution e;e.list=object->id;e.thread=GetCurrentThreadId();e.immediate=!deferred(*r);e.restore_context_state=restore!=FALSE;
        {std::lock_guard lock(object->mutex);if(object->command_list){e.source_context=object->command_list->context;e.generation=object->command_list->generation;e.metadata_available=true;}}
        std::lock_guard lock(r->mutex);auto& h=history(*r);if(h.execution_serial==UINT64_MAX)throw std::runtime_error("Execution serial overflow");e.context_serial=++h.execution_serial;
        if(h.executions.size()==DeferredHistory::execution_limit)h.executions.pop_front();h.executions.push_back(e);
    }catch(...){object_gap("ExecuteCommandList",__uuidof(ID3D11CommandList),"Command list execution metadata unavailable");}
}
std::string command_list_gap(IUnknown* list){
    const auto r=object_record(list);if(!r)return "CommandList serialization is not implemented (untracked list)";
    std::lock_guard lock(r->mutex);std::string reason="CommandList serialization is not implemented (list="+std::to_string(r->id);
    if(const auto& b=r->command_list){reason+=", source_context="+std::to_string(b->context)+", generation="+std::to_string(b->generation)+", calls="+std::to_string(b->calls.size());if(!b->gaps.empty())reason+=", first_gap="+b->gaps.front();}
    else reason+=", compilation metadata unavailable";
    return reason+")";
}
}
