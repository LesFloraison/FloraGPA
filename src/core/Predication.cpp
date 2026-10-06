#include "Predication.h"
namespace flora {
PredicateDescriptor readPredicate(const Frame &frame, Id id) {
    if (!frame.entries().contains(id))
        throw std::runtime_error("Predicate resource " + std::to_string(id) +
                                 " is absent from the capture; its descriptor cannot be reconstructed");
    Reader r(frame.payload(id, 5, 0x96));
    PredicateDescriptor out{id, r.read<Id>(), r.read<Id>(), r.read<uint32_t>(), r.read<uint32_t>()};
    r.end();
    if ((out.type != 5 && out.type != 7) || (out.flags & ~1u) || (out.flags && out.type != 5))
        throw std::runtime_error("Unsupported predicate query type or misc flags");
    return out;
}
std::optional<PredicateOperation> predicateOperation(uint16_t type) {
    switch (type) {
    case 0x241:
    case 0x30b2:
    case 0x31b2:
    case 0x331b:
    case 0x33e1:
    case 0x34f9:
        return PredicateOperation::Begin;
    case 0x243:
    case 0x30b3:
    case 0x31b3:
    case 0x331c:
    case 0x33e2:
    case 0x34fa:
        return PredicateOperation::End;
    case 0x248:
    case 0x30b5:
    case 0x31b5:
    case 0x331e:
    case 0x33e4:
    case 0x34fc:
        return PredicateOperation::Set;
    default:
        return {};
    }
}
PredicateCommand readPredicateCommand(uint16_t type, Bytes payload) {
    auto operation = predicateOperation(type);
    if (!operation)
        throw std::runtime_error("Not a predicate command");
    Reader r(payload);
    r.skip(8);
    PredicateCommand out{*operation, r.read<Id>(), r.read<Id>()};
    if (*operation == PredicateOperation::Set)
        out.value = r.read<uint32_t>();
    r.end();
    return out;
}
} // namespace flora
