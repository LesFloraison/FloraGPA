#pragma once
#include "MdScheduledPool.h"
#include "MetricIterations.h"
#include "MetricPriorityWin32.h"
#include "MetricPublisher.h"
struct ID3D11Device;
namespace flora {
class MetricsDiscovery;
nlohmann::json mdIterationDescriptors(const nlohmann::json &catalog, const nlohmann::json &symbols);
struct MdIterationClient {
    MetricSampleTransport &samples;
    MetricClockTransport &clock;
    const nlohmann::json &catalog;
    std::function<void(const std::string &)> select;
    std::function<uint64_t()> sampleCount;
    std::function<void()> close;
    std::function<bool()> closed;
};
// Single immediate context; calls are serialized. Client/device outlive the session.
// Explicit close reports failures and can be retried while priority ownership remains held.
class MdIterationTransport final : public MetricIterationTransport {
  public:
    using Acquire = std::function<void(MdIterationTransport &, uint32_t, const nlohmann::json &)>;
    using LockFactory = std::function<std::unique_ptr<MetricPriorityLock>(const nlohmann::json &)>;
    MdIterationTransport(MdIterationClient client, uint64_t devicePointer, const nlohmann::json &symbols,
                         Acquire acquire, LockFactory lockFactory = {});
    MdIterationTransport(MetricsDiscovery &metrics, ID3D11Device *device, const nlohmann::json &symbols,
                         Acquire acquire, LockFactory lockFactory = {});
    ~MdIterationTransport() override;
    MdIterationTransport(const MdIterationTransport &) = delete;
    MdIterationTransport &operator=(const MdIterationTransport &) = delete;
    nlohmann::json descriptions() override;
    nlohmann::json prepare(const nlohmann::json &ids) override;
    nlohmann::json replay(uint32_t pass, const nlohmann::json &ranges, bool requestFlag) override;
    void begin(MdScheduledPool::Consumer consume);
    void submit();
    void deliver(const nlohmann::json &description, const nlohmann::json &row);
    void close();
    bool closed() const;
    nlohmann::json audit() const;
    const nlohmann::json &catalog() const;
    const nlohmann::json &plan() const;
    const nlohmann::json &requestedIds() const;
    MetricPublisherValues &publisherValues();

  private:
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace flora
