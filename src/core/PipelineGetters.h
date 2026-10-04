#pragma once
#include "Frame.h"
namespace flora {
struct GetterField {
    std::string name, format;
    size_t offset{};
    bool reference{};
};
struct PipelineGetter {
    Id link{}, context{};
    std::vector<GetterField> fields;
};
bool isPipelineGetter(uint16_t type);
PipelineGetter readPipelineGetter(uint16_t type, Bytes payload);
// Returned IDs are observations, not binding commands or native pointers.
void validatePipelineGetter(const Frame &frame, const PipelineGetter &record);
} // namespace flora
