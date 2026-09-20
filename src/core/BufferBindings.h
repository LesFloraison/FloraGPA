#pragma once
#include "Frame.h"

namespace flora {
struct BufferBinding {
    std::string role, stage;
    int slot = -1;
    Id view = 0;
    uint32_t offset = 0;
};
std::vector<BufferBinding> bufferBindings(const Frame &frame, const Event &event, const State &state,
                                          Id resource);
bool persistentBufferEdit(const std::vector<BufferBinding> &bindings);
void validateBufferPatch(const Frame &frame, Id event, Id resource, uint64_t offset, size_t size,
                         const State *effective = nullptr);
} // namespace flora
