#pragma once
#include "MetricPriorityWin32.h"
#include "core/Frame.h"
#include <functional>

namespace flora {
class MetricConsumer {
  public:
    explicit MetricConsumer(const nlohmann::json &handles = nlohmann::json::array(),
                            nlohmann::json timingHandle = 0);
    void reset(const nlohmann::json &handles);
    void receive(uint64_t handle, uint64_t payloadType, std::optional<Bytes> payload, bool present = true);
    nlohmann::json snapshot() const;
    const auto &handles() const { return handles_; }
    const auto &counts() const { return counts_; }
    const auto &rows() const { return rows_; }
    const auto &timings() const { return timings_; }
    size_t slot(uint64_t handle) const;

  private:
    nlohmann::json timingHandle_;
    std::vector<uint64_t> handles_, counts_;
    std::vector<std::vector<double>> rows_;
    std::vector<std::array<uint64_t, 2>> timings_;
};
class MetricProbeBackend {
  public:
    virtual ~MetricProbeBackend() = default;
    virtual int32_t beginProbe(uint64_t handle, uint64_t key0, uint64_t key1, uint32_t tag,
                               uint64_t context) = 0;
    virtual int32_t endProbe(uint64_t handle, uint64_t key, uint32_t tag, uint64_t context) = 0;
};
class MetricProbeFanout {
  public:
    MetricProbeFanout(const nlohmann::json &handles, MetricProbeBackend &backend);
    bool begin(uint64_t key0, uint64_t key1, uint32_t tag, uint64_t context = 0);
    bool end(uint64_t key, uint32_t tag, uint64_t context = 0);

  private:
    std::vector<uint64_t> handles_;
    MetricProbeBackend &backend_;
};
class MetricPassPublisher {
  public:
    virtual ~MetricPassPublisher() = default;
    virtual void setPool(uint32_t capacity) = 0;
    virtual int32_t subscribe(uint64_t consumer, uint64_t metric) = 0;
    virtual int32_t unsubscribe(uint64_t consumer, uint64_t metric) = 0;
    virtual bool begin(uint64_t key0, uint64_t key1, uint32_t tag, uint64_t context) = 0;
    virtual bool end(uint64_t key, uint32_t tag, uint64_t context) = 0;
    virtual bool configure(uint64_t key, Bytes configuration) = 0;
    virtual void flush(uint64_t consumer) = 0;
};
// Callers serialize operations and own publisher/lock/GPU cleanup on every exit.
// In particular, empty requests and failures do not synthesize lock releases.
class MetricPassController {
  public:
    using ConfigurationProvider = std::function<std::vector<uint8_t>(uint32_t)>;
    MetricPassController(const nlohmann::json &handles, const nlohmann::json &requests,
                         const nlohmann::json &passes, MetricPassPublisher &publisher,
                         MetricPriorityLock &primary, MetricPriorityLock *secondary = nullptr,
                         const nlohmann::json &options = nlohmann::json::object(),
                         MetricConsumer *consumer = nullptr);
    nlohmann::json prepare(const nlohmann::json &requests, const nlohmann::json &compatibility,
                           const nlohmann::json &deviceKey, const ConfigurationProvider &provider);
    void select(const nlohmann::json &index);
    void begin(const nlohmann::json &deviceKey);
    bool configure(const nlohmann::json &deviceKey, Bytes configuration);
    void end();
    void flush();
    void finish(const nlohmann::json &unsubscribe = true);
    void setProbeDeviceKey(const nlohmann::json &value);
    uint32_t probeDeviceKey() const { return probeDeviceKey_; }
    MetricConsumer &consumer() { return *consumer_; }
    const auto &handles() const { return handles_; }
    const auto &requests() const { return requests_; }
    const auto &passes() const { return passes_; }
    uint32_t currentPass{UINT32_MAX}, completedProbes{}, reportedPassCount{};
    nlohmann::json snapshot() const;

  private:
    nlohmann::json handles_, requests_, passes_, timingHandle_;
    uint64_t consumerHandle_{};
    uint32_t vendor_{}, probeDeviceKey_{}, poolSize_{};
    MetricPassPublisher &publisher_;
    MetricPriorityLock &primary_;
    MetricPriorityLock *secondary_;
    std::unique_ptr<MetricConsumer> ownedConsumer_;
    MetricConsumer *consumer_{};
};
} // namespace flora
