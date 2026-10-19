#pragma once
#include "Api.h"
#include <nlohmann/json.hpp>
#include <functional>
namespace flora {
nlohmann::json rdcVertexOutputLayout(const ShaderReflection &, uint32_t stride);
nlohmann::json rdcVertexInputSnapshot(const ActionDescription &, const D3D12Pipe::InputAssembly &,
                                     uint32_t vertex, uint32_t instance,
                                     const std::function<bytebuf(ResourceId, uint64_t, uint64_t)> &read);
nlohmann::json rdcExecutedAction(const ActionDescription &action);
nlohmann::json rdcVertexInvocation(const ActionDescription &, const D3D12Pipe::InputAssembly &,
                                  uint32_t vertex, uint32_t instance,
                                  const std::function<bytebuf(ResourceId, uint64_t, uint64_t)> &read);
// Read one instrumented VS position using the returned mesh's remapped indices,
// not the capture's original vertex index. D3D12 post-VS sizes are buffer sizes.
nlohmann::json rdcVertexPositionReference(const ActionDescription &, const MeshFormat &,
                                         uint32_t vertex, uint32_t instance,
                                         const std::function<bytebuf(ResourceId, uint64_t, uint64_t)> &read);
nlohmann::json rdcVertexOutputReference(const ActionDescription &, const MeshFormat &, const ShaderReflection &,
                                       uint32_t vertex, uint32_t instance,
                                       const std::function<bytebuf(ResourceId, uint64_t, uint64_t)> &read);
nlohmann::json rdcComputeInvocation(const ActionDescription &action, const ShaderReflection &shader,
                                    const rdcfixedarray<uint32_t, 3> &group,
                                    const rdcfixedarray<uint32_t, 3> &thread);
} // namespace flora
