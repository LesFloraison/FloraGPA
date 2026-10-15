#include "capture_session.hpp"
#include "command_stream.hpp"
#include "objects.hpp"
#include "swap_chain.hpp"
#include "encoder.hpp"
#include "presentation_resources.hpp"
#include <thread>
#include <optional>
#ifdef FLORA_CAPTURE_FINAL_MSAA_DIAGNOSTICS
#include "../diagnostics/final_msaa.hpp"
#endif

namespace flora::capture {
using Microsoft::WRL::ComPtr;
namespace {
enum class Phase { Armed,Capturing,Finalizing,Completed,Cancelled,Failed };
const char* name(Phase phase){switch(phase){case Phase::Armed:return "Armed";case Phase::Capturing:return "Capturing";case Phase::Finalizing:return "Finalizing";case Phase::Completed:return "Completed";case Phase::Cancelled:return "Cancelled";default:return "Failed";}}
struct Request {
    uint64_t id;
    uint32_t resource_limit_mib;
    std::filesystem::path output;
    std::mutex operation,publication,report;
    std::atomic<bool> cancelled=false,published=false,released=false,writing=false,aborted=false;
    std::atomic<uint64_t> present_ordinal=0;
    Phase phase=Phase::Armed;
    std::string error;
    DWORD owner=0;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Texture2D> buffer;
    std::shared_ptr<FrameRecording> recording;
    std::optional<FrozenFrame> pending;
    DXGI_SWAP_CHAIN_DESC descriptor{};
    explicit Request(uint64_t value,const std::filesystem::path& path,uint32_t limit):id(value),resource_limit_mib(limit),output(path){}
    void state(Phase value){std::lock_guard lock(report);phase=value;}
    Phase state(){std::lock_guard lock(report);return phase;}
    void release(){recording.reset();context.Reset();buffer.Reset();released=true;}
    void fail(const char* reason){std::lock_guard lock(report);error=reason;phase=cancelled?Phase::Cancelled:Phase::Failed;}
    bool terminal(){const auto p=state();return p==Phase::Completed || p==Phase::Cancelled || p==Phase::Failed;}
};
struct Service {std::mutex mutex;std::shared_ptr<Request> request;bool disconnected=false;};
std::atomic<bool> boundary_open=false;
Service& service(){static auto* value=new Service;return *value;}
std::shared_ptr<Request> active(){auto& s=service();std::lock_guard lock(s.mutex);return s.request;}
struct LastErrorScope{DWORD value=GetLastError();~LastErrorScope(){SetLastError(value);}};
void check(HRESULT result,const char* reason){if(FAILED(result))throw std::runtime_error(reason);}
Json progress(const std::shared_ptr<Request>& r){
    if(!r)return {{"state","Tracking"},{"busy",false}};
    std::lock_guard lock(r->report);
    const bool terminal=r->phase==Phase::Completed || r->phase==Phase::Cancelled || r->phase==Phase::Failed;
    Json result={{"state",name(r->phase)},{"swap_chain",r->id},{"busy",!terminal || !r->released.load()},{"cancel_requested",r->cancelled.load()}};
    result["resource_limit_mib"]=r->resource_limit_mib;
    const auto request_name=r->output.filename().u8string();result["request_name"]=std::string(request_name.begin(),request_name.end());
    if(r->published){result["state"]="Completed";const auto path=r->output.u8string();result["file"]=std::string(path.begin(),path.end());}
    if(r->present_ordinal)result["present_ordinal"]=r->present_ordinal.load();
    if(!r->error.empty())result["error"]=r->error;
    return result;
}
void stopped(Request& r,const char* reason){boundary_open=false;r.cancelled=true;if(r.recording)r.recording->cancel();r.release();r.pending.reset();r.fail(reason);}
void start(Request& r,IDXGISwapChain* swap){
    check(swap->GetDesc(&r.descriptor),"Cannot query capture swap-chain descriptor");
    const auto& d=r.descriptor;
    if(!supported_swap_buffers(d.SwapEffect,d.BufferCount) || d.SampleDesc.Count!=1 || d.SampleDesc.Quality!=0)
        throw std::runtime_error("Unsupported swap effect/buffer count or MSAA");
    if(d.BufferDesc.Format!=DXGI_FORMAT_R8G8B8A8_UNORM && d.BufferDesc.Format!=DXGI_FORMAT_B8G8R8A8_UNORM && d.BufferDesc.Format!=DXGI_FORMAT_R10G10B10A2_UNORM)
        throw std::runtime_error("Capture output format is not implemented");
    ComPtr<ID3D11Device> device;check(swap->GetDevice(IID_PPV_ARGS(&device)),"Swap chain has no DX11 device");
    device->GetImmediateContext(&r.context);wrap_object(r.context.GetAddressOf(),"Capture::ImmediateContext");
    check(swap->GetBuffer(0,IID_PPV_ARGS(&r.buffer)),"Cannot get capture backbuffer");wrap_object(r.buffer.GetAddressOf(),"Capture::Backbuffer");
    RecordingOptions options;options.byte_limit=resource_limit_bytes(r.resource_limit_mib);options.cancelled=[&r]{return r.cancelled.load();};options.serialized_context_threads=true;
    r.owner=GetCurrentThreadId();r.recording=begin_recording(r.context.Get(),options);r.state(Phase::Capturing);
}
void finish(Request& r,IDXGISwapChain* swap){
    if(swap){
        ComPtr<IUnknown> current,retained;
        check(swap->GetBuffer(0,IID_PPV_ARGS(&current)),"Cannot identify ending backbuffer");
        check(unwrap(r.buffer.Get())->QueryInterface(IID_PPV_ARGS(&retained)),"Cannot identify retained backbuffer");
        retained=presentation_identity(retained.Get());
        if(current!=retained)throw std::runtime_error("DX11 logical backbuffer identity changed during capture");
    }
    if(r.owner!=GetCurrentThreadId())throw std::runtime_error("Present moved to another application thread");
    boundary_open=true;
    auto frame=r.recording->finish();frame.require_complete();
    SnapshotOptions options;options.byte_limit=resource_limit_bytes(r.resource_limit_mib);options.cancelled=[&r]{return r.cancelled.load();};
    auto final=snapshot_resource(r.context.Get(),r.buffer.Get(),options);
#ifdef FLORA_CAPTURE_FINAL_MSAA_DIAGNOSTICS
    auto diagnostic_path=r.output;diagnostic_path+=L".final-msaa";
    diagnostics::export_final_msaa(r.context.Get(),frame,diagnostic_path,options);
    const auto after=snapshot_resource(r.context.Get(),r.buffer.Get(),options);
    if(final.subresources.size()!=1 || after.subresources.size()!=1 ||
       final.subresources[0].bytes!=after.subresources[0].bytes)
        throw std::runtime_error("Final-MSAA diagnosis changed the capture backbuffer");
#endif
    const auto id=final.record->id;
    r.pending=freeze_frame(std::move(frame),id,std::move(final));
    r.release();r.state(Phase::Finalizing);
}
void publish(std::shared_ptr<Request> r){
    r->writing=true;
    std::thread([r]{
        try{
            EncodeOptions options;options.cancelled=[r]{return r->cancelled.load();};options.publication_mutex=&r->publication;options.published=&r->published;
            options.msaa_byte_limit=resource_limit_bytes(r->resource_limit_mib);
            encode_frame(*r->pending,r->output,options);r->pending.reset();r->state(Phase::Completed);
        }catch(const std::exception& e){r->pending.reset();r->fail(e.what());}
        catch(...){r->pending.reset();r->fail("Background encoder failed");}
    }).detach();
}
}
Json arm_capture(uint64_t id,const std::filesystem::path& output,uint32_t resource_limit_mib){
    if(!resource_limit_mib || resource_limit_mib>max_resource_limit_mib)return {{"ok",false},{"error","invalid_resource_limit"}};
    if(!id || !output.is_absolute() || output.extension()!=L".gpa_frame")return {{"ok",false},{"error","Invalid capture identity or destination"}};
    auto& s=service();std::lock_guard lock(s.mutex);
    if(s.disconnected)return {{"ok",false},{"error","disconnected"}};
    if(s.request && (!s.request->terminal() || !s.request->released))return {{"ok",false},{"error","busy"}};
    if(std::filesystem::exists(output))return {{"ok",false},{"error","Capture destination exists"}};
    const auto observations=swap_chain_status();bool found=false;
    for(const auto& row:observations["active"])if(row["id"]==id && row["successful_presents"].get<uint64_t>()>0)found=true;
    if(!found)return {{"ok",false},{"error","Selected swap chain is missing or has not presented"}};
    s.request=std::make_shared<Request>(id,output,resource_limit_mib);auto result=progress(s.request);result["ok"]=true;return result;
}
Json capture_progress(){return progress(active());}
Json cancel_capture(){
    auto r=active();if(!r)return {{"ok",true},{"cancelled",false}};
    std::lock_guard publication(r->publication);
    if(r->published){auto result=progress(r);result["ok"]=true;result["cancelled"]=false;return result;}
    if(r->terminal()){auto result=progress(r);result["ok"]=true;result["cancelled"]=r->state()==Phase::Cancelled;return result;}
    r->cancelled=true;
    // Never issue GPU calls or release target objects from the control thread.
    // An armed request owns none; a capturing request drains on a target callback.
    std::unique_lock operation(r->operation,std::try_to_lock);
    if(operation.owns_lock() && r->state()==Phase::Armed){r->released=true;r->state(Phase::Cancelled);}
    auto result=progress(r);result["ok"]=true;result["cancelled"]=r->state()==Phase::Cancelled;return result;
}
void disconnect_capture(){auto& s=service();{std::lock_guard lock(s.mutex);s.disconnected=true;}cancel_capture();}
void capture_before_present(uint64_t id,IDXGISwapChain* native,UINT flags,bool partial) noexcept {
    LastErrorScope error;if(flags&DXGI_PRESENT_TEST)return;
    auto r=active();if(!r || r->terminal() || r->writing)return;
    if(r->id!=id){record_external_call("Other swap-chain Present during capture");return;}
    std::unique_lock operation(r->operation,std::try_to_lock);
    if(!operation.owns_lock()){r->aborted=true;return;}
    try{
        if(r->aborted)throw std::runtime_error("Concurrent swap-chain callback is unsupported");
        if(r->cancelled){stopped(*r,"Capture cancelled");return;}
        if(flags&~DXGI_PRESENT_ALLOW_TEARING)throw std::runtime_error("Unsupported Present flags: "+std::to_string(flags));
        if(partial)throw std::runtime_error("Dirty/scroll Present1 capture is not implemented");
        if(r->state()==Phase::Capturing)finish(*r,native);
    }catch(const std::exception& e){boundary_open=false;r->release();r->pending.reset();r->fail(e.what());}
    catch(...){boundary_open=false;r->release();r->pending.reset();r->fail("Present snapshot failed");}
}
void capture_after_present(uint64_t id,IDXGISwapChain* native,UINT interval,UINT flags,bool extended,HRESULT result,uint64_t present_ordinal) noexcept {
    LastErrorScope error;if(flags&DXGI_PRESENT_TEST)return;
    auto r=active();if(!r || r->id!=id || r->terminal() || r->writing)return;
    std::unique_lock operation(r->operation,std::try_to_lock);
    if(!operation.owns_lock()){r->aborted=true;return;}
    try{
        if(r->aborted)throw std::runtime_error("Concurrent swap-chain callback is unsupported");
        if(r->cancelled){stopped(*r,"Capture cancelled");return;}
        boundary_open=false;
        if(result!=S_OK){boundary_open=false;r->release();r->pending.reset();r->fail("Present failed or was occluded");return;}
        if(r->state()==Phase::Armed)start(*r,native);
        else if(r->state()==Phase::Finalizing && r->pending){
            r->present_ordinal=present_ordinal;
            r->pending->presentation=Presentation{r->descriptor,interval,flags,extended};publish(r);
        }
    }catch(const std::exception& e){boundary_open=false;r->release();r->pending.reset();r->fail(e.what());}
    catch(...){boundary_open=false;r->release();r->pending.reset();r->fail("Present capture transition failed");}
}
void capture_context_activity() noexcept {
    if(!boundary_open.load(std::memory_order_relaxed))return;
    try{auto r=active();if(r)r->aborted=true;}catch(...){}
}
void capture_invalidate_swap(uint64_t id,const char* reason) noexcept {
    LastErrorScope error;auto r=active();if(!r || r->id!=id || r->terminal() || r->writing)return;
    std::unique_lock operation(r->operation,std::try_to_lock);
    if(!operation.owns_lock()){r->aborted=true;return;}
    if(r->state()==Phase::Finalizing)return; // CPU-only writer owns no backbuffer.
    boundary_open=false;r->release();r->pending.reset();r->fail(reason);
}
}
