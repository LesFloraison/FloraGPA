#include "frame_start.hpp"
#include "objects.hpp"
#include <wrl/client.h>
#include <sstream>
namespace flora::capture {
namespace {
struct LastErrorScope{DWORD value=GetLastError();~LastErrorScope(){SetLastError(value);}};
void check(HRESULT result,const char* reason){if(FAILED(result)){std::ostringstream text;text<<reason<<": 0x"<<std::hex<<uint32_t(result);throw std::runtime_error(text.str());}}
}
FrameStartCompletion complete_frame_start(ID3D11DeviceContext* context,const FrameStartOptions& options){
    LastErrorScope last_error;NativeCall internal;
    auto record=object_record(context);
    if(!record || record->kind!="context")throw std::runtime_error("Frame start requires a tracked context");
    auto* native=unwrap(context);
    if(native->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)throw std::runtime_error("Frame start requires an immediate context");
    Microsoft::WRL::ComPtr<ID3D11Device> device;native->GetDevice(&device);
    Microsoft::WRL::ComPtr<ID3D11Predicate> predicate;BOOL predicate_value=FALSE;native->GetPredication(&predicate,&predicate_value);
    if(predicate)throw std::runtime_error("Frame start with predication is unsupported");
    FrameStartCompletion completed;completed.context=record->id;completed.device=record->device;completed.thread=GetCurrentThreadId();
    {std::lock_guard lock(record->mutex);if(record->has_active_non_timing_query())throw std::runtime_error("Frame start cannot run inside an active query");completed.last_end_serial=record->query_end_serial;}
    const auto start=GetTickCount64();
    auto checkpoint=[&]{
        if(options.cancelled && options.cancelled())throw std::runtime_error("Frame start completion cancelled");
        if(GetTickCount64()-start>=options.timeout_ms)throw std::runtime_error("Frame start completion timed out");
        check(device->GetDeviceRemovedReason(),"Frame start device removed");
    };
    checkpoint();D3D11_QUERY_DESC desc{D3D11_QUERY_EVENT,0};Microsoft::WRL::ComPtr<ID3D11Query> query;
    check(device->CreateQuery(&desc,&query),"Create frame start EVENT");checkpoint();
    native->End(query.Get());native->Flush();
    for(;;){
        checkpoint();BOOL ready=FALSE;
        // Permit runtime progress after an earlier private query was cancelled.
        // One explicit Flush followed by DONOTFLUSH can stall native hardware
        // GetData after early query release; this wait owns its submission.
        const auto result=native->GetData(query.Get(),&ready,sizeof ready,0);
        if(result==S_FALSE){Sleep(1);continue;}
        check(result,"Frame start GetData");
        if(result!=S_OK || ready!=TRUE)throw std::runtime_error("Frame start EVENT did not confirm completion");
        checkpoint();
        {std::lock_guard lock(record->mutex);if(record->query_end_serial!=completed.last_end_serial || record->has_active_non_timing_query())throw std::runtime_error("Query activity changed during frame start completion");}
        completed.result=result;completed.value=uint32_t(ready);completed.elapsed_ms=GetTickCount64()-start;return completed;
    }
}
}
