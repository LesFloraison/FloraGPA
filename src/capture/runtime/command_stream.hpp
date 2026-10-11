#pragma once
#include "context_state.hpp"
#include <atomic>

namespace flora::capture {
struct RecordedFrame {
    std::map<ObjectId,RetainedObject> objects;
    std::map<ObjectId,ResourceSnapshot> initial_resources;
    std::vector<RecordedCommand> commands;
    std::vector<std::string> gaps;
    size_t initial_bytes=0;
    bool failed=false,cancelled=false;
    void require_complete() const;
};
struct RecordingOptions {
    size_t byte_limit=default_resource_byte_limit;
    size_t command_limit=100000;
    uint32_t readback_timeout_ms=5000;
};
// Run on the application thread before handing CPU data to a background writer.
// Validates the model and releases every retained native reference on success.
FrozenFrame freeze_frame(RecordedFrame&& frame,ObjectId output,ResourceSnapshot&& final_output);
class FrameRecording {
    struct Impl;
    std::unique_ptr<Impl> impl_;
    explicit FrameRecording(ID3D11DeviceContext*,const RecordingOptions&);
    friend std::shared_ptr<FrameRecording> begin_recording(ID3D11DeviceContext*,const RecordingOptions&);
public:
    ~FrameRecording();
    void cancel() noexcept;
    RecordedFrame finish();
    void observe(ID3D11DeviceContext*,const char*,CommandKind,const std::array<uint32_t,5>&,
        IUnknown*,const float*,const UINT*);
    void reject_external(const char*);
};
// Temporary single-thread/immediate-context recorder. Caller owns the frame
// boundary and must finish on the starting application thread. Any other
// context/thread, unsupported GPU call or open query rejects the whole model.
std::shared_ptr<FrameRecording> begin_recording(ID3D11DeviceContext*,const RecordingOptions& options={});
// Called before native execution by generated proxies; always preserves native
// forwarding, including on recording errors. Pointer arguments are transient.
// api must be a static string literal, as emitted by the proxy generator.
void record_context_call(ID3D11DeviceContext*,const char*,CommandKind,
    std::array<uint32_t,5> words={},IUnknown* operand=nullptr,const float* floats=nullptr,const UINT* integers=nullptr) noexcept;
void record_external_call(const char* api) noexcept;
}
