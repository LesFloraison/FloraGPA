#pragma once
#include "MetricPriority.h"
#include <QString>
#include <memory>
#include <nlohmann/json.hpp>
#include <stdexcept>
namespace flora {
class MetricPriorityTimeout : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};
QString metricPriorityDefaultPath();
bool metricPriorityProcessAlive(uint32_t pid);
struct MetricPriorityOptions {
    std::optional<QString> path, mutexName;
    double timeout{3}, guardTimeout{5};
};
class MetricPriorityLock {
  public:
    virtual ~MetricPriorityLock() = default;
    virtual void setPriority(uint32_t value) = 0;
    virtual bool acquire() = 0;
    virtual void release() = 0;
    virtual void close() = 0;
    virtual bool closed() const = 0;
    virtual nlohmann::json audit() const = 0;
};
class SharedMetricPriorityMutex final : public MetricPriorityLock {
  public:
    explicit SharedMetricPriorityMutex(std::string resource, MetricPriorityOptions options = {});
    ~SharedMetricPriorityMutex() override;
    SharedMetricPriorityMutex(const SharedMetricPriorityMutex &) = delete;
    SharedMetricPriorityMutex &operator=(const SharedMetricPriorityMutex &) = delete;
    void setPriority(uint32_t value) override;
    bool tryAcquire();
    bool acquire() override;
    void release() override;
    void close() override;
    bool closed() const override;
    nlohmann::json audit() const override;
    bool eligible();
    uint32_t highest();

  private:
    struct State;
    std::unique_ptr<State> state_;
};
std::unique_ptr<SharedMetricPriorityMutex> metricDeviceMutex(const nlohmann::json &catalog,
                                                             MetricPriorityOptions options = {});
} // namespace flora
