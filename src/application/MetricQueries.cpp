#include "MetricQueries.h"
#include <algorithm>
#include <stdexcept>
namespace flora {
namespace {
MetricQuery &requireQuery(const std::shared_ptr<MetricQuery> &query) {
    if (!query)
        throw std::runtime_error("Missing query object");
    return *query;
}
} // namespace
MetricQueryBatch::MetricQueryBatch(std::vector<std::shared_ptr<MetricQuery>> queries, uint64_t first,
                                   uint64_t second, uint32_t value)
    : key0(first), key1(second), tag(value), queries_(std::move(queries)) {}
void MetricQueryBatch::begin(void *context) {
    if (state == 0) {
        for (const auto &query : queries_)
            requireQuery(query).begin(context);
        state = 1;
    }
}
void MetricQueryBatch::end(void *context) {
    if (state == 1) {
        for (auto it = queries_.rbegin(); it != queries_.rend(); ++it)
            requireQuery(*it).end(context);
        state = 2;
    }
}
bool MetricQueryBatch::poll(bool wait) {
    if (state == 2) {
        bool allReady = true;
        for (const auto &query : queries_) {
            bool ready = false;
            for (unsigned attempt = 0; attempt < (wait ? 0xffffffu : 1u); ++attempt) {
                if (requireQuery(query).ready(wait && attempt == 0)) {
                    if (!requireQuery(query).valid())
                        return false;
                    ready = true;
                    break;
                }
            }
            allReady = allReady && ready;
        }
        if (allReady)
            state = 3;
    }
    return state == 3;
}
bool MetricQueryBatch::dispatch(std::span<MetricQuerySink *const> metrics, bool wait) {
    if (!poll(wait))
        return false;
    for (auto *metric : metrics) {
        if (!metric)
            continue;
        metric->setKey(key0);
        for (const auto &query : queries_) {
            if (requireQuery(query).category() == metric->category() &&
                requireQuery(query).writeMetric(*metric)) {
                metric->complete(key1, tag);
                break;
            }
        }
    }
    state = 0;
    return true;
}
std::vector<MetricBatchPtr> PendingMetricPool::drain(uint64_t slot, uint64_t limit,
                                                     const MetricBatchAcceptor &accept) {
    auto &pending = bySlot[slot];
    const auto count = limit ? size_t(std::min<uint64_t>(limit, pending.size())) : pending.size();
    const std::vector<MetricBatchPtr> inspected(pending.begin(), pending.begin() + count);
    std::vector<MetricBatchPtr> output;
    size_t consumed = 0;
    for (const auto &item : inspected) {
        if (item) {
            if (accept(item)) {
                failures = 0;
                output.push_back(item);
            } else {
                failures = uint8_t(unsigned(failures) + 1);
                if (failures <= failureLimit)
                    break;
            }
        }
        ++consumed;
    }
    // The publisher retains the entire inspected prefix when none succeeded.
    if (consumed && !output.empty())
        pending.erase(pending.begin(), pending.begin() + std::min(consumed, pending.size()));
    return output;
}
bool DeferredMetricQueries::arm() {
    if (dirty)
        return false;
    dirty = true;
    for (const auto &batch : batches) {
        if (!batch)
            throw std::runtime_error("Null query batch cannot be armed");
        if (batch->state == 0)
            batch->state = 2;
    }
    return true;
}
void DeferredMetricQueries::drain(const MetricBatchAcceptor &accept) {
    if (dirty) {
        dirty = false;
        for (const auto &batch : batches)
            if (batch && batch->state == 2 && !accept(batch))
                dirty = true;
    }
}
void MetricQueryDrain::drain(bool wait) {
    if (!pending || !recycled)
        return;
    if (updateClock)
        updateClock(false);
    const auto accept = [&](const MetricBatchPtr &batch) { return batch->dispatch(metrics, wait); };
    for (uint64_t slot = 0; slot < (slotCount ? slotCount : 1); ++slot) {
        auto ready = pending->drain(slot, limited ? limit : 0, accept);
        if (!ready.empty()) {
            auto &list = (*recycled)[slot];
            list.insert(list.end(), ready.begin(), ready.end());
        }
    }
    for (const auto &[key, queries] : deferred) {
        if (!queries)
            throw std::runtime_error("Missing deferred query list");
        queries->drain(accept);
    }
    for (auto *metric : metrics)
        if (metric)
            metric->flush();
}
} // namespace flora
