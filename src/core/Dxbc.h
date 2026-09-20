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
} // namespace flora
