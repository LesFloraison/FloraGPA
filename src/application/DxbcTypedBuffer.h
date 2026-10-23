#pragma once
#include "DxbcExecution.h"

namespace flora::dxbc_exec {
struct TypedBufferFormat {
    uint32_t bytes, returnType;
    std::array<uint32_t, 4> bits;
};
// Storage layout and shader arithmetic class. Device support is queried separately.
std::optional<TypedBufferFormat> typedBufferFormat(uint32_t format);
} // namespace flora::dxbc_exec
