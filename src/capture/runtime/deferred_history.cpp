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
}
void observe_deferred_call(ID3D11DeviceContext* context,const char* api,CommandKind kind,const std::array<uint32_t,5>& words,IUnknown* operand,const float* floats,const UINT* integers,IUnknown* source,const UpdateSource* update,const CopyRegion* region) noexcept{
    LastError error;std::shared_ptr<ObjectRecord> r;
    try{
        r=object_record(context);if(!r || !deferred(*r))return;
        const std::string_view name=api;
        if(name.ends_with("::FinishCommandList") || (kind==CommandKind::read && !name.ends_with("::End")))return;
        std::lock_guard lock(r->mutex);auto& h=history(*r);auto& b=h.pending;
        if(native_call_depth>1){gap(b,std::string(api)+": reentrant deferred call");return;}
        if(!b.recording_thread)b.recording_thread=GetCurrentThreadId();
        else if(b.recording_thread!=GetCurrentThreadId())gap(b,std::string(api)+": recording thread changed; synchronization unproven");
        if(b.calls.size()>=DeferredHistory::call_limit){gap(b,std::string(api)+": deferred call limit exceeded");return;}
        DeferredCall c;c.api=api;c.kind=kind;c.sequence=b.calls.size();c.words=words;c.operand=identity(operand);c.source=identity(source);
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
            }else if(kind!=CommandKind::clear_rtv && kind!=CommandKind::clear_dsv && kind!=CommandKind::clear_uav_uint && kind!=CommandKind::clear_uav_float && kind!=CommandKind::copy_resource && kind!=CommandKind::copy_region)
                throw std::runtime_error("Deferred state, mapping, query or command arguments require further tracking");
        }catch(const std::exception& e){gap(b,std::string(api)+": "+e.what());}
        b.calls.push_back(std::move(c));
    }catch(...){object_gap(api,__uuidof(ID3D11DeviceContext),"Deferred call metadata unavailable");}
}
void remember_deferred_finish(ID3D11DeviceContext* context,HRESULT result,BOOL restore,ID3D11CommandList* list) noexcept{
    LastError error;
    try{
        auto r=object_record(context);if(!r || !deferred(*r))return;
        auto output=list?object_record(list):nullptr;std::shared_ptr<const DeferredCommandList> completed;
        {std::lock_guard lock(r->mutex);auto& h=history(*r);if(FAILED(result)){++h.failed_finishes;return;}
            if(h.finishes==UINT64_MAX)throw std::runtime_error("Deferred generation overflow");
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
