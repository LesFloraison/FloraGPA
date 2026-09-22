#pragma once
#include "MetricSampleTransport.h"
#include <functional>
#include <memory>
namespace flora {
class MdScheduledPool final {
  public:
    using Consumer = std::function<void(MetricResult &)>;
    // Calls are serialized. The borrowed transport and observer must outlive this pool.
    MdScheduledPool(MetricSampleTransport &metrics, MetricPublisherObserver &publisher,
                    unsigned capacity = 256, unsigned timeoutMs = 10000);
    ~MdScheduledPool();
    MdScheduledPool(const MdScheduledPool &) = delete;
    MdScheduledPool &operator=(const MdScheduledPool &) = delete;
    void begin(Consumer consume = {});
    void submit();
    MetricResult end();
    void drain(bool wait = false);
    void finish();
    void close();
    nlohmann::json report() const;
    size_t ownedCount() const;
    bool active() const;

  private:
    struct State;
    std::shared_ptr<State> state_;
};
} // namespace flora
