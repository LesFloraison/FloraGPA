#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace flora {
inline constexpr uint32_t priorityEmpty = 0xffffffff;
inline constexpr size_t priorityRows = 16, priorityEntries = 1024, priorityRowSize = 0x308c;
inline constexpr size_t priorityTableSize = 16 + priorityRows * priorityRowSize;
std::string metricResourceName(uint32_t kind, uint32_t low, uint32_t high, const std::string &suffix);
int64_t signedPriority(uint32_t value);
// Callers must serialize every operation, including construction, with the shared guard.
class MetricPriorityTable {
  public:
    explicit MetricPriorityTable(std::span<uint8_t> storage);
    static std::vector<uint8_t> initial();
    size_t offset(size_t row) const;
    std::string name(size_t row) const;
    size_t allocate(const std::string &name);
    uint32_t get(size_t row, size_t field) const;
    void put(size_t row, size_t field, uint32_t value);
    std::array<uint32_t, 3> entry(size_t row, size_t index) const;
    void insert(size_t row, uint32_t pid, uint32_t tid, uint32_t priority);
    void update(size_t row, uint32_t pid, uint32_t tid, uint32_t priority);
    void remove(size_t row, uint32_t pid, uint32_t tid);
    uint32_t maximum(size_t row) const;
    void cleanup(size_t row, const std::function<bool(uint32_t)> &alive);
    bool emptyAfterCleanup(size_t row, const std::function<bool(uint32_t)> &alive);

  private:
    std::span<uint8_t> storage_;
    void setEntry(size_t row, size_t index, std::array<uint32_t, 3> values);
};
class MetricPriorityState {
  public:
    MetricPriorityState(MetricPriorityTable &table, size_t row, std::optional<size_t> globalRow, uint32_t pid,
                        uint32_t tid, uint32_t priority, bool globalStrategy = false);
    void setPriority(uint32_t value);
    bool eligible() const;
    uint32_t highest() const;
    bool canGrant() const;
    bool owned() const;
    bool tryAcquire(const std::function<bool(uint32_t)> &alive);
    void release();
    void unregister();
    uint32_t priority() const { return priority_; }
    size_t row() const { return row_; }

  private:
    MetricPriorityTable &table_;
    size_t row_;
    std::optional<size_t> global_;
    uint32_t pid_, tid_, priority_;
    bool globalStrategy_;
};
} // namespace flora
