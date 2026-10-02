#pragma once
#include "Frame.h"
namespace flora {
struct CopyCommand {
    uint16_t type{};
    Id context{}, destination{}, source{};
    uint32_t destinationSubresource{}, sourceSubresource{}, x{}, y{}, z{};
    std::optional<std::array<uint32_t, 6>> box;
};
enum class CopyValidation { Buffer, TextureReferences };
bool isCopyCommand(uint16_t type);
CopyCommand readCopyCommand(uint16_t type, Bytes payload);
CopyValidation validateCopyCommand(const Frame &frame, const CopyCommand &copy);
} // namespace flora
