#pragma once
#include "MetricPriorityWin32.h"
#include <exception>
namespace flora {
class MetricsDiscovery;
struct MetricPriorityClient {
    nlohmann::json catalog;
    std::function<void()> close;
    std::function<bool()> closed;
};
class MetricAcquisitionPriority {
  public:
    using LockFactory = std::function<std::unique_ptr<MetricPriorityLock>(const nlohmann::json &)>;
    using Save = std::function<void(const QString &, const nlohmann::json &)>;
    MetricAcquisitionPriority(MetricPriorityClient client, QString output, LockFactory factory = {},
                              Save save = {});
    MetricAcquisitionPriority(MetricsDiscovery &metrics, QString output, LockFactory factory = {},
                              Save save = {});
    nlohmann::json report() const;
    void save();
    void run(const std::function<void(MetricAcquisitionPriority &)> &body);
    void replay(const nlohmann::json &index, const nlohmann::json &name, const nlohmann::json &sample,
                const std::function<void()> &body);
    void close();

  private:
    MetricPriorityClient client_;
    QString path_;
    LockFactory factory_;
    Save save_;
    std::unique_ptr<MetricPriorityLock> lock_;
    nlohmann::json passes_ = nlohmann::json::array(), failure_;
    bool active_{};
    void enter();
};
nlohmann::json metricPriorityFailure(std::exception_ptr exception);
nlohmann::json validateMetricPriorityResult(const QString &folder, const nlohmann::json &profile,
                                            bool required = false);
} // namespace flora
