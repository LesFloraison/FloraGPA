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
QueryCompletionAudit auditQueryCompletions(const Frame &frame, const CancelCheck &cancelled) {
    checkCancellation(cancelled);
    struct Interval {
        Id context{}, end{};
        bool active{};
    };
    std::map<Id, Interval> intervals;
    QueryCompletionAudit audit;
    for (const auto &[id, e] : frame.entries()) {
        checkCancellation(cancelled);
        if (e.category != 7)
            continue;
        const auto operation = predicateOperation(e.type);
        if (operation && *operation != PredicateOperation::Set) {
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
