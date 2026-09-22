#pragma once
#include "MetricSampleTransport.h"
#include <QString>
#include <memory>
namespace flora {
class MetricCommandLists {
  public:
    virtual ~MetricCommandLists() = default;
    virtual int32_t contextType(uint64_t context) = 0;
    virtual uint64_t finish(uint64_t context, bool restore) = 0;
    virtual void release(uint64_t command) = 0;
};
// Owns finished command lists and counter tokens. Use one host thread and close before metrics/device.
class MdRecordedQueries final {
  public:
    explicit MdRecordedQueries(MetricRecordedTransport &metrics, unsigned timeoutMs = 10000,
                               bool publisherValues = false, MetricCommandLists *commands = nullptr);
    ~MdRecordedQueries();
    MdRecordedQueries(const MdRecordedQueries &) = delete;
    MdRecordedQueries &operator=(const MdRecordedQueries &) = delete;
    void begin(uint64_t context, uint64_t key0, std::optional<uint64_t> key1 = {}, uint32_t tag = 6);
    void end(uint64_t context);
    uint64_t finish(uint64_t context, bool restore = false);
    uint64_t execute(uint64_t command, bool restore = false);
    void release(uint64_t command);
    void drain(bool wait = true);
    nlohmann::json report() const;
    nlohmann::json exportReport(const QString &folder) const;
    void close();
    bool closed() const;
    bool failed() const;
    bool abandoned() const;
    size_t ownedCount() const;
    size_t commandCount() const;
    const nlohmann::json &records() const;

  private:
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace flora
