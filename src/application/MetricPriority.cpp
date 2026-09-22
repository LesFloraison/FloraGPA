#include "MetricPriority.h"
#include <algorithm>
#include <cstring>
#include <stdexcept>
namespace flora {
namespace {
constexpr uint64_t magic = 0x7072696d75746578ULL, version = 2;
template <class T> T read(std::span<const uint8_t> bytes, size_t offset) {
    T result;
    std::memcpy(&result, bytes.data() + offset, sizeof result);
    return result;
}
template <class T> void write(std::span<uint8_t> bytes, size_t offset, T value) {
    std::memcpy(bytes.data() + offset, &value, sizeof value);
}
} // namespace
int64_t signedPriority(uint32_t value) { return value < 0x80000000 ? value : int64_t(value) - 0x100000000LL; }
std::string metricResourceName(uint32_t kind, uint32_t low, uint32_t high, const std::string &suffix) {
    std::string result;
    if (kind && low) {
        constexpr char digits[] = "0123456789abcdef";
        for (const auto part : {low, high})
            for (int byte = 0; byte < 4; ++byte) {
                const auto value = uint8_t(part >> (byte * 8));
                result += digits[value >> 4];
                result += digits[value & 15];
            }
    }
    for (const unsigned char c : suffix) {
        if (c > 127)
            throw std::invalid_argument("Resource suffix must be ASCII");
        result += char(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
    }
    return result;
}
MetricPriorityTable::MetricPriorityTable(std::span<uint8_t> storage) : storage_(storage) {
    if (storage.size() != priorityTableSize || read<uint64_t>(storage, 0) != magic ||
        read<uint64_t>(storage, 8) != version)
        throw std::invalid_argument("Invalid priority table size, magic or version");
}
std::vector<uint8_t> MetricPriorityTable::initial() {
    std::vector<uint8_t> result(priorityTableSize, 0xff);
    write(result, 0, magic);
    write(result, 8, version);
    for (size_t row = 0; row < priorityRows; ++row)
        std::fill_n(result.begin() + 16 + row * priorityRowSize, 128, uint8_t(0));
    return result;
}
size_t MetricPriorityTable::offset(size_t row) const {
    if (row >= priorityRows)
        throw std::invalid_argument("Priority resource row outside table");
    return 16 + row * priorityRowSize;
}
std::string MetricPriorityTable::name(size_t row) const {
    const auto data = storage_.subspan(offset(row), 128);
    const auto end = std::find(data.begin(), data.end(), uint8_t(0));
    if (end == data.end())
        throw std::invalid_argument("Unterminated resource name");
    return {reinterpret_cast<const char *>(data.data()), size_t(end - data.begin())};
}
size_t MetricPriorityTable::allocate(const std::string &value) {
    if (value.empty() || value.size() >= 128 ||
        std::any_of(value.begin(), value.end(), [](unsigned char c) { return c == 0 || c > 127; }))
        throw std::invalid_argument("Resource name must contain 1..127 nonzero ASCII bytes");
    for (size_t row = 0; row < priorityRows; ++row) {
        const auto current = name(row);
        if (current.empty() || current == value) {
            std::memcpy(storage_.data() + offset(row), value.data(), value.size());
            return row;
        }
    }
    throw std::overflow_error("Priority resource table is full");
}
uint32_t MetricPriorityTable::get(size_t row, size_t field) const {
    if (field != 0x80 && field != 0x84 && field != 0x88)
        throw std::invalid_argument("Invalid row field");
    return read<uint32_t>(storage_, offset(row) + field);
}
void MetricPriorityTable::put(size_t row, size_t field, uint32_t value) {
    if (field != 0x80 && field != 0x84 && field != 0x88)
        throw std::invalid_argument("Invalid row field");
    write(storage_, offset(row) + field, value);
}
std::array<uint32_t, 3> MetricPriorityTable::entry(size_t row, size_t index) const {
    if (index >= priorityEntries)
        throw std::out_of_range("Priority entry outside table");
    std::array<uint32_t, 3> result;
    std::memcpy(result.data(), storage_.data() + offset(row) + 0x8c + index * 12, 12);
    return result;
}
void MetricPriorityTable::setEntry(size_t row, size_t index, std::array<uint32_t, 3> values) {
    if (index >= priorityEntries)
        throw std::out_of_range("Priority entry outside table");
    std::memcpy(storage_.data() + offset(row) + 0x8c + index * 12, values.data(), 12);
}
void MetricPriorityTable::insert(size_t row, uint32_t pid, uint32_t tid, uint32_t priority) {
    if (pid == priorityEmpty)
        throw std::invalid_argument("Reserved process ID");
    size_t first = priorityEntries;
    for (size_t i = 0; i < priorityEntries; ++i) {
        const auto e = entry(row, i);
        if (e[0] == pid && e[1] == tid)
            throw std::invalid_argument("Duplicate process/thread entry");
        if (e[0] == priorityEmpty && first == priorityEntries)
            first = i;
    }
    if (first == priorityEntries)
        throw std::overflow_error("Priority entry table is full");
    setEntry(row, first, {pid, tid, priority});
}
void MetricPriorityTable::update(size_t row, uint32_t pid, uint32_t tid, uint32_t priority) {
    for (size_t i = 0; i < priorityEntries; ++i) {
        const auto e = entry(row, i);
        if (e[0] == pid && e[1] == tid) {
            setEntry(row, i, {pid, tid, priority});
            return;
        }
    }
    throw std::invalid_argument("Missing process/thread entry");
}
void MetricPriorityTable::remove(size_t row, uint32_t pid, uint32_t tid) {
    for (size_t i = 0; i < priorityEntries; ++i) {
        const auto e = entry(row, i);
        if (e[0] == pid && e[1] == tid) {
            setEntry(row, i, {priorityEmpty, priorityEmpty, priorityEmpty});
            return;
        }
    }
}
uint32_t MetricPriorityTable::maximum(size_t row) const {
    int64_t maximum = -1;
    for (size_t i = 0; i < priorityEntries; ++i) {
        const auto e = entry(row, i);
        if (e[0] != priorityEmpty)
            maximum = std::max(maximum, signedPriority(e[2]));
    }
    return uint32_t(maximum);
}
void MetricPriorityTable::cleanup(size_t row, const std::function<bool(uint32_t)> &alive) {
    for (size_t i = 0; i < priorityEntries; ++i) {
        const auto e = entry(row, i);
        if (e[0] != priorityEmpty && !alive(e[0]))
            setEntry(row, i, {priorityEmpty, priorityEmpty, priorityEmpty});
    }
    // The recovered protocol intentionally leaves owner TID and cached maximum stale.
    if (!alive(get(row, 0x84)))
        put(row, 0x84, priorityEmpty);
}
bool MetricPriorityTable::emptyAfterCleanup(size_t row, const std::function<bool(uint32_t)> &alive) {
    cleanup(row, alive);
    for (size_t i = 0; i < priorityEntries; ++i)
        if (entry(row, i)[0] != priorityEmpty)
            return false;
    return true;
}
MetricPriorityState::MetricPriorityState(MetricPriorityTable &table, size_t row,
                                         std::optional<size_t> globalRow, uint32_t pid, uint32_t tid,
                                         uint32_t priority, bool globalStrategy)
    : table_(table), row_(row), global_(globalRow), pid_(pid), tid_(tid), priority_(priority),
      globalStrategy_(globalStrategy) {}
void MetricPriorityState::setPriority(uint32_t value) {
    table_.update(row_, pid_, tid_, value);
    priority_ = value;
    table_.put(row_, 0x80, table_.maximum(row_));
}
bool MetricPriorityState::eligible() const {
    return priority_ == table_.get(row_, 0x80) &&
           signedPriority(table_.get(global_.value_or(priorityRows), 0x80)) <= signedPriority(priority_);
}
uint32_t MetricPriorityState::highest() const {
    const auto a = table_.get(row_, 0x80), b = table_.get(global_.value_or(priorityRows), 0x80);
    return signedPriority(a) >= signedPriority(b) ? a : b;
}
bool MetricPriorityState::canGrant() const {
    if (priority_ != table_.get(row_, 0x80))
        return false;
    if (globalStrategy_) {
        for (size_t r = 0; r < priorityRows; ++r)
            if (table_.get(r, 0x84) != priorityEmpty && table_.name(r) != "UI")
                return false;
        return true;
    }
    return table_.get(row_, 0x84) == priorityEmpty &&
           (!global_ || table_.get(*global_, 0x84) == priorityEmpty);
}
bool MetricPriorityState::owned() const {
    return table_.get(row_, 0x84) == pid_ && table_.get(row_, 0x88) == tid_;
}
bool MetricPriorityState::tryAcquire(const std::function<bool(uint32_t)> &alive) {
    table_.cleanup(row_, alive);
    if (canGrant()) {
        table_.put(row_, 0x84, pid_);
        table_.put(row_, 0x88, tid_);
    }
    return owned();
}
void MetricPriorityState::release() {
    if (owned()) {
        table_.put(row_, 0x84, priorityEmpty);
        table_.put(row_, 0x88, priorityEmpty);
    }
}
void MetricPriorityState::unregister() {
    release();
    table_.remove(row_, pid_, tid_);
    table_.put(row_, 0x80, table_.maximum(row_));
}
} // namespace flora
