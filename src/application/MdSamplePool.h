#pragma once
#include "MetricSampleTransport.h"
#include <functional>
#include <memory>
namespace flora {
// Borrowed transports/observers must outlive these serialized collection adapters.
class MdCounter final {
  public:
    MdCounter(MetricCounterTransport &metrics, MetricPublisherObserver &publisher,
              unsigned timeoutMs = 10000);
    ~MdCounter();
    MdCounter(const MdCounter &) = delete;
    MdCounter &operator=(const MdCounter &) = delete;
    void begin();
    MetricResult end();
    bool active() const;
    const nlohmann::json &audit() const;

  private:
    struct State;
    std::unique_ptr<State> state_;
};
class MdSamplePool {
  public:
    using Consumer = std::function<void(MetricResult &)>;
    MdSamplePool(MetricSampleTransport &metrics, MetricPublisherObserver &publisher, unsigned capacity = 256,
                 unsigned timeoutMs = 10000);
    virtual ~MdSamplePool();
    MdSamplePool(const MdSamplePool &) = delete;
    MdSamplePool &operator=(const MdSamplePool &) = delete;
    void begin(Consumer consume = {});
    void submit();
    MetricResult end();
    void drain(bool wait = false);
    void finish();
    void close();
    nlohmann::json report() const;
    size_t ownedCount() const;
    bool active() const;

  protected:
    MdSamplePool(MetricSampleTransport &metrics, MetricPublisherObserver &publisher, unsigned capacity,
                 unsigned timeoutMs, bool reuse);

  private:
    struct State;
    std::unique_ptr<State> state_;
};
class MdReusingPool final : public MdSamplePool {
  public:
    MdReusingPool(MetricSampleTransport &metrics, MetricPublisherObserver &publisher, unsigned capacity = 256,
                  unsigned timeoutMs = 10000)
        : MdSamplePool(metrics, publisher, capacity, timeoutMs, true) {}
};
} // namespace flora
