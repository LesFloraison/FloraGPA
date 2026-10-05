#include "InitializationSchedule.h"
#include <algorithm>
#include <stdexcept>
#include <string>

namespace flora {
InitializationSchedule modelInitializationSchedule(const std::vector<InitializationScheduleNode> &input,
                                                   size_t workLimit) {
    InitializationSchedule result;
    std::map<uint32_t, const InitializationScheduleNode *> nodes;
    std::map<uint32_t, uint32_t> status;
    auto fail = [](uint32_t id, const char *reason) {
        throw std::runtime_error("Initialization node " + std::to_string(id) + ": " + reason);
    };
    auto step = [&](uint32_t id) {
        if (!workLimit)
            fail(id, "model work limit exceeded");
        --workLimit;
    };
    for (const auto &node : input) {
        step(node.id);
        if (node.kind < 1 || node.kind > 4)
            fail(node.id, "unknown cache category");
        if (!nodes.emplace(node.id, &node).second)
            fail(node.id, "duplicate identity");
        status[node.id] = node.initiallyReady ? 2 : 1;
        result.dependents[node.id];
    }
    // Reject cycles without recursing or trying the original unchecked native
    // scheduler. Pending nodes would otherwise recurse indefinitely.
    std::map<uint32_t, size_t> remaining;
    std::map<uint32_t, std::set<uint32_t>> edges;
    std::vector<uint32_t> ready;
    for (const auto &[id, node] : nodes) {
        remaining[id] = node->dependencies.size();
        if (node->dependencies.empty())
            ready.push_back(id);
        for (auto dependency : node->dependencies) {
            step(id);
            if (!nodes.contains(dependency))
                throw std::runtime_error("Initialization node " + std::to_string(id) +
                                         ": missing dependency " + std::to_string(dependency));
            edges[dependency].insert(id);
        }
    }
    size_t visited = 0;
    while (!ready.empty()) {
        auto id = ready.back();
        ready.pop_back();
        ++visited;
        for (auto child : edges[id]) {
            step(child);
            if (--remaining[child] == 0)
                ready.push_back(child);
        }
    }
    if (visited != nodes.size())
        for (const auto &[id, count] : remaining)
            if (count)
                fail(id, "cyclic or cycle-dependent graph is outside model scope");

    auto initialize = [&](uint32_t id) {
        step(id);
        result.order.push_back(id);
        result.previousStatuses.push_back(status.at(id));
        status.at(id) = 2;
    };
    struct Cursor {
        uint32_t id;
        std::set<uint32_t>::const_iterator next;
    };
    std::vector<Cursor> stack;
    auto enter = [&](uint32_t id) {
        step(id);
        bool allReady = true;
        for (auto dependency : nodes.at(id)->dependencies) {
            step(id);
            allReady &= status.at(dependency) == 2;
            result.dependents.at(dependency).insert(id);
        }
        if (allReady)
            initialize(id);
        stack.push_back({id, result.dependents.at(id).begin()});
    };
    // Native 0x9a8f0 traverses containers in this order, IDs ascending.
    for (auto kind : {4u, 2u, 1u, 3u}) {
        for (const auto &[id, node] : nodes) {
            if (node->kind != kind)
                continue;
            enter(id);
            while (!stack.empty()) {
                auto &cursor = stack.back();
                const auto &children = result.dependents.at(cursor.id);
                if (cursor.next == children.end()) {
                    stack.pop_back();
                    continue;
                }
                auto child = *cursor.next++;
                step(child);
                // Ready dependents are reinitialized directly, without a
                // recursive propagation pass. Preserve repeated callbacks.
                if (status.at(child) == 2)
                    initialize(child);
                else
                    enter(child);
            }
        }
    }
    for (const auto &[id, value] : status)
        if (value != 2)
            fail(id, "node remained uninitialized");
    return result;
}
} // namespace flora
