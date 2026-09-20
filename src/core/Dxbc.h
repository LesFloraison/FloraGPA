#pragma once
#include "Frame.h"
namespace flora {
using DxbcParts = std::vector<std::pair<uint32_t, Bytes>>;
DxbcParts readDxbcParts(Bytes bytes);
std::vector<uint8_t> addEmptyInputSignature(Bytes bytes);
} // namespace flora
