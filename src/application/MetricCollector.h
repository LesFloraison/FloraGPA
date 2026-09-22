#pragma once
#include "MetricQueries.h"
#include <optional>
#include <set>
namespace flora {
class MetricQueryProvider {
  public:
    virtual ~MetricQueryProvider() = default;
    virtual int32_t category() const = 0;
    virtual std::shared_ptr<MetricQuery> create() = 0;
};
using MetricProviderPtr = std::shared_ptr<MetricQueryProvider>;
std::vector<MetricProviderPtr> metricPoolProviderOrder(std::span<const MetricProviderPtr> providers);
class MetricQueryPool {
  public:
    explicit MetricQueryPool(std::vector<MetricProviderPtr> providers = {}, uint64_t capacity = 0);
    std::vector<MetricBatchPtr> &items(uint64_t slot);
    bool empty(uint64_t slot);
    MetricBatchPtr peek(uint64_t slot);
    void append(uint64_t slot, MetricBatchPtr batch);
    MetricBatchPtr takeFirst(uint64_t slot);
    MetricBatchPtr take(uint64_t slot, bool replenish = false);
    MetricBatchSlots bySlot;
    const std::vector<MetricProviderPtr> &providers() const { return providers_; }
    uint64_t capacity;

  private:
    std::vector<MetricProviderPtr> providers_;
};
class MetricContextSlots {
  public:
    std::vector<uint64_t> contexts;
    std::function<int32_t(uint64_t)> contextType;
    uint64_t slot(uint64_t context = 0);
};
class MetricDeferredNotifications {
  public:
    MetricDeferredNotifications(MetricContextSlots &contexts, MetricQueryDrain &drain);
    void notify(uint64_t context, uint64_t key, int32_t kind);
    MetricQueryPool *recording{};

  private:
    MetricContextSlots &contexts_;
    MetricQueryDrain &drain_;
};
class MetricProviderBinding : public MetricQuerySink {
  public:
    explicit MetricProviderBinding(std::vector<MetricProviderPtr> providers, size_t selected = 0,
                                   std::optional<std::set<uint32_t>> compatible = {});
    const MetricProviderPtr &provider() const { return providers_[selected_]; }
    uint64_t category() const override { return uint64_t(int64_t(provider()->category())); }
    virtual void setMode(bool value) { mode = value; }
    virtual MetricProviderPtr bind(int32_t kind);
    const auto &compatible() const { return compatible_; }
    size_t selectedIndex() const { return selected_; }
    bool mode{};
    // Binding-only metrics must supply dispatch callbacks before collection.
    void setKey(uint64_t) override;
    void complete(uint64_t, uint32_t) override;
    void flush() override;

  private:
    std::vector<MetricProviderPtr> providers_;
    size_t selected_;
    std::set<uint32_t> compatible_;
};
class MetricSubscriptions {
  public:
    explicit MetricSubscriptions(std::function<void()> rebuild, bool mode = false,
                                 std::optional<uint32_t> preferred = {});
    MetricProviderPtr select(MetricProviderBinding &metric);
    void subscribe(std::shared_ptr<MetricProviderBinding> metric);
    void unsubscribe(const std::shared_ptr<MetricProviderBinding> &metric);
    std::vector<std::shared_ptr<MetricProviderBinding>> metrics;
    std::vector<MetricProviderPtr> providers;
    std::set<uint32_t> compatible;
    bool mode;
    std::optional<uint32_t> preferred;

  private:
    std::function<void()> rebuild_;
};
class MetricCollector final {
  public:
    // Adapters serialize access and own native query cleanup; rebuild discards pool ownership.
    explicit MetricCollector(std::shared_ptr<MetricContextSlots> contexts = {},
                             std::function<void(bool)> updateClock = {}, uint64_t capacity = 256,
                             bool mode = false, std::optional<uint32_t> preferred = {},
                             bool deferReads = false);
    MetricCollector(const MetricCollector &) = delete;
    MetricCollector &operator=(const MetricCollector &) = delete;
    void rebuild();
    void setCapacity(uint64_t value);
    void subscribe(std::shared_ptr<MetricProviderBinding> metric);
    void unsubscribe(const std::shared_ptr<MetricProviderBinding> &metric);
    MetricBatchPtr begin(uint64_t key0, uint64_t key1, uint32_t tag = 6, uint64_t context = 0);
    void end(uint64_t context = 0);
    bool deferReads;
    uint64_t capacity, generation{};
    std::shared_ptr<MetricContextSlots> contexts;
    std::unique_ptr<MetricQueryPool> recycled, recording;
    std::unique_ptr<PendingMetricPool> pending;
    MetricQueryDrain drain;
    MetricDeferredNotifications notifications;
    MetricSubscriptions subscriptions;

  private:
    void updateMetrics();
};
} // namespace flora
