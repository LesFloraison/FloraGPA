#pragma once
#include "context_state.hpp"
#include <atomic>

namespace flora::capture {
struct RecordedFrame {
    std::optional<FrameStartCompletion> start_completion;
    std::map<ObjectId,RetainedObject> objects;
    std::map<ObjectId,FrozenObject> deferred_definitions;
    std::map<ObjectId,ResourceSnapshot> initial_resources;
    std::map<ObjectId,MsaaSamples> initial_msaa;
    std::vector<RecordedCommand> commands;
    std::vector<std::string> gaps;
    size_t initial_bytes=0;
    bool failed=false,cancelled=false;
    void require_complete() const;
};
struct RecordingOptions {
    size_t byte_limit=default_resource_byte_limit;
    size_t command_limit=100000;
    size_t msaa_plane_limit=1024;
    uint32_t readback_timeout_ms=5000;
    std::function<bool()> cancelled;
};
// Run on the application thread before handing CPU data to a background writer.
// Validates the model and releases every retained native reference on success.
FrozenFrame freeze_frame(RecordedFrame&& frame,ObjectId output,ResourceSnapshot&& final_output);
// Borrowed only during the pre-native observer; no pointer enters RecordedFrame.
struct UpdateSource {const void* data;const D3D11_BOX* box;UINT row_pitch,depth_pitch,flags;bool extended;};
class FrameRecording {
    struct Impl;
    std::unique_ptr<Impl> impl_;
    explicit FrameRecording(ID3D11DeviceContext*,const RecordingOptions&);
    void complete_start();
    friend std::shared_ptr<FrameRecording> begin_recording(ID3D11DeviceContext*,const RecordingOptions&);
public:
    ~FrameRecording();
    void cancel() noexcept;
    RecordedFrame finish();
    void observe(ID3D11DeviceContext*,const char*,CommandKind,const std::array<uint32_t,5>&,
        IUnknown*,const float*,const UINT*,IUnknown* source=nullptr,const UpdateSource* update=nullptr,const CopyRegion* region=nullptr);
    void mapped(ID3D11DeviceContext*,ID3D11Resource*,UINT,HRESULT,const D3D11_MAPPED_SUBRESOURCE*);
    void unmap(ID3D11DeviceContext*,ID3D11Resource*,UINT);
    void query(ID3D11DeviceContext*,ID3D11Asynchronous*,CommandKind,HRESULT,const void*,UINT,UINT);
    void reject_external(const char*);
};
// One immediate execution thread, including supported precompiled command lists.
// Caller owns the frame boundary and must finish on the starting application
// thread. In-frame deferred compilation, other contexts/threads, unsupported GPU
// calls and open queries reject the whole model.
std::shared_ptr<FrameRecording> begin_recording(ID3D11DeviceContext*,const RecordingOptions& options={});
// Called before native execution by generated proxies; always preserves native
// forwarding, including on recording errors. Pointer arguments are transient.
// api must be a static string literal, as emitted by the proxy generator.
void record_context_call(ID3D11DeviceContext*,const char*,CommandKind,
    std::array<uint32_t,5> words={},IUnknown* operand=nullptr,const float* floats=nullptr,const UINT* integers=nullptr,IUnknown* source=nullptr,const UpdateSource* update=nullptr,const CopyRegion* region=nullptr) noexcept;
void record_map_result(ID3D11DeviceContext*,ID3D11Resource*,UINT,HRESULT,const D3D11_MAPPED_SUBRESOURCE*) noexcept;
void record_unmap(ID3D11DeviceContext*,ID3D11Resource*,UINT) noexcept;
// Post-native only. Never read pending/failed output or retain caller pointers.
void record_query_observation(ID3D11DeviceContext*,ID3D11Asynchronous*,CommandKind,
    HRESULT result=S_OK,const void* data=nullptr,UINT size=0,UINT flags=0) noexcept;
void record_external_call(const char* api) noexcept;
}
