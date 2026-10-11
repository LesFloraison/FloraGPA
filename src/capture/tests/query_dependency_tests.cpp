#include "query_dependencies.hpp"
#include <cstring>
#include <iostream>
#include <stdexcept>
using namespace flora::capture;
namespace {
void require(bool v,const char* message){if(!v)throw std::runtime_error(message);}
template<class T>std::vector<uint8_t> bytes(const T& d){const auto* p=reinterpret_cast<const uint8_t*>(&d);return {p,p+sizeof d};}
template<class F>void rejects(F f,const char* message){try{f();}catch(const std::exception& e){require(std::string(e.what()).find(message)!=std::string::npos,e.what());return;}throw std::runtime_error("Expected dependency rejection absent");}
FrozenFrame base(){
    FrozenFrame f;
    auto add=[&](ObjectId id,ObjectId device,const char* kind,std::vector<uint8_t> desc){FrozenObject o;o.id=id;o.device=device;o.kind=kind;o.descriptor=std::move(desc);f.objects.emplace(id,std::move(o));};
    add(1,0,"device",bytes(DeviceRecordDesc{D3D_FEATURE_LEVEL_11_0,0}));
    add(2,1,"context",bytes(ContextRecordDesc{D3D11_DEVICE_CONTEXT_IMMEDIATE,0}));
    add(3,1,"query",bytes(D3D11_QUERY_DESC{D3D11_QUERY_EVENT,0}));
    add(4,1,"buffer",bytes(D3D11_BUFFER_DESC{16,D3D11_USAGE_DYNAMIC,D3D11_BIND_VERTEX_BUFFER,D3D11_CPU_ACCESS_WRITE,0,0}));
    add(5,1,"buffer",bytes(D3D11_BUFFER_DESC{16,D3D11_USAGE_STAGING,0,D3D11_CPU_ACCESS_READ,0,0}));
    for(ObjectId id:{4,5}){ResourceContents c;c.shape.dimension=D3D11_RESOURCE_DIMENSION_BUFFER;c.shape.width=16;f.initial_resources.emplace(id,std::move(c));}
    return f;
}
void append(FrozenFrame& f,CommandKind kind,ObjectId operand=0){RecordedCommand c;c.kind=kind;c.sequence=f.commands.size();c.bindings.context=2;c.bindings.device=1;c.operand=operand;c.api="dependency fixture";f.commands.push_back(std::move(c));}
void copy(FrozenFrame& f){append(f,CommandKind::copy_resource,5);f.commands.back().source=4;}
void end(FrozenFrame& f,uint64_t generation=1,ObjectId query=3){append(f,CommandKind::query_end,query);auto& c=f.commands.back();c.completed=true;c.query=QueryObservation{{generation,2,7,true},c.sequence,false,{}};}
void get(FrozenFrame& f,QueryEndStamp stamp,std::optional<uint64_t> sequence,HRESULT hr=S_OK,bool output=false){
    append(f,CommandKind::query_get_data,3);auto& c=f.commands.back();c.completed=true;c.result=hr;c.words={output?4u:0u,1};c.query=QueryObservation{stamp,sequence,output,output && hr==S_OK?std::optional<uint32_t>{TRUE}:std::optional<uint32_t>{}};
}
void intervals(){
    auto f=base();copy(f);end(f);get(f,{1,2,7,true},1,S_FALSE);get(f,{1,2,7,true},1,S_OK,true);
    append(f,CommandKind::draw);get(f,{1,2,7,true},1);end(f,2);get(f,{2,2,7,true},6);
    const auto r=analyze_event_dependencies(f);require(r.ends==2 && r.observations.size()==4,"Query intervals lost");
    require(r.observations[0].scope==EventObservationScope::pending && !r.observations[0].prefix,"Pending poll became a completion");
    const auto& a=r.observations[1];const auto& b=r.observations[2];const auto& c=r.observations[3];
    require(a.prefix && a.prefix==b.prefix && !a.prefix->uncovered_commands,"A later Draw leaked into an earlier End prefix");
    require(a.prefix->copies.size()==1 && a.prefix->copies[0].source==4 && a.prefix->copies[0].destination==5 && a.prefix->copies[0].sequence==0,"Copy dependency identity/order lost");
    require(c.prefix!=a.prefix && c.prefix->uncovered_commands==1 && c.prefix->first_uncovered_sequence==4,"Reused End did not receive its own prefix");
    // Query1 metadata preserves the same analysis contract.
    f.objects[3].kind="query1";f.objects[3].descriptor=bytes(D3D11_QUERY_DESC1{D3D11_QUERY_EVENT,0,D3D11_CONTEXT_TYPE_ALL});
    require(analyze_event_dependencies(f).observations[3].prefix->copies.size()==1,"Query1 dependency lost");
    // A second query's End must not authorize another object's GetData.
    f.objects[6]=f.objects[3];f.objects[6].id=6;f.commands[1].operand=6;
    rejects([&]{analyze_event_dependencies(f);},"GetData End identity");
}
void scopes(){
    auto f=base();get(f,{},{});get(f,{12,2,7,true},{});get(f,{12,9,7,false},{});get(f,{12,9,7,true},{});get(f,{}, {},E_INVALIDARG,true);
    const auto r=analyze_event_dependencies(f);
    const EventObservationScope expected[]{EventObservationScope::unissued,EventObservationScope::preframe,EventObservationScope::deferred,EventObservationScope::foreign_context,EventObservationScope::failed};
    for(size_t i=0;i<std::size(expected);++i)require(r.observations[i].scope==expected[i] && !r.observations[i].prefix,"Unrecorded work acquired a dependency proof");
    auto zero=base();end(zero);get(zero,{1,2,7,true},0);
    require(analyze_event_dependencies(zero).observations[0].prefix!=nullptr,"End at sequence zero treated as missing");
    auto interleaved=base();interleaved.objects[6]=interleaved.objects[3];interleaved.objects[6].id=6;
    copy(interleaved);end(interleaved);copy(interleaved);end(interleaved,1,6);
    get(interleaved,{1,2,7,true},1);get(interleaved,{1,2,7,true},3);interleaved.commands.back().operand=6;
    const auto independent=analyze_event_dependencies(interleaved);
    require(independent.observations[0].prefix->copies.size()==1 && independent.observations[1].prefix->copies.size()==2,"Interleaved queries conflated their copy prefixes");
}
void gaps(){
    for(auto kind:{CommandKind::draw,CommandKind::draw_indexed,CommandKind::draw_instanced,CommandKind::draw_indexed_instanced,CommandKind::dispatch,CommandKind::clear_rtv,CommandKind::clear_dsv,CommandKind::clear_uav_uint,CommandKind::clear_uav_float,CommandKind::map,CommandKind::unmap,CommandKind::unsupported,CommandKind::binding,CommandKind::read}){
        auto f=base();append(f,kind);end(f);get(f,{1,2,7,true},1);
        const auto p=analyze_event_dependencies(f).observations[0].prefix;require(p->uncovered_commands==1 && p->first_uncovered_sequence==0 && p->copies.empty(),"Unknown operation silently covered by query wait");
    }
    for(unsigned mode=0;mode<8;++mode){
        auto f=base();copy(f);end(f);get(f,{1,2,7,true},1);
        if(mode==0)f.commands[0].source=999;
        if(mode==1)f.commands[0].source=5;
        if(mode==2)f.commands[0].bindings.predicate=99;
        if(mode==3)f.objects[5].descriptor=bytes(D3D11_BUFFER_DESC{16,D3D11_USAGE_STAGING,0,D3D11_CPU_ACCESS_READ|D3D11_CPU_ACCESS_WRITE,0,0});
        if(mode==4)f.objects[5].kind="texture2d";
        if(mode==5)f.initial_resources.erase(5);
        if(mode==6)f.initial_resources[5].shape.width=32;
        if(mode==7)f.objects[4].device=99;
        const auto p=analyze_event_dependencies(f).observations[0].prefix;require(p->copies.empty() && p->uncovered_commands==1 && !p->first_uncovered_reason.empty(),"Unsafe copy acquired a read dependency");
    }
}
void malformed(){
    auto good=base();copy(good);end(good);get(good,{1,2,7,true},1,S_OK,true);
    for(unsigned mode=0;mode<15;++mode){
        auto f=good;auto& c=f.commands.back();
        if(mode==0)c.sequence=100;
        if(mode==1)c.bindings.context=99;
        if(mode==2)c.query.reset();
        if(mode==3)c.query->end_sequence=9;
        if(mode==4)c.query->last_end.generation=2;
        if(mode==5)c.query->end_sequence.reset();
        if(mode==6)c.query->value=FALSE;
        if(mode==7)c.result=S_FALSE;
        if(mode==8)c.words[0]=8;
        if(mode==9)c.words[1]=2;
        if(mode==10)c.query->last_end.thread=0;
        if(mode==11)c.completed=false;
        if(mode==12)f.objects.erase(1);
        if(mode==13)f.objects[3].descriptor=bytes(D3D11_QUERY_DESC{D3D11_QUERY_TIMESTAMP,0});
        if(mode==14)f.objects[2].descriptor=bytes(ContextRecordDesc{D3D11_DEVICE_CONTEXT_DEFERRED,0});
        rejects([&]{analyze_event_dependencies(f);},"Event dependency at sequence=");
    }
    auto reused=good;end(reused,3);rejects([&]{analyze_event_dependencies(reused);},"End generation mismatch");
    EventDependencyOptions options;options.entry_limit=0;rejects([&]{analyze_event_dependencies(good,options);},"entry limit");
    options.entry_limit=2;rejects([&]{analyze_event_dependencies(good,options);},"entry limit");
    options={};options.cancelled=[] {return true;};rejects([&]{analyze_event_dependencies(good,options);},"cancelled");
    unsigned checks=0;options.cancelled=[&]{return ++checks>2;};rejects([&]{analyze_event_dependencies(good,options);},"cancelled");
}
}
int main(){try{intervals();scopes();gaps();malformed();std::cout<<"EVENT dependency scopes, copy edges, bounded snapshots and rejection cases passed\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
