#pragma once
#include "Frame.h"
namespace flora {
using DxbcParts = std::vector<std::pair<uint32_t, Bytes>>;
DxbcParts readDxbcParts(Bytes bytes);
std::vector<uint8_t> makeDxbc(const DxbcParts &parts);
struct DxbcProgram {
    std::array<uint32_t, 2> header;
    std::vector<std::vector<uint32_t>> instructions;
};
DxbcProgram readDxbcProgram(Bytes bytes);
std::vector<uint8_t> writeDxbcProgram(const DxbcProgram &program);
std::vector<uint8_t> addEmptyInputSignature(Bytes bytes);
// SM4/5 declarations survive reflection stripping. Unknown/interface layouts
// conservatively retain every slot; this is not full shader validation.
std::array<bool, 128> shaderSrvDeclarations(Bytes bytes);
// A checked RESINFO mip-count-only use does not consume missing MinLOD state.
// Dimensions/sampling, ambiguous operands and dynamic linkage remain guarded.
std::array<bool, 128> shaderSrvLodDependencies(Bytes bytes);
// False only proves absence of hidden-counter instructions in checked SM4/5 code.
// Unknown versions, opcodes, interfaces and extensions remain conservative.
bool shaderMayUseHiddenCounters(Bytes bytes);
// Per-UAV-register version of the same proof; unknown operands retain all slots.
std::array<bool, 64> shaderHiddenCounterSlots(Bytes bytes);
} // namespace flora
