#pragma once
#include "Frame.h"
namespace flora {
struct PipelineBinding {
    uint16_t type{};
    Id context{};
    State values;
};
bool isPipelineSetter(uint16_t type);
PipelineBinding readPipelineSetter(uint16_t type, Bytes bytes);
void validatePipelineBinding(const Frame &frame, const PipelineBinding &binding);
void overlayPipelineBinding(State &state, const PipelineBinding &binding);
State pipelineBindingsAt(const Frame &frame, Id event, State state,
                         const std::map<Id, PipelineBinding> &edits);
} // namespace flora
