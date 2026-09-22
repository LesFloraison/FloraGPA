#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <vector>
namespace flora {
class MetricQuerySink {
  public:
    virtual ~MetricQuerySink() = default;
    virtual uint64_t category() const = 0;
    virtual void setKey(uint64_t key) = 0;
    virtual void complete(uint64_t key, uint32_t tag) = 0;
    virtual void flush() = 0;
};
class MetricQuery {
  public:
    virtual ~MetricQuery() = default;
    virtual uint64_t category() const = 0;
    virtual void begin(void *context) = 0;
    virtual void end(void *context) = 0;
    virtual bool ready(bool flush) = 0;
    virtual bool valid() = 0;
    virtual bool writeMetric(MetricQuerySink &metric) = 0;
};
class MetricQueryBatch {
  public:
    explicit MetricQueryBatch(std::vector<std::shared_ptr<MetricQuery>> queries, uint64_t key0 = 0,
                              uint64_t key1 = 0, uint32_t tag = 0);
    void begin(void *context);
    void end(void *context);
    bool poll(bool wait = false);
    bool dispatch(std::span<MetricQuerySink *const> metrics, bool wait = false);
    const auto &queries() const { return queries_; }
    uint32_t state{};
    uint64_t key0{}, key1{};
    uint32_t tag{};

  private:
    std::vector<std::shared_ptr<MetricQuery>> queries_;
};
using MetricBatchPtr = std::shared_ptr<MetricQueryBatch>;
using MetricBatchSlots = std::map<uint64_t, std::vector<MetricBatchPtr>>;
using MetricBatchAcceptor = std::function<bool(const MetricBatchPtr &)>;
class PendingMetricPool {
  public:
    MetricBatchSlots bySlot;
    uint8_t failures{}, failureLimit{};
    std::vector<MetricBatchPtr> drain(uint64_t slot, uint64_t limit, const MetricBatchAcceptor &accept);
};
class DeferredMetricQueries {
  public:
    std::vector<MetricBatchPtr> batches;
    bool dirty{};
    uint64_t sourceSlot{};
    bool arm();
    void drain(const MetricBatchAcceptor &accept);
};
class MetricQueryDrain {
  public:
    PendingMetricPool *pending{};
    MetricBatchSlots *recycled{};
    std::vector<MetricQuerySink *> metrics;
    std::function<void(bool)> updateClock;
    uint64_t slotCount{}, limit{};
    bool limited{};
    std::map<uint64_t, std::shared_ptr<DeferredMetricQueries>> deferred;
    void drain(bool wait = false);
};
} // namespace flora
