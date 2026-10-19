#pragma once
#include "Api.h"
#include <nlohmann/json.hpp>
namespace flora {
nlohmann::json rdcExecutedAction(const ActionDescription &action);
nlohmann::json rdcComputeInvocation(const ActionDescription &action, const ShaderReflection &shader,
                                    const rdcfixedarray<uint32_t, 3> &group,
                                    const rdcfixedarray<uint32_t, 3> &thread);
} // namespace flora
