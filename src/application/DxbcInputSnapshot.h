#pragma once
#include "DxbcExecution.h"
#include "DxbcTextureInputs.h"

namespace flora {
class Replay;
namespace dxbc_exec {
struct InputBinding {
    uint64_t resource = 0, view = 0, offset = 0, size = 0;
    uint32_t stride = 0;
    uint32_t format = 0;
    bool experimentalView = false;
};
struct InputSnapshot {
    uint64_t event = 0, shader = 0, replayGeneration = 0;
    unsigned stage = 0; // Native pipeline order: VS, HS, DS, GS, PS, CS.
    std::string frameSha256;
    uint64_t adapterLuid = 0;
    uint32_t adapterVendor = 0, adapterDevice = 0;
    std::vector<uint8_t> bytecode;
    Inputs inputs;
    // Keys are binding registers: b/t/u plus slot, without element indices.
    std::map<Register, InputBinding> bindings;
    std::map<Register, std::string> unavailable;
    std::map<uint64_t, std::string> unavailableCounters;
    static constexpr const char *source = "independent-replay-resource-inputs/6";
};
// Runs a before-event replay and copies the selected stage's actual bindings
// inside its experiment scope. Resource bytes are observations of this replay,
// not proof of original-capture initialization or defined shader behavior.
InputSnapshot snapshotBufferInputs(Replay &, unsigned stage, uint64_t maxBytes = 256 * 1024 * 1024,
                                   const CancelCheck &cancelled = {});
InputSnapshot snapshotResourceInputs(Replay &, unsigned stage, uint64_t maxBytes = 256 * 1024 * 1024,
                                     const CancelCheck &cancelled = {});
} // namespace dxbc_exec
} // namespace flora
