#include "MetricCollector.h"
#include <algorithm>
#include <bit>
#include <iterator>
#include <stdexcept>
namespace flora {
std::vector<MetricProviderPtr> metricPoolProviderOrder(std::span<const MetricProviderPtr> providers) {
    std::vector<MetricProviderPtr> regular;
    MetricProviderPtr special;
    for (const auto &provider : providers) {
        if (!provider)
            throw std::runtime_error("Missing query provider");
        if (provider->category() == -1)
            special = provider;
        else
            regular.push_back(provider);
    }
    if (special)
        regular.push_back(special);
    return regular;
}
MetricQueryPool::MetricQueryPool(std::vector<MetricProviderPtr> providers, uint64_t count)
    : capacity(count), providers_(std::move(providers)) {}
std::vector<MetricBatchPtr> &MetricQueryPool::items(uint64_t slot) { return bySlot[slot]; }
bool MetricQueryPool::empty(uint64_t slot) { return items(slot).empty(); }
MetricBatchPtr MetricQueryPool::peek(uint64_t slot) {
    const auto &list = items(slot);
    return list.empty() ? nullptr : list.back();
}
void MetricQueryPool::append(uint64_t slot, MetricBatchPtr batch) {
    if (batch)
        items(slot).push_back(std::move(batch));
}
MetricBatchPtr MetricQueryPool::takeFirst(uint64_t slot) {
    auto &list = items(slot);
    if (list.empty())
        return {};
    auto batch = list.front();
    list.erase(list.begin());
    return batch;
}
MetricBatchPtr MetricQueryPool::take(uint64_t slot, bool replenish) {
    auto &list = items(slot);
    if (list.empty() && replenish) {
        if (capacity >= (uint64_t(1) << 32))
            throw std::runtime_error("Native pool refill counter would wrap");
        for (uint64_t i = 0; i < capacity; ++i) {
            std::vector<std::shared_ptr<MetricQuery>> queries;
            for (const auto &provider : providers_)
                if (provider) {
                    auto query = provider->create();
                    if (query)
                        queries.push_back(std::move(query));
                }
            list.push_back(std::make_shared<MetricQueryBatch>(std::move(queries), 0, 0, 6));
        }
    }
    if (list.empty())
        return {};
    auto batch = list.back();
    list.pop_back();
    return batch;
}
uint64_t MetricContextSlots::slot(uint64_t context) {
    if (!context) {
        for (size_t i = 0; i < contexts.size(); ++i)
            if (contexts[i] && contextType(contexts[i]) == 0)
                return i;
        return 0;
    }
    const auto found = std::find(contexts.begin(), contexts.end(), context);
    if (found != contexts.end())
        return uint64_t(found - contexts.begin());
    contexts.push_back(context);
    return contexts.size() - 1;
}
MetricDeferredNotifications::MetricDeferredNotifications(MetricContextSlots &contexts,
                                                         MetricQueryDrain &drain)
    : contexts_(contexts), drain_(drain) {}
void MetricDeferredNotifications::notify(uint64_t context, uint64_t key, int32_t kind) {
    if (!recording || !drain_.pending || !drain_.recycled)
        return;
    if (kind == 2) {
        const auto slot = contexts_.slot(context);
        drain_.slotCount = contexts_.contexts.size();
        auto &list = recording->items(slot);
        auto batches = std::make_shared<DeferredMetricQueries>();
        batches->batches = std::move(list);
        list.clear();
        batches->sourceSlot = slot;
        // Duplicate keys consume the recording list but keep the original sealed list.
        drain_.deferred.emplace(key, std::move(batches));
    } else if (kind == 3) {
        const auto found = drain_.deferred.find(key);
        if (found != drain_.deferred.end() && found->second) {
            const auto batches = found->second;
            while (!batches->arm())
                drain_.drain(true);
        }
    } else if (kind == 0) {
        const auto found = drain_.deferred.find(key);
        if (found != drain_.deferred.end() && found->second) {
            const auto batches = found->second;
            for (const auto &batch : batches->batches) {
                if (!batches->dirty) {
                    if (!batch)
                        throw std::runtime_error("Missing released query batch");
                    batch->state = 0;
                    (*drain_.recycled)[batches->sourceSlot].push_back(batch);
                } else if (batch)
                    drain_.pending->bySlot[batches->sourceSlot].push_back(batch);
            }
            drain_.deferred.erase(found);
        }
    }
}
MetricProviderBinding::MetricProviderBinding(std::vector<MetricProviderPtr> providers, size_t selected,
                                             std::optional<std::set<uint32_t>> compatible)
    : providers_(std::move(providers)), selected_(selected) {
    if (providers_.empty() || selected_ >= providers_.size())
        throw std::runtime_error("Metric requires a valid selected provider");
    for (const auto &provider : providers_) {
        if (!provider)
            throw std::runtime_error("Missing query provider");
        compatible_.insert(uint32_t(provider->category()));
    }
    if (compatible)
        compatible_ = std::move(*compatible);
}
MetricProviderPtr MetricProviderBinding::bind(int32_t kind) {
    for (size_t i = 0; i < providers_.size(); ++i)
        if (providers_[i]->category() == kind) {
            selected_ = i;
            return providers_[i];
        }
    return {};
}
void MetricProviderBinding::setKey(uint64_t) {
    throw std::runtime_error("Metric key callback is not implemented");
}
void MetricProviderBinding::complete(uint64_t, uint32_t) {
    throw std::runtime_error("Metric completion callback is not implemented");
}
void MetricProviderBinding::flush() { throw std::runtime_error("Metric flush callback is not implemented"); }
MetricSubscriptions::MetricSubscriptions(std::function<void()> rebuild, bool value,
                                         std::optional<uint32_t> category)
    : mode(value), preferred(category), rebuild_(std::move(rebuild)) {}
MetricProviderPtr MetricSubscriptions::select(MetricProviderBinding &metric) {
    const auto current = metric.provider();
    if (current->category() == -1)
        return current;
    const auto &choices = metric.compatible();
    std::set<uint32_t> overlap;
    std::set_intersection(compatible.begin(), compatible.end(), choices.begin(), choices.end(),
                          std::inserter(overlap, overlap.end()));
    uint32_t chosen;
    if (!compatible.empty() && !overlap.empty()) {
        compatible = std::move(overlap);
        chosen = *compatible.begin();
    } else {
        compatible = choices;
        chosen = uint32_t(current->category());
    }
    if (preferred && compatible.contains(*preferred))
        chosen = *preferred;
    return metric.bind(std::bit_cast<int32_t>(chosen));
}
void MetricSubscriptions::subscribe(std::shared_ptr<MetricProviderBinding> metric) {
    if (!metric)
        throw std::runtime_error("Missing provider metric");
    metric->setMode(mode);
    if (std::find(metrics.begin(), metrics.end(), metric) == metrics.end())
        metrics.push_back(metric);
    for (const auto &provider : providers)
        if (metric->bind(provider->category()))
            return;
    if (metric->provider()->category() == -1)
        providers.push_back(metric->provider());
    else {
        const auto provider = select(*metric);
        if (!provider)
            return;
        std::erase_if(providers, [](const auto &p) { return p->category() != -1; });
        providers.push_back(provider);
        for (const auto &item : metrics)
            if (item->provider()->category() != -1)
                item->bind(provider->category());
    }
    rebuild_();
}
void MetricSubscriptions::unsubscribe(const std::shared_ptr<MetricProviderBinding> &metric) {
    if (!metric)
        throw std::runtime_error("Missing provider metric");
    if (std::none_of(metrics.begin(), metrics.end(), [&](const auto &item) {
            return item != metric && item->provider() == metric->provider();
        })) {
        std::erase(providers, metric->provider());
        rebuild_();
    }
    std::erase(metrics, metric);
}
MetricCollector::MetricCollector(std::shared_ptr<MetricContextSlots> contextSlots,
                                 std::function<void(bool)> updateClock, uint64_t count, bool mode,
                                 std::optional<uint32_t> preferred, bool defer)
    : deferReads(defer), capacity(count),
      contexts(contextSlots ? std::move(contextSlots) : std::make_shared<MetricContextSlots>()),
      notifications(*contexts, drain), subscriptions([this] { rebuild(); }, mode, preferred) {
    drain.updateClock = std::move(updateClock);
    drain.slotCount = contexts->contexts.size();
}
void MetricCollector::rebuild() {
    ++generation;
    auto providers = metricPoolProviderOrder(subscriptions.providers);
    auto free = providers.empty() ? nullptr : std::make_unique<MetricQueryPool>(providers, capacity);
    auto active = providers.empty() ? nullptr : std::make_unique<MetricQueryPool>(providers, 0);
    auto waiting = providers.empty() ? nullptr : std::make_unique<PendingMetricPool>();
    if (waiting)
        waiting->failureLimit = 3;
    recycled = std::move(free);
    recording = std::move(active);
    pending = std::move(waiting);
    drain.pending = pending.get();
    drain.recycled = recycled ? &recycled->bySlot : nullptr;
    notifications.recording = recording.get();
}
void MetricCollector::setCapacity(uint64_t value) {
    if (value != capacity) {
        capacity = value;
        rebuild();
    }
}
void MetricCollector::updateMetrics() {
    drain.metrics.clear();
    for (const auto &metric : subscriptions.metrics)
        drain.metrics.push_back(metric.get());
}
void MetricCollector::subscribe(std::shared_ptr<MetricProviderBinding> metric) {
    subscriptions.subscribe(std::move(metric));
    updateMetrics();
}
void MetricCollector::unsubscribe(const std::shared_ptr<MetricProviderBinding> &metric) {
    subscriptions.unsubscribe(metric);
    updateMetrics();
}
MetricBatchPtr MetricCollector::begin(uint64_t key0, uint64_t key1, uint32_t tag, uint64_t context) {
    if (!recycled || !pending)
        return {};
    const auto slot = contexts->slot(context);
    drain.slotCount = contexts->contexts.size();
    auto &list = pending->bySlot[slot];
    auto batch = recycled->take(slot, list.empty());
    if (!batch && !list.empty()) {
        batch = list.front();
        list.erase(list.begin());
        batch->dispatch(drain.metrics, true);
    }
    if (!batch)
        return {};
    batch->key0 = key0;
    batch->key1 = key1;
    batch->tag = tag;
    batch->begin(reinterpret_cast<void *>(uintptr_t(context)));
    if (context && contexts->contextType(context) == 1) {
        if (!recording)
            throw std::runtime_error("Deferred recording pool is missing");
        recording->append(slot, batch);
    } else
        list.push_back(batch);
    return batch;
}
void MetricCollector::end(uint64_t context) {
    if (!pending)
        return;
    const auto slot = contexts->slot(context);
    drain.slotCount = contexts->contexts.size();
    std::vector<MetricBatchPtr> *list;
    if (context && contexts->contextType(context) == 1) {
        if (!recording)
            throw std::runtime_error("Deferred recording pool is missing");
        list = &recording->items(slot);
    } else
        list = &pending->bySlot[slot];
    if (!list->empty()) {
        if (!list->back())
            throw std::runtime_error("Missing pending query batch");
        list->back()->end(reinterpret_cast<void *>(uintptr_t(context)));
    }
    if (!deferReads)
        drain.drain(false);
}
} // namespace flora
