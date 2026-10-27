#pragma once
#include "DxbcExecution.h"
namespace flora::dxbc_exec {
// One serialized 32-bit storage transaction. The caller resolves view bounds;
// the scheduler supplies atomicity within the independent execution model.
std::pair<Value, std::vector<MemoryChange>> atomicStorageWord(uint64_t resource, const Buffer &,
                                                              size_t offset, uint32_t opcode, const Value &,
                                                              const Value &comparison);
} // namespace flora::dxbc_exec
