#include "deferred_history.hpp"
#include "command_stream.hpp"
#include "objects.hpp"
#include <iostream>
#include <cstring>
#include <thread>
using namespace flora::capture;
using Microsoft::WRL::ComPtr;
namespace {
void require(bool v,const char* why){if(!v)throw std::runtime_error(why);}
void check(HRESULT hr){require(SUCCEEDED(hr),"Native deferred fixture failed");}
std::shared_ptr<const DeferredCommandList> trace(ID3D11CommandList* list){auto r=object_record(list);require(r && r->command_list,"Missing compiled history");return r->command_list;}
void pixels(ID3D11DeviceContext* context,ID3D11Texture2D* texture,UINT value){auto data=snapshot_resource(context,texture);for(size_t at=0;at<data.subresources[0].bytes.size();at+=4){UINT actual=0;memcpy(&actual,data.subresources[0].bytes.data()+at,4);require(actual==value,"Native deferred execution changed pixels");}}
void run(bool hardware){std::shared_ptr<const DeferredCommandList> retained;
    {ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> immediate;
    check(D3D11CreateDevice(nullptr,hardware?D3D_DRIVER_TYPE_HARDWARE:D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&immediate));wrap_object(device.GetAddressOf(),"deferred::Device");wrap_object(immediate.GetAddressOf(),"deferred::Immediate");
    ComPtr<ID3D11DeviceContext> a,b;check(device->CreateDeferredContext(0,&a));check(device->CreateDeferredContext(0,&b));
    D3D11_TEXTURE2D_DESC d{4,4,1,1,DXGI_FORMAT_R8G8B8A8_UNORM,{1,0},D3D11_USAGE_DEFAULT,D3D11_BIND_RENDER_TARGET,0,0};UINT blue[16];std::fill_n(blue,16,0xffff0000u);D3D11_SUBRESOURCE_DATA initial{blue,16,64};ComPtr<ID3D11Texture2D> output;check(device->CreateTexture2D(&d,&initial,&output));ComPtr<ID3D11RenderTargetView> rtv;check(device->CreateRenderTargetView(output.Get(),nullptr,&rtv));
    ComPtr<ID3D11CommandList> first,second;std::exception_ptr error;
    std::thread worker([&]{try{UINT data[24];std::fill_n(data,24,0xff00ff00u);a->UpdateSubresource(output.Get(),0,nullptr,data,24,96);std::fill_n(data,24,0xccccccccu);check(a->FinishCommandList(TRUE,&first));}catch(...){error=std::current_exception();}});worker.join();if(error)std::rethrow_exception(error);
    const float red[]{1,0,0,1};b->ClearRenderTargetView(rtv.Get(),red);check(b->FinishCommandList(FALSE,&second));
    retained=trace(first.Get());const auto other=trace(second.Get());require(retained->calls.size()==1 && other->calls.size()==1 && retained->generation==1 && other->generation==1,"Compile histories mixed contexts");require(retained->recording_thread!=other->recording_thread,"Worker provenance lost");require(retained->restore_deferred_state && retained->starts_default && !other->restore_deferred_state,"Finish state flags lost");
    const auto& c=retained->calls[0];require(c.kind==CommandKind::update_subresource && c.data.size()==64 && c.words[1]==24 && c.words[2]==96,"Update payload was not packed/owned");for(size_t at=0;at<c.data.size();at+=4){UINT v;memcpy(&v,c.data.data()+at,4);require(v==0xff00ff00u,"Update retained caller memory");}
    immediate->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
    immediate->ExecuteCommandList(second.Get(),TRUE);pixels(immediate.Get(),output.Get(),0xff0000ffu);
    D3D11_PRIMITIVE_TOPOLOGY topology{};immediate->IAGetPrimitiveTopology(&topology);require(topology==D3D11_PRIMITIVE_TOPOLOGY_LINELIST,"Execute TRUE failed native state restoration");
    immediate->ExecuteCommandList(first.Get(),TRUE);pixels(immediate.Get(),output.Get(),0xff00ff00u);
    immediate->ExecuteCommandList(first.Get(),FALSE);pixels(immediate.Get(),output.Get(),0xff00ff00u);immediate->IAGetPrimitiveTopology(&topology);require(topology==D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED,"Execute FALSE failed native default state");
    const auto h=object_record(immediate.Get())->deferred_history;require(h->executions.size()==3 && h->executions[0].list==object_record(second.Get())->id && h->executions[1].list==h->executions[2].list,"Execution order sorted by compilation or deduplicated");for(size_t i=0;i<3;++i)require(h->executions[i].context_serial==i+1 && h->executions[i].immediate && h->executions[i].metadata_available,"Execution provenance incomplete");
    ComPtr<ID3D11CommandList> empty;check(a->FinishCommandList(FALSE,&empty));require(trace(empty.Get())->generation==2 && !trace(empty.Get())->starts_default && trace(empty.Get())->gaps.empty(),"Retained deferred state provenance changed");empty.Reset();check(a->FinishCommandList(FALSE,&empty));require(trace(empty.Get())->generation==3 && trace(empty.Get())->starts_default,"Finish FALSE did not reset history state");require(retained->generation==1 && retained->calls.size()==1,"Later Finish mutated previous batch");
    // A synthetic failed native result must not consume the pending journal.
    b->ClearRenderTargetView(rtv.Get(),red);remember_deferred_finish(b.Get(),E_FAIL,TRUE,nullptr);require(object_record(b.Get())->deferred_history->pending.calls.size()==1,"Failed Finish consumed journal");ComPtr<ID3D11CommandList> afterFailure;check(b->FinishCommandList(FALSE,&afterFailure));require(trace(afterFailure.Get())->calls.size()==1,"Failed Finish lost native compile contents");
    // Real null-output semantics are observed, not guessed.
    b->ClearRenderTargetView(rtv.Get(),red);const auto before=object_record(b.Get())->deferred_history->finishes;const auto discarded=b->FinishCommandList(FALSE,nullptr);const auto history=object_record(b.Get())->deferred_history;require(SUCCEEDED(discarded)?history->finishes==before+1 && history->pending.calls.empty():history->finishes==before && !history->pending.calls.empty(),"Null-output Finish bookkeeping differs from native result");
    // Supported precompiled operations expand at their actual execution point.
    auto recording=begin_recording(immediate.Get());immediate->ExecuteCommandList(first.Get(),TRUE);auto frame=recording->finish();frame.require_complete();require(frame.commands.size()==1 && frame.commands[0].deferred && frame.commands[0].deferred->source_context==retained->context,"CommandList execution lost compilation provenance");pixels(immediate.Get(),output.Get(),0xff00ff00u);frame={};
    ComPtr<ID3D11DeviceContext> merged;check(device->CreateDeferredContext(0,&merged));merged->ExecuteCommandList(second.Get(),FALSE);ComPtr<ID3D11CommandList> merge;check(merged->FinishCommandList(FALSE,&merge));require(trace(merge.Get())->calls.size()==1 && !trace(merge.Get())->gaps.empty() && trace(merge.Get())->calls[0].operand==object_record(second.Get())->id,"Nested list was silently flattened");immediate->ExecuteCommandList(merge.Get(),FALSE);pixels(immediate.Get(),output.Get(),0xff0000ffu);
    for(size_t i=0;i<DeferredHistory::execution_limit+4;++i)immediate->ExecuteCommandList(second.Get(),FALSE);
    require(h->executions.size()==DeferredHistory::execution_limit && h->executions.front().context_serial==h->execution_serial-DeferredHistory::execution_limit+1,"Execution history is unbounded or serials were reset");pixels(immediate.Get(),output.Get(),0xff0000ffu);
    ComPtr<ID3D11Device3> device3;check(device.As(&device3));ComPtr<ID3D11DeviceContext3> v3;check(device3->CreateDeferredContext3(0,&v3));v3->ClearRenderTargetView(rtv.Get(),red);ComPtr<ID3D11CommandList> v3list;check(v3->FinishCommandList(FALSE,&v3list));require(trace(v3list.Get())->context==object_record(v3.Get())->id && trace(v3list.Get())->calls.size()==1,"Context3 Finish history missing");
    for(size_t i=0;i<=DeferredHistory::call_limit;++i)b->ClearRenderTargetView(rtv.Get(),red);ComPtr<ID3D11CommandList> limit;check(b->FinishCommandList(FALSE,&limit));require(trace(limit.Get())->calls.size()==DeferredHistory::call_limit && !trace(limit.Get())->gaps.empty(),"Deferred call limit was not explicit");
    {std::lock_guard lock(object_record(b.Get())->mutex);object_record(b.Get())->deferred_history->pending.owned_bytes=DeferredHistory::byte_limit;}
    b->UpdateSubresource(output.Get(),0,nullptr,blue,16,64);ComPtr<ID3D11CommandList> bytes;check(b->FinishCommandList(FALSE,&bytes));require(trace(bytes.Get())->calls[0].data.empty() && !trace(bytes.Get())->gaps.empty(),"Deferred byte budget ignored");
    immediate->ClearState();immediate->Flush();}
    require(object_status().at("active")==0 && metadata_status().at("alive")==0,"CPU deferred model retains native objects");require(retained && retained->calls[0].data.size()==64,"Frozen metadata did not survive native destruction");
}
}
int wmain(int argc,wchar_t** argv){try{run(argc==2 && std::wstring(argv[1])==L"--hardware");std::cout<<"Owned deferred histories, repeated execution order, state flags, limits and native forwarding passed\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
