#pragma once
#include "capture_model.hpp"
#include <functional>
namespace flora::capture {
struct FrameStartOptions {
    uint32_t timeout_ms=5000;
    std::function<bool()> cancelled;
};
// Exclusive application-thread boundary. Uses an unwrapped private EVENT;
// neither the query nor its commands enter the application's object/event graph.
FrameStartCompletion complete_frame_start(ID3D11DeviceContext*,const FrameStartOptions& = {});
}
