#pragma once
#include "Frame.h"
namespace flora {
struct ReplayCapability {
    const char *handling;
    const char *implementation;
    const char *evidence;
};
ReplayCapability replayCapability(uint16_t type);
} // namespace flora
