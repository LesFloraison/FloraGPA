#include "PipelineCreation.h"
#include "ClassLinkage.h"
#include "Dxbc.h"
#include <algorithm>
namespace flora {
bool isStateCreation(uint16_t t) { return t >= 0x3589 && t <= 0x358c; }
bool isPipelineCreation(uint16_t t) {
    return t == 0x3580 || t == 0x3581 || t == 0x3584 || t == 0x3587 || isStateCreation(t);
}
uint16_t pipelineCreatedType(uint16_t t) {
    switch (t) {
    case 0x3580:
        return 0x82;
    case 0x3581:
        return 0x90;
    case 0x3584:
        return 0x92;
    case 0x3587:
        return 0x93;
    case 0x3589:
        return 0x8a;
    case 0x358a:
        return 0x8b;
    case 0x358b:
        return 0x89;
    case 0x358c:
        return 0x88;
    default:
        throw std::runtime_error("Unrecovered pipeline creation");
    }
}
PipelineCreationRecord readPipelineCreation(uint16_t t, Bytes bytes) {
    pipelineCreatedType(t);
    Reader r(bytes);
    PipelineCreationRecord c;
    c.type = t;
    c.link = r.read<Id>();
    c.device = r.read<Id>();
    c.result = r.read<int32_t>();
    if (isStateCreation(t)) {
        c.hasDescriptor = r.flag();
        if (c.hasDescriptor)
            c.descriptor = r.take(t == 0x3589 ? 264 : t == 0x358b ? 40 : 52);
    } else {
        if (t == 0x3580) {
            const auto count = r.read<uint32_t>();
            c.hasDescriptor = r.flag();
            if (count > 32)
                throw std::runtime_error("Input layout count exceeds 32");
            if (c.hasDescriptor)
                for (uint32_t i = 0; i < count; ++i)
                    c.elements.push_back({r.read<Id>(), r.array<uint32_t, 6>(), {}});
            else if (count && !c.result)
                throw std::runtime_error("Successful layout has no element observations");
        }
        c.bytecodeLength = r.read<uint64_t>();
        c.hasBytecode = r.flag();
        if (c.hasBytecode)
            c.bytecode = r.take(size_t(c.bytecodeLength));
        if (t != 0x3580)
            c.linkage = r.read<Id>();
    }
    c.resource = r.read<Id>();
    r.end();
    return c;
}
PipelineCreationAudit auditPipelineCreations(const Frame &frame) {
    PipelineCreationAudit a;
    for (const auto &[id, e] : frame.entries())
        if (e.category == 7 && isPipelineCreation(e.type)) {
            auto &c = a.records[id];
            try {
                c = readPipelineCreation(e.type, frame.payload(id));
                if (c.link)
                    throw std::runtime_error("Linked pipeline creation is unresolved");
                if (c.result != 0) {
                    if (c.result > 1 || c.resource)
                        throw std::runtime_error("Unexpected pipeline creation result or identity");
                    continue;
                }
                if (!c.resource)
                    throw std::runtime_error("Successful pipeline creation has no identity");
                frame.payload(c.device, 5, 0x81);
                if (isStateCreation(c.type)) {
                    if (!c.hasDescriptor)
                        throw std::runtime_error("State descriptor is absent");
                } else {
                    if (!c.hasBytecode || c.bytecode.empty())
                        throw std::runtime_error("Shader/signature bytes are absent");
                    readDxbcParts(c.bytecode);
                    if (c.linkage)
                        frame.payload(c.linkage, 5, 0x97);
                }
                const auto [it, inserted] = a.creationEvents.emplace(c.resource, id);
                if (!inserted) {
                    const auto &previous = a.records.at(it->second);
                    if (previous.device != c.device || previous.type != c.type ||
                        previous.linkage != c.linkage ||
                        (!isStateCreation(c.type) &&
                         !std::equal(previous.bytecode.begin(), previous.bytecode.end(), c.bytecode.begin(),
                                     c.bytecode.end())))
                        throw std::runtime_error("Conflicting immutable pipeline creation identity");
                }
                if (!frame.entries().contains(c.resource)) {
                    if (c.type == 0x3580)
                        c.note = "Input layout semantic strings are absent; retain this unmaterialized "
                                 "creation as metadata and reject any attempt to use its identity";
                    continue;
                }
                const auto resource = frame.resource(c.resource);
                auto expected = pipelineCreatedType(c.type);
                bool kind = resource.type == expected || (expected == 0x8a && resource.type == 0x10d) ||
                            (expected == 0x89 && (resource.type == 0x10e || resource.type == 0x10f));
                if (!kind || resource.device != c.device)
                    throw std::runtime_error(
                        "Created pipeline resource kind/device differs from its snapshot");
                if (c.type == 0x3580) {
                    Reader r(frame.capturedPayload(c.resource, 5, 0x82));
                    r.skip(16);
                    Reader layout(frame.payload(r.read<Id>(), 9, 0x84));
                    r.end();
                    const auto count = layout.read<uint32_t>();
                    if (count != c.elements.size())
                        throw std::runtime_error("Input element count differs from saved layout");
                    for (auto &element : c.elements) {
                        auto name = frame.data(layout.read<Id>());
                        if (!element.capturedName || name.empty() || name.back() != 0 ||
                            std::find(name.begin(), name.end() - 1, uint8_t(0)) != name.end() - 1)
                            throw std::runtime_error("Input semantic string is absent or malformed");
                        element.name.assign(reinterpret_cast<const char *>(name.data()), name.size() - 1);
                        if (element.fields != layout.array<uint32_t, 6>())
                            throw std::runtime_error("Input element fields differ from saved layout");
                    }
                    auto code = layout.take(layout.read<uint32_t>());
                    layout.end();
                    if (!std::equal(code.begin(), code.end(), c.bytecode.begin(), c.bytecode.end()))
                        throw std::runtime_error("Input signature differs from creation bytecode");
                } else if (!isStateCreation(c.type)) {
                    const auto code = frame.shader(resource.data);
                    if (!std::equal(code.begin(), code.end(), c.bytecode.begin(), c.bytecode.end()) ||
                        shaderClassLinkage(frame, c.resource) != c.linkage)
                        throw std::runtime_error("Created shader bytecode/linkage differs from saved shader");
                    Reader r(frame.capturedPayload(c.resource));
                    r.skip(40);
                    if (r.read<Id>())
                        throw std::runtime_error(
                            "Ordinary shader creation has unexpected stream-output data");
                } else {
                    auto bytes = frame.capturedPayload(c.resource).subspan(16);
                    const auto size = resource.type == 0x10d   ? 328
                                      : resource.type == 0x10e ? 44
                                      : resource.type == 0x10f ? 48
                                                               : c.descriptor.size();
                    if (bytes.size() != size)
                        throw std::runtime_error("Saved pipeline state descriptor length differs");
                }
            } catch (const std::exception &error) {
                c.error = error.what();
            }
        }
    for (auto &[event, creation] : a.records)
        if (!creation.note.empty() && creation.error.empty()) {
            for (const auto &[id, entry] : frame.entries())
                if (entry.category == 3 && entry.type == 3) {
                    try {
                        if (frame.state(id).layout == creation.resource) {
                            creation.error =
                                "Input semantic strings are absent for a layout required by state snapshot " +
                                std::to_string(id);
                            break;
                        }
                    } catch (const std::exception &error) {
                        creation.error =
                            "Cannot prove unmaterialized layout is unused: " + std::string(error.what());
                        break;
                    }
                }
        }
    return a;
}
const PipelineCreationRecord &requirePipelineCreation(const PipelineCreationAudit &a, Id event) {
    const auto &c = a.records.at(event);
    if (!c.error.empty())
        throw std::runtime_error("Pipeline creation event " + std::to_string(event) + ", resource " +
                                 std::to_string(c.resource) + ": " + c.error);
    return c;
}
} // namespace flora
