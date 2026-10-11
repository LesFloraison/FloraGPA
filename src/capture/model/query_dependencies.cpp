#include "query_dependencies.hpp"
#include "resource_layout.hpp"
#include <cstring>
#include <stdexcept>
namespace flora::capture {
namespace {
bool same(const QueryEndStamp& a,const QueryEndStamp& b){return a.generation==b.generation && a.context==b.context && a.thread==b.thread && a.immediate==b.immediate && a.context_serial==b.context_serial;}
template<class T>T descriptor(const FrozenObject& o){
    if(o.descriptor.size()!=sizeof(T))throw std::runtime_error("Descriptor length mismatch");
    T value;memcpy(&value,o.descriptor.data(),sizeof value);return value;
}
const FrozenObject& object(const FrozenFrame& frame,ObjectId id,ObjectId device){
    const auto i=frame.objects.find(id);
    if(!id || i==frame.objects.end() || i->second.id!=id || i->second.device!=device)
        throw std::runtime_error("Object identity/device mismatch");
    return i->second;
}
void event_query(const FrozenFrame& frame,ObjectId id,ObjectId device){
    const auto& o=object(frame,id,device);
    if(o.kind=="query"){
        const auto d=descriptor<D3D11_QUERY_DESC>(o);
        if(d.Query==D3D11_QUERY_EVENT && !d.MiscFlags)return;
    }else if(o.kind=="query1"){
        const auto d=descriptor<D3D11_QUERY_DESC1>(o);
        if(d.Query==D3D11_QUERY_EVENT && !d.MiscFlags && d.ContextType==D3D11_CONTEXT_TYPE_ALL)return;
    }
    throw std::runtime_error("Only EVENT/ALL queries with zero miscellaneous flags are analyzed");
}
void readable_copy(const FrozenFrame& frame,const RecordedCommand& command){
    const auto& source=object(frame,command.source,command.bindings.device);
    const auto& destination=object(frame,command.operand,command.bindings.device);
    if(source.kind!="buffer" || destination.kind!="buffer" || command.source==command.operand)
        throw std::runtime_error("Copy dependency requires distinct buffers");
    const auto s=descriptor<D3D11_BUFFER_DESC>(source),d=descriptor<D3D11_BUFFER_DESC>(destination);
    if(d.Usage!=D3D11_USAGE_STAGING || d.CPUAccessFlags!=D3D11_CPU_ACCESS_READ || d.BindFlags || d.MiscFlags || d.StructureByteStride)
        throw std::runtime_error("Copy destination is not a read-only staging buffer");
    if(!s.ByteWidth || s.ByteWidth!=d.ByteWidth)throw std::runtime_error("Copy buffer widths differ");
    const auto si=frame.initial_resources.find(source.id),di=frame.initial_resources.find(destination.id);
    if(si==frame.initial_resources.end() || di==frame.initial_resources.end())throw std::runtime_error("Copy initial resource contents missing");
    if(si->second.shape.dimension!=D3D11_RESOURCE_DIMENSION_BUFFER || si->second.shape.width!=s.ByteWidth ||
       di->second.shape.dimension!=D3D11_RESOURCE_DIMENSION_BUFFER || di->second.shape.width!=d.ByteWidth)
        throw std::runtime_error("Copy descriptor/shape mismatch");
    validate_copy_resource(di->second.shape,destination.descriptor,si->second.shape);
    if(command.bindings.predicate)throw std::runtime_error("Predicated copy dependency is unsupported");
}
}
EventDependencyReport analyze_event_dependencies(const FrozenFrame& frame,const EventDependencyOptions& options){
    if(options.cancelled && options.cancelled())throw std::runtime_error("Event dependency analysis cancelled");
    EventDependencyReport report;EventDependencyPrefix prefix;
    struct End {QueryEndStamp stamp;uint64_t sequence;std::shared_ptr<const EventDependencyPrefix> prefix;};
    std::map<ObjectId,End> ends;
    size_t retained=0;ObjectId context=0,device=0;
    const auto* start=frame.start_completion?&*frame.start_completion:nullptr;
    if(start){
        const auto& ctx=object(frame,start->context,start->device);
        const auto dev=frame.objects.find(start->device);
        if(!start->thread || start->result!=S_OK || start->value!=1u || ctx.kind!="context" ||
           descriptor<ContextRecordDesc>(ctx).type!=D3D11_DEVICE_CONTEXT_IMMEDIATE ||
           dev==frame.objects.end() || dev->second.id!=start->device || dev->second.kind!="device")
            throw std::runtime_error("Invalid frame start completion");
    }
    uint64_t end_serial=start?start->last_end_serial:0;
    auto charge=[&](size_t n){if(n>options.entry_limit-retained)throw std::runtime_error("Event dependency entry limit exceeded");retained+=n;};
    auto gap=[&](const RecordedCommand& c,const std::string& reason){
        ++prefix.uncovered_commands;
        if(!prefix.first_uncovered_sequence){prefix.first_uncovered_sequence=c.sequence;prefix.first_uncovered_reason=c.api+": "+reason;}
    };
    for(size_t at=0;at<frame.commands.size();++at){
        if(options.cancelled && options.cancelled())throw std::runtime_error("Event dependency analysis cancelled");
        const auto& c=frame.commands[at];
        try{
            if(c.sequence!=at)throw std::runtime_error("Command sequence mismatch");
            if(!context){context=c.bindings.context;device=c.bindings.device;}
            if(!context || !device || c.bindings.context!=context || c.bindings.device!=device)
                throw std::runtime_error("Multiple or missing context/device identities");
            if(start && (context!=start->context || device!=start->device))throw std::runtime_error("Frame start context/device mismatch");
            const auto dev=frame.objects.find(device);
            if(dev==frame.objects.end() || dev->second.id!=device || dev->second.kind!="device")
                throw std::runtime_error("Device identity missing");
            const auto& ctx=object(frame,context,device);
            if(ctx.kind!="context" || descriptor<ContextRecordDesc>(ctx).type!=D3D11_DEVICE_CONTEXT_IMMEDIATE)
                throw std::runtime_error("Immediate context required");
            if(c.kind==CommandKind::query_end || c.kind==CommandKind::query_get_data){
                event_query(frame,c.operand,device);
                if(!c.completed || !c.query)throw std::runtime_error("Query observation missing");
                const auto& q=*c.query;const auto& stamp=q.last_end;
                if((!stamp.generation && (stamp.context || stamp.thread || stamp.immediate || stamp.context_serial || q.end_sequence)) ||
                   (stamp.generation && (!stamp.context || !stamp.thread)))throw std::runtime_error("Incomplete End history");
                if(c.kind==CommandKind::query_end){
                    if(!stamp.generation || !stamp.thread || !stamp.immediate || stamp.context!=context || q.end_sequence!=c.sequence || q.has_output || q.value)
                        throw std::runtime_error("End provenance mismatch");
                    if(auto previous=ends.find(c.operand);previous!=ends.end() && stamp.generation!=previous->second.stamp.generation+1)
                        throw std::runtime_error("End generation mismatch");
                    if(start && (stamp.thread!=start->thread || stamp.context_serial<=end_serial))throw std::runtime_error("End frame start order mismatch");
                    end_serial=stamp.context_serial;
                    charge(1);charge(prefix.copies.size());
                    ends[c.operand]={stamp,c.sequence,std::make_shared<const EventDependencyPrefix>(prefix)};
                    ++report.ends;continue;
                }
                if(c.words[1]&~D3D11_ASYNC_GETDATA_DONOTFLUSH)throw std::runtime_error("Invalid GetData flags");
                if(c.result>=0 && c.result!=S_OK && c.result!=S_FALSE)throw std::runtime_error("Invalid GetData result");
                if(c.result!=S_OK && q.value)throw std::runtime_error("Unavailable GetData result bytes retained");
                if(c.result==S_OK && ((q.has_output && (c.words[0]!=sizeof(BOOL) || q.value!=1u)) || (!q.has_output && (c.words[0] || q.value))))
                    throw std::runtime_error("Invalid completed EVENT output");
                EventDependencyObservation observation;observation.query=c.operand;observation.sequence=c.sequence;observation.generation=stamp.generation;observation.end_sequence=q.end_sequence;
                const auto end=ends.find(c.operand);
                if(q.end_sequence){
                    if(end==ends.end() || end->second.sequence!=*q.end_sequence || !same(stamp,end->second.stamp) || *q.end_sequence>=c.sequence)
                        throw std::runtime_error("GetData End identity/generation/sequence mismatch");
                }else if(end!=ends.end())throw std::runtime_error("GetData lost its in-frame End association");
                const bool completed_before_frame=start && stamp.generation && stamp.immediate && stamp.context==context &&
                    stamp.context_serial && stamp.context_serial<=start->last_end_serial && !q.end_sequence;
                if(completed_before_frame && c.result==S_FALSE)throw std::runtime_error("Pending EVENT contradicts frame start completion");
                if(c.result<0)observation.scope=EventObservationScope::failed;
                else if(c.result==S_FALSE)observation.scope=EventObservationScope::pending;
                else if(!stamp.generation)observation.scope=EventObservationScope::unissued;
                else if(!stamp.immediate)observation.scope=EventObservationScope::deferred;
                else if(stamp.context!=context)observation.scope=EventObservationScope::foreign_context;
                else if(!q.end_sequence)observation.scope=completed_before_frame?EventObservationScope::preframe_completed:EventObservationScope::preframe;
                else{observation.scope=EventObservationScope::recorded_prefix;observation.prefix=end->second.prefix;}
                charge(1);report.observations.push_back(std::move(observation));continue;
            }
            if(c.query)throw std::runtime_error("Query provenance attached to a non-query command");
            if(c.kind==CommandKind::copy_resource){
                try{readable_copy(frame,c);}
                catch(const std::exception& e){gap(c,e.what());continue;}
                charge(1);prefix.copies.push_back({c.sequence,c.source,c.operand});
            }else gap(c,(c.kind==CommandKind::map || c.kind==CommandKind::unmap)?
                "CPU mapping hazards require separate analysis":"GPU completion dependency has no verified lowering");
        }catch(const std::exception& e){throw std::runtime_error("Event dependency at sequence="+std::to_string(c.sequence)+", object="+std::to_string(c.operand)+": "+e.what());}
    }
    return report;
}
}
