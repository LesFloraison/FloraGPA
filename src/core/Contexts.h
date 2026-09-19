#pragma once
#include "Frame.h"
#include <optional>

namespace flora {
struct ContextEvidence {
    Id event{}, resource{}, device{}, data{}, unmap{};
    uint16_t wireType{};
    uint32_t subresource{}, mapType{};
    int32_t hresult{};
};
struct ContextDescription {
    Id id{}, device{};
    bool deferred{};
    std::optional<unsigned> version;
    std::optional<uint32_t> flags;
    std::optional<Id> pointer;
    std::vector<ContextEvidence> evidence;
};
struct ContextIssue {
    Id context{};
    std::optional<Id> event;
    std::string reason;
};
struct ContextRecovery {
    std::vector<ContextDescription> contexts;
    std::vector<ContextIssue> issues;
};
std::optional<unsigned> contextVersion(uint16_t type);
ContextDescription describeContext(const Frame &frame, Id id, bool allowRecovery = true);
void requireImmediateContext(const Frame &frame, Id id);

struct FinishCommandList {
    Id link{}, context{}, reference{};
    unsigned version{};
    int32_t hresult{};
    uint32_t restore{};
};
std::optional<unsigned> finishCommandListVersion(uint16_t type);
FinishCommandList readFinishCommandList(uint16_t type, Bytes bytes);
void acceptFinishCommandList(const Frame &frame, const FinishCommandList &command);
} // namespace flora
