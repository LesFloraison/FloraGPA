#include "QueryCompletion.h"
#include "Contexts.h"
#include "Predication.h"
namespace flora {
bool isQueryGetData(uint16_t type) {
    return type == 0x30b4 || type == 0x31b4 || type == 0x331d || type == 0x33e3 || type == 0x34fb;
}
QueryCompletion readQueryGetData(Bytes bytes) {
    Reader r(bytes);
    QueryCompletion q;
    q.link = r.read<Id>();
    q.context = r.read<Id>();
    q.result = r.read<int32_t>();
    q.resource = r.read<Id>();
    q.hasData = r.flag();
    if (q.hasData)
        q.word = r.read<uint32_t>();
    q.size = r.read<uint32_t>();
    q.flags = r.read<uint32_t>();
    r.end();
    return q;
}
std::map<Id, uint32_t> auditOrdinaryQueryOperations(const Frame &frame, const CancelCheck &cancelled) {
    checkCancellation(cancelled);
    std::map<Id, uint32_t> descriptors;
    std::map<Id, uint32_t> result;
    for (const auto &[id, e] : frame.entries()) {
        checkCancellation(cancelled);
        if (e.category != 7) continue;
        if (e.type == 0x3152) {
            Reader r(frame.payload(id));
            const auto link = r.read<Id>(), query = r.read<Id>();
            const bool present = r.flag();
            uint32_t type = UINT32_MAX, flags = UINT32_MAX;
            if (present) { type = r.read<uint32_t>(); flags = r.read<uint32_t>(); }
            r.end();
            const bool ordinary = !link && query && present && (type == 0 || type == 2 || type == 3) && flags == 0 && !frame.entries().contains(query);
            const auto found = descriptors.find(query);
            // Conflicting metadata cannot establish an ordinary-query identity.
            descriptors[query] = ordinary && (found == descriptors.end() || found->second == type) ? type : UINT32_MAX;
        } else if (e.type != 0x243 && e.type != 0x241 &&
                   (predicateOperation(e.type) == PredicateOperation::End || predicateOperation(e.type) == PredicateOperation::Begin)) {
            Reader r(frame.payload(id));
            const auto link = r.read<Id>(), context = r.read<Id>(), query = r.read<Id>();
            r.end();
            if (!link && descriptors.contains(query) && descriptors.at(query) != UINT32_MAX &&
                (predicateOperation(e.type) == PredicateOperation::End || descriptors.at(query) == 3)) {
                requireImmediateContext(frame, context);
                result.emplace(id, descriptors.at(query));
            }
        }
    }
    return result;
}
std::set<Id> auditEventEndObservations(const Frame &frame, const CancelCheck &cancelled) {
    std::set<Id> result;
    for (const auto &[id, type] : auditOrdinaryQueryOperations(frame, cancelled)) result.insert(id);
    return result;
}
QueryCompletionAudit auditQueryCompletions(const Frame &frame, const CancelCheck &cancelled) {
    checkCancellation(cancelled);
    const auto eventEnds = auditOrdinaryQueryOperations(frame, cancelled);
    struct Interval {
        Id context{}, end{};
        bool active{};
        bool ordinary{};
        uint32_t type{};
    };
    std::map<Id, Interval> intervals;
    QueryCompletionAudit audit;
    for (const auto &[id, e] : frame.entries()) {
        checkCancellation(cancelled);
        if (e.category != 7)
            continue;
        const auto operation = predicateOperation(e.type);
        if (e.type == 0x3152) {
            Reader r(frame.payload(id));
            const auto link = r.read<Id>(), query = r.read<Id>();
            const bool present = r.flag();
            uint32_t type = UINT32_MAX, flags = UINT32_MAX;
            if (present) { type = r.read<uint32_t>(); flags = r.read<uint32_t>(); }
            r.end();
            auto found = intervals.find(query);
            if (found != intervals.end() && found->second.ordinary &&
                (link || !present || flags || type != found->second.type)) intervals.erase(found);
        }
        if (eventEnds.contains(id)) {
            const auto command = readPredicateCommand(e.type, frame.payload(id));
            intervals[command.resource] = {command.context, *operation == PredicateOperation::End ? id : 0,
                                          *operation == PredicateOperation::Begin, true, eventEnds.at(id)};
        }
        if (operation && *operation != PredicateOperation::Set && !eventEnds.contains(id)) {
            // Invalid predicate commands are independently rejected by replay.
            // Never use a malformed or unmatched interval as a completion proof.
            try {
                Reader prefix(frame.payload(id));
                if (prefix.read<Id>()) {
                    intervals.clear();
                    continue;
                }
                const auto command = readPredicateCommand(e.type, frame.payload(id));
                readPredicate(frame, command.resource);
                requireImmediateContext(frame, command.context);
                auto &interval = intervals[command.resource];
                if (*operation == PredicateOperation::Begin)
                    interval = {command.context, 0, true};
                else {
                    interval.end = interval.active && interval.context == command.context ? id : 0;
                    interval.active = false;
                }
            } catch (const OperationCancelled &) {
                throw;
            } catch (const std::exception &) {
                // Existing command validation supplies the located error.
                intervals.clear();
            }
        }
        if (!isQueryGetData(e.type))
            continue;
        auto &q = audit[id];
        try {
            q = readQueryGetData(frame.payload(id));
            if (q.result < 0 || q.result == 1)
                continue;
            if (q.result != 0)
                throw std::runtime_error("Unexpected successful GetData HRESULT");
            if (q.link)
                throw std::runtime_error("Nested GetData completion is unresolved");
            requireImmediateContext(frame, q.context);
            if (q.flags & ~1u)
                throw std::runtime_error("Unsupported GetData flags");
            if (!q.hasData && q.size)
                throw std::runtime_error("Successful GetData has a null output with nonzero size");
            const auto resource = frame.entries().find(q.resource);
            if (resource == frame.entries().end()) {
                const auto interval = intervals.find(q.resource);
                if (interval != intervals.end() && interval->second.ordinary && interval->second.end &&
                    !interval->second.active && interval->second.context == q.context) {
                    const auto type = interval->second.type;
                    const auto expected = type == 0 ? 4u : type == 2 ? 8u : 16u;
                    if (q.hasData && q.size != expected)
                        throw std::runtime_error("Ordinary query GetData size does not match its descriptor");
                    if (type == 0 && q.hasData && q.word != 1)
                        throw std::runtime_error("Successful EVENT GetData did not report completion");
                    q.end = interval->second.end;
                    q.ordinary = true;
                    continue;
                }
                q.missing = "Successful GetData has no saved query resource or End boundary; its CPU/GPU "
                            "completion dependency cannot be reconstructed";
                continue;
            }
            const auto desc = readPredicate(frame, q.resource);
            if (q.size && q.size != 4)
                throw std::runtime_error("Predicate GetData size is not BOOL");
            if (desc.flags & 1)
                throw std::runtime_error("GetData on a predicate with PREDICATEHINT is unsupported");
            const auto interval = intervals.find(q.resource);
            if (interval == intervals.end() || !interval->second.end || interval->second.active ||
                interval->second.context != q.context) {
                q.missing = "Successful predicate GetData has no completed saved Begin/End interval on this "
                            "context; frame-before completion is unavailable";
                continue;
            }
            q.end = interval->second.end;
        } catch (const OperationCancelled &) {
            throw;
        } catch (const std::exception &error) {
            q.error = error.what();
        }
    }
    return audit;
}
const QueryCompletion &requireQueryCompletion(const QueryCompletionAudit &audit, Id event) {
    const auto &q = audit.at(event);
    if (!q.error.empty())
        throw std::runtime_error("GetData at event " + std::to_string(event) + ", query " +
                                 std::to_string(q.resource) + ": " + q.error);
    return q;
}
} // namespace flora
