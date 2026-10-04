#pragma once
#include "Frame.h"
namespace flora {
struct ContextStateRecord {
    uint16_t type{};
    Id link{}, owner{}, state{}, previous{};
    int32_t result{};
    uint32_t flags{}, levelCount{}, sdk{};
    bool hasLevels{};
    std::vector<uint32_t> levels;
    std::array<uint8_t, 16> emulatedInterface{};
    std::optional<uint32_t> chosenLevel;
};
bool isContextStateRecord(uint16_t type);
ContextStateRecord readContextStateRecord(uint16_t type, Bytes payload);
void validateContextStateOwner(const Frame &frame, Id event, const ContextStateRecord &record);
// Empty for read-only GetCreationFlags. State creation/swap remain unsupported.
std::string contextStateReplayGap(const ContextStateRecord &record);
const char *contextStateGapKind(const ContextStateRecord &record);
} // namespace flora
