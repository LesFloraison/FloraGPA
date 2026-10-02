#pragma once
#include "CopyCommands.h"
namespace flora {
// Structure checks only: hardware format support is checked by the replay device.
CopyValidation validateTextureCopy(const Resource &source, const Resource &destination,
                                   const CopyCommand &command);
} // namespace flora
