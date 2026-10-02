#pragma once
#include "Frame.h"
namespace flora {
struct TextureCreationRecord {
    uint16_t type{};
    Id link{}, device{}, resource{}, source{}, data{};
    int32_t result{};
    bool hasDescriptor{}, hasInitial{};
    std::vector<uint32_t> descriptor;
    struct Initial {
        Id pointer{};
        uint32_t rowPitch{}, slicePitch{};
    };
    std::vector<Initial> initial;
    std::string error;
};
struct TextureCreationAudit {
    std::map<Id, TextureCreationRecord> records;
    std::map<Id, Id> creationEvents;
};
bool isTextureCreation(uint16_t type);
TextureCreationRecord readTextureCreation(uint16_t type, Bytes payload);
TextureCreationAudit auditTextureCreations(const Frame &frame);
const TextureCreationRecord &requireTextureCreation(const TextureCreationAudit &, Id event);
bool isTextureCreationObservation(uint16_t type);
bool acceptTextureCreationObservation(uint16_t type, Bytes payload);
bool textureSrvDescriptorEqual(std::span<const uint32_t> a, std::span<const uint32_t> b);
} // namespace flora
