#pragma once
#include <nlohmann/json.hpp>

namespace flora {
// The pinned original ERG initializer's references, not GPU resource usage.
// Unknown families and invalid records remain explicit; this does not schedule
// initialization or resolve Command List execution identities.
nlohmann::json inspectInitializationReferences(const nlohmann::json &command);
} // namespace flora
