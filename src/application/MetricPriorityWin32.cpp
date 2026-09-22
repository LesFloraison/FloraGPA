#include "MetricPriorityWin32.h"
#include <QDir>
#include <QFileInfo>
#define NOMINMAX
#include <ShlObj.h>
#include <Windows.h>
#include <chrono>
#include <cmath>
#include <system_error>
#include <thread>
namespace flora {
namespace {
constexpr wchar_t mutexName[] = L"PMMutex_{C6C9F37E-C70B-4172-BE47-D83F44656DF9}";
constexpr char fileName[] = "PMSharedMemory_{C6C9F37E-C70B-4172-BE47-D83F44656DF9}";
[[noreturn]] void windowsError(const char *operation) {
    throw std::system_error(int(GetLastError()), std::system_category(), operation);
}
uint32_t uint32(const nlohmann::json &value) {
    if (!value.is_number_integer() ||
        (value.is_number_unsigned() ? value.get<uint64_t>() > UINT32_MAX
                                    : value.get<int64_t>() < 0 || value.get<int64_t>() > UINT32_MAX))
        throw std::invalid_argument("Expected uint32");
    return value.get<uint32_t>();
}
} // namespace
QString metricPriorityDefaultPath() {
    PWSTR folder{};
    const auto status = SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &folder);
    if (FAILED(status))
        throw std::runtime_error("Cannot resolve Documents folder: " + std::to_string(status));
    const auto path = QDir(QString::fromWCharArray(folder)).filePath(QString("GPA/") + fileName);
    CoTaskMemFree(folder);
    return QDir::toNativeSeparators(path);
}
bool metricPriorityProcessAlive(uint32_t pid) {
    const auto handle = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!handle)
        return false;
    const auto alive = WaitForSingleObject(handle, 0) == WAIT_TIMEOUT;
    CloseHandle(handle);
    return alive;
}
struct SharedMetricPriorityMutex::State {
    std::string resource;
    QString path, mutex;
    double timeout{};
    DWORD guardTimeout{}, pid{GetCurrentProcessId()}, tid{GetCurrentThreadId()};
    HANDLE guardHandle{}, file{INVALID_HANDLE_VALUE}, mapping{};
    uint8_t *view{};
    std::unique_ptr<MetricPriorityTable> table;
    std::unique_ptr<MetricPriorityState> priority;
    bool closed{}, deleted{};
    int depth{};
    uint64_t attempts{}, timeouts{}, abandoned{};
    nlohmann::json priorities = nlohmann::json::array(), deleteError;
    ~State() {
        closeStorage();
        if (guardHandle)
            CloseHandle(guardHandle);
    }
    void thread() const {
        if (closed)
            throw std::invalid_argument("Priority mutex is closed");
        if (pid != GetCurrentProcessId() || tid != GetCurrentThreadId())
            throw std::runtime_error("Priority mutex must be used by its registering thread");
    }
    struct Guard {
        State &s;
        Guard(State &state, DWORD timeout) : s(state) {
            s.thread();
            const auto result = WaitForSingleObject(s.guardHandle, timeout);
            if (result == WAIT_TIMEOUT)
                throw MetricPriorityTimeout("Shared priority table mutex is busy");
            if (result != WAIT_OBJECT_0 && result != WAIT_ABANDONED)
                windowsError("WaitForSingleObject");
            if (result == WAIT_ABANDONED)
                ++s.abandoned;
        }
        ~Guard() { ReleaseMutex(s.guardHandle); }
    };
    void closeStorage() {
        if (view) {
            UnmapViewOfFile(view);
            view = nullptr;
        }
        if (mapping) {
            CloseHandle(mapping);
            mapping = nullptr;
        }
        if (file != INVALID_HANDLE_VALUE) {
            CloseHandle(file);
            file = INVALID_HANDLE_VALUE;
        }
    }
};
SharedMetricPriorityMutex::SharedMetricPriorityMutex(std::string resource, MetricPriorityOptions options)
    : state_(std::make_unique<State>()) {
    auto &s = *state_;
    if (!(options.timeout >= 0 && options.timeout <= 60))
        throw std::invalid_argument("Invalid acquisition timeout");
    if (!(options.guardTimeout >= 0 && options.guardTimeout <= 60))
        throw std::invalid_argument("Invalid guard timeout");
    if (options.path.has_value() != options.mutexName.has_value())
        throw std::invalid_argument("Test storage requires both path and mutex name");
    s.resource = std::move(resource);
    s.path = options.path ? QDir::toNativeSeparators(QFileInfo(*options.path).absoluteFilePath())
                          : metricPriorityDefaultPath();
    s.mutex = options.mutexName && !options.mutexName->isEmpty() ? *options.mutexName
                                                                 : QString::fromWCharArray(mutexName);
    s.timeout = options.timeout;
    s.guardTimeout = DWORD(options.guardTimeout * 1000);
    s.guardHandle = CreateMutexW(nullptr, FALSE, reinterpret_cast<LPCWSTR>(s.mutex.utf16()));
    if (!s.guardHandle)
        windowsError("CreateMutexW");
    State::Guard guard(s, s.guardTimeout);
    if (!QDir().mkpath(QFileInfo(s.path).absolutePath()))
        throw std::runtime_error("Cannot create priority table directory");
    s.file = CreateFileW(reinterpret_cast<LPCWSTR>(s.path.utf16()), GENERIC_READ | GENERIC_WRITE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
    if (s.file == INVALID_HANDLE_VALUE)
        windowsError("CreateFileW");
    const auto existing = GetLastError() == ERROR_ALREADY_EXISTS;
    if (!existing) {
        const auto initial = MetricPriorityTable::initial();
        DWORD written{};
        if (!WriteFile(s.file, initial.data(), DWORD(initial.size()), &written, nullptr))
            windowsError("WriteFile");
        if (written != initial.size())
            throw std::runtime_error("Incomplete priority table initialization");
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(s.file, &size))
        windowsError("GetFileSizeEx");
    if (size.QuadPart != priorityTableSize)
        throw std::invalid_argument("Invalid priority table size");
    s.mapping = CreateFileMappingW(s.file, nullptr, PAGE_READWRITE, 0, 0, nullptr);
    if (!s.mapping)
        windowsError("CreateFileMappingW");
    s.view = static_cast<uint8_t *>(MapViewOfFile(s.mapping, FILE_MAP_ALL_ACCESS, 0, 0, priorityTableSize));
    if (!s.view)
        windowsError("MapViewOfFile");
    s.table = std::make_unique<MetricPriorityTable>(std::span(s.view, priorityTableSize));
    const auto global = s.table->allocate("GLOBAL"), row = s.table->allocate(s.resource);
    s.table->cleanup(row, metricPriorityProcessAlive);
    s.table->insert(row, s.pid, s.tid, priorityEmpty);
    s.table->put(row, 0x80, s.table->maximum(row));
    s.priority = std::make_unique<MetricPriorityState>(*s.table, row, global, s.pid, s.tid, priorityEmpty,
                                                       s.resource == "GLOBAL");
}
SharedMetricPriorityMutex::~SharedMetricPriorityMutex() {
    try {
        close();
    } catch (...) {
    }
}
void SharedMetricPriorityMutex::setPriority(uint32_t value) {
    auto &s = *state_;
    State::Guard guard(s, s.guardTimeout);
    s.priority->setPriority(value);
    s.priorities.push_back(value);
}
bool SharedMetricPriorityMutex::tryAcquire() {
    auto &s = *state_;
    ++s.attempts;
    State::Guard guard(s, 0);
    const auto acquired = s.priority->tryAcquire(metricPriorityProcessAlive);
    s.depth = int(acquired);
    return acquired;
}
bool SharedMetricPriorityMutex::acquire() {
    auto &s = *state_;
    s.thread();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(s.timeout);
    for (;;) {
        try {
            if (tryAcquire())
                return true;
        } catch (const MetricPriorityTimeout &) {
        }
        const auto remaining =
            std::chrono::duration<double>(deadline - std::chrono::steady_clock::now()).count();
        if (remaining <= 0) {
            ++s.timeouts;
            setPriority(priorityEmpty);
            throw MetricPriorityTimeout("Priority resource acquisition timed out: " + s.resource);
        }
        std::this_thread::sleep_for(std::chrono::duration<double>(std::min(.1, remaining)));
    }
}
void SharedMetricPriorityMutex::release() {
    auto &s = *state_;
    State::Guard guard(s, s.guardTimeout);
    s.priority->release();
    s.depth = 0;
}
void SharedMetricPriorityMutex::close() {
    auto &s = *state_;
    if (s.closed)
        return;
    {
        State::Guard guard(s, s.guardTimeout);
        s.priority->unregister();
        s.depth = 0;
        bool empty = true;
        for (size_t row = 0; row < priorityRows; ++row)
            if (!s.table->emptyAfterCleanup(row, metricPriorityProcessAlive)) {
                empty = false;
                break;
            }
        s.closeStorage();
        if (empty) {
            if (DeleteFileW(reinterpret_cast<LPCWSTR>(s.path.utf16())))
                s.deleted = true;
            else
                s.deleteError =
                    std::system_error(int(GetLastError()), std::system_category(), "DeleteFileW").what();
        }
    }
    CloseHandle(s.guardHandle);
    s.guardHandle = nullptr;
    s.closed = true;
}
bool SharedMetricPriorityMutex::closed() const { return state_->closed; }
bool SharedMetricPriorityMutex::eligible() {
    auto &s = *state_;
    State::Guard guard(s, s.guardTimeout);
    return s.priority->eligible();
}
uint32_t SharedMetricPriorityMutex::highest() {
    auto &s = *state_;
    State::Guard guard(s, s.guardTimeout);
    return s.priority->highest();
}
nlohmann::json SharedMetricPriorityMutex::audit() const {
    const auto &s = *state_;
    return {{"resource", s.resource},
            {"path", s.path.toStdString()},
            {"mutex", s.mutex.toStdString()},
            {"pid", s.pid},
            {"tid", s.tid},
            {"closed", s.closed},
            {"depth", s.depth},
            {"priority", s.priority ? nlohmann::json(s.priority->priority()) : nlohmann::json()},
            {"priorities", s.priorities},
            {"attempts", s.attempts},
            {"timeouts", s.timeouts},
            {"abandoned_guards", s.abandoned},
            {"deleted_empty_storage", s.deleted},
            {"delete_error", s.deleteError},
            {"cross_process", true},
            {"production_gpa_dependency", false},
            {"acquisition_failure_policy", "Raise and withdraw priority; no GPU work after timeout"}};
}
std::unique_ptr<SharedMetricPriorityMutex> metricDeviceMutex(const nlohmann::json &catalog,
                                                             MetricPriorityOptions options) {
    if (catalog.at("vendor") != 0x8086)
        throw std::invalid_argument("This collector requires an Intel adapter");
    const auto &luid = catalog.at("luid");
    const auto high =
        luid.at(1).is_number_unsigned() ? luid.at(1).get<uint64_t>() : uint64_t(luid.at(1).get<int64_t>());
    return std::make_unique<SharedMetricPriorityMutex>(
        metricResourceName(1, uint32(luid.at(0)), uint32_t(high), "OA"), std::move(options));
}
} // namespace flora
