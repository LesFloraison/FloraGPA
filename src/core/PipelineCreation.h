#pragma once
#include "Frame.h"
#include "StreamOutput.h"
namespace flora {
struct PipelineCreationRecord {
    uint16_t type{};
    Id link{}, device{}, resource{}, linkage{};
    int32_t result{};
    bool hasDescriptor{}, hasBytecode{};
    uint64_t bytecodeLength{};
    Bytes descriptor, bytecode;
    StreamOutputDeclaration streamOutput;
    Id streamOutputId{};
    uint32_t strideCount{};
    std::optional<uint32_t> firstStride;
    struct Element {
        Id capturedName{};
        std::array<uint32_t, 6> fields{};
        std::string name;
    };
    std::vector<Element> elements;
    std::string error, note;
};
struct PipelineCreationAudit {
    std::map<Id, PipelineCreationRecord> records;
    std::map<Id, Id> creationEvents;
};
bool isPipelineCreation(uint16_t type);
bool isStateCreation(uint16_t type);
uint16_t pipelineCreatedType(uint16_t type);
PipelineCreationRecord readPipelineCreation(uint16_t type, Bytes bytes);
PipelineCreationAudit auditPipelineCreations(const Frame &frame, const CancelCheck &cancelled = {});
const PipelineCreationRecord &requirePipelineCreation(const PipelineCreationAudit &, Id event);
} // namespace flora
