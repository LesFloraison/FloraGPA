#pragma once
#include "Api.h"
#include <array>
#include <span>
namespace flora {
struct InputAttributeFormat {
    uint32_t bytes{};
    CompType outputType{};
};
// Normalized/float attributes become float32. Integer attributes retain all
// bits in uint32/sint32 storage, including the typed (0,0,0,1) defaults.
InputAttributeFormat inputAttributeFormat(const ResourceFormat &);
std::array<uint32_t, 4> inputAttributeValue(std::span<const uint8_t>, const ResourceFormat &);
uint32_t appendInputAttributeOffset(uint32_t previousEnd, const ResourceFormat &);
}
