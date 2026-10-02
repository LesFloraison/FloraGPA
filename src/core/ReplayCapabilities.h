#pragma once
#include "Frame.h"
namespace flora {
// This is the existing fallback policy, not a promise of validated wire semantics.
bool isReplayAuxiliary(uint16_t type);
struct ReplayCapability {
    const char *handling;
    const char *implementation;
    const char *evidence;
};
ReplayCapability replayCapability(uint16_t type);
} // namespace flora
