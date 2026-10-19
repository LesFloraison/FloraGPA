#pragma once
#include "Api.h"
#include <nlohmann/json.hpp>
#include <functional>
namespace flora {
nlohmann::json rdcExecutedAction(const ActionDescription &action);
nlohmann::json rdcVertexInvocation(const ActionDescription &, const D3D12Pipe::InputAssembly &,
                                  uint32_t vertex, uint32_t instance,
                                  const std::function<bytebuf(ResourceId, uint64_t, uint64_t)> &read);
nlohmann::json rdcComputeInvocation(const ActionDescription &action, const ShaderReflection &shader,
                                    const rdcfixedarray<uint32_t, 3> &group,
                                    const rdcfixedarray<uint32_t, 3> &thread);
} // namespace flora
