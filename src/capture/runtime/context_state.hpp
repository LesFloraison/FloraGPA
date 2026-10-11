#pragma once
#include "metadata.hpp"
#include <wrl/client.h>
#include <array>
#include <map>

namespace flora::capture {
struct RetainedObject {
    std::shared_ptr<ObjectRecord> record;
    Microsoft::WRL::ComPtr<IUnknown> native;
};
struct ContextSnapshot {
    PipelineBindings bindings;
    // Owns canonical native references only for this capture snapshot lifetime.
    // Persistent ObjectRecord itself never owns native objects.
    std::map<ObjectId,RetainedObject> objects;
    std::vector<std::string> gaps;
    void require_supported_bindings() const;
};
// Caller must exclude concurrent context mutation. This only samples bound
// pipeline state: not active queries, command lists, resource bytes or counters.
// Never call from the IPC thread. Does not apply state to the application.
ContextSnapshot snapshot_context(ID3D11DeviceContext* application_context);
}
