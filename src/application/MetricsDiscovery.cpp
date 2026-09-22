#include "MetricsDiscovery.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLibrary>
#include <psapi.h>
#include <set>
namespace flora {
using Json = nlohmann::json;
namespace {
uint64_t token(uint64_t value) {
    if (!value)
        throw std::runtime_error("Sample token must be a positive uint64");
    return value;
}
bool natural(const Json &v) {
    return v.is_number_integer() && (v.is_number_unsigned() || v.get<int64_t>() >= 0);
}
} // namespace
QString metricsDriverLibrary() {
    const auto root = QFileInfo(qEnvironmentVariable("SystemRoot") + "/System32/DriverStore/FileRepository")
                          .canonicalFilePath();
    std::vector<HMODULE> modules(256);
    DWORD needed = 0;
    for (;;) {
        if (!EnumProcessModules(GetCurrentProcess(), modules.data(), DWORD(modules.size() * sizeof(HMODULE)),
                                &needed))
            throw std::runtime_error("Cannot enumerate loaded driver modules");
        if (needed <= modules.size() * sizeof(HMODULE))
            break;
        modules.resize(needed / sizeof(HMODULE));
    }
    std::set<QString> candidates;
    for (size_t i = 0; i < needed / sizeof(HMODULE); ++i) {
        wchar_t path[32768]{};
        const auto count = GetModuleFileNameW(modules[i], path, 32768);
        if (!count || count >= 32768)
            continue;
        const QFileInfo loaded(QString::fromWCharArray(path, int(count)));
        if (loaded.fileName().compare("igd10iumd64.dll", Qt::CaseInsensitive))
            continue;
        const auto candidate = QFileInfo(loaded.canonicalPath() + "/igdmd64.dll").canonicalFilePath();
        if (!root.isEmpty() && !candidate.isEmpty() && candidate.startsWith(root + '/', Qt::CaseInsensitive))
            candidates.insert(candidate);
    }
    if (candidates.size() != 1)
        throw std::runtime_error(
            "Cannot identify one Metrics Discovery library from the loaded Intel DX11 driver");
    return *candidates.begin();
}
struct MetricsDiscovery::Impl {
    QLibrary library;
    QString driver, bridge;
    void *handle{};
    Json catalog, selected;
    template <class R, class... Args> R call(const char *name, Args... args) {
        const auto symbol = library.resolve((QByteArray("FloraMd") + name).constData());
        if (!symbol)
            throw std::runtime_error(std::string("Missing Metrics Discovery bridge export: ") + name);
        return reinterpret_cast<R (*)(Args...)>(symbol)(args...);
    }
    bool supports(std::initializer_list<const char *> names) {
        for (auto name : names)
            if (!library.resolve((QByteArray("FloraMd") + name).constData()))
                return false;
        return true;
    }
    void open() const {
        if (!handle)
            throw std::runtime_error("Metrics device is closed");
    }
    void require(bool supported, const char *feature) const {
        open();
        if (!supported)
            throw std::runtime_error(std::string("This bridge does not support ") + feature);
    }
    [[noreturn]] void error() {
        const auto message = call<const char *>("Error");
        throw std::runtime_error(message ? message : "Metrics Discovery bridge failed");
    }
    void checked(int status) {
        if (status)
            error();
    }
    Json json(const char *text) {
        if (!text)
            error();
        return Json::parse(text);
    }
    MetricResult parse(const char *text) {
        auto value = json(text);
        const auto hex = value.at("raw_hex").get<std::string>();
        if (hex.size() % 2)
            throw std::runtime_error("Invalid raw counter hexadecimal data");
        const auto digit = [](char ch) -> unsigned {
            if (ch >= '0' && ch <= '9')
                return ch - '0';
            if (ch >= 'a' && ch <= 'f')
                return ch - 'a' + 10;
            if (ch >= 'A' && ch <= 'F')
                return ch - 'A' + 10;
            throw std::runtime_error("Invalid raw counter hexadecimal data");
        };
        std::vector<uint8_t> raw(hex.size() / 2);
        for (size_t i = 0; i < raw.size(); ++i)
            raw[i] = uint8_t(digit(hex[2 * i]) * 16 + digit(hex[2 * i + 1]));
        value.erase("raw_hex");
        return {validateMetricResult(selected, std::move(value), raw), std::move(raw)};
    }
    ~Impl() {
        if (handle)
            call<void>("Close", handle);
    }
};
MetricsDiscovery::MetricsDiscovery(ID3D11Device *device, const QString &bridge)
    : impl_(std::make_unique<Impl>()) {
    auto &p = *impl_;
    p.driver = metricsDriverLibrary();
    p.bridge = QFileInfo(bridge.isEmpty() ? QCoreApplication::applicationDirPath() + "/FloraGPA.Metrics.dll"
                                          : bridge)
                   .absoluteFilePath();
    p.library.setFileName(p.bridge);
    if (!p.library.load())
        throw std::runtime_error(p.library.errorString().toStdString());
    if (!p.supports({"Open", "Close", "Error", "Catalog", "Result", "Select", "Begin", "End", "Decode"}))
        throw std::runtime_error("Incomplete Metrics Discovery bridge");
    p.handle = p.call<void *>("Open", static_cast<void *>(device),
                              reinterpret_cast<const wchar_t *>(p.driver.utf16()));
    if (!p.handle)
        p.error();
    p.catalog = annotateMetricCatalog(p.json(p.call<const char *>("Catalog", p.handle)));
}
MetricsDiscovery::~MetricsDiscovery() = default;
const Json &MetricsDiscovery::catalog() const { return impl_->catalog; }
const Json &MetricsDiscovery::selected() const { return impl_->selected; }
void MetricsDiscovery::close() {
    if (impl_->handle) {
        impl_->call<void>("Close", impl_->handle);
        impl_->handle = nullptr;
    }
}
void MetricsDiscovery::select(const std::string &name) {
    impl_->open();
    Json chosen;
    size_t count = 0;
    for (const auto &set : catalog().at("sets"))
        if (set.at("name") == name) {
            chosen = set;
            ++count;
        }
    if (count != 1)
        throw std::runtime_error("Unknown or ambiguous hardware metric set: " + name);
    impl_->checked(impl_->call<int>("Select", impl_->handle, chosen.at("group").get<unsigned>(),
                                    chosen.at("index").get<unsigned>()));
    impl_->selected = std::move(chosen);
}
void MetricsDiscovery::begin() {
    impl_->open();
    impl_->checked(impl_->call<int>("Begin", impl_->handle));
}
MetricResult MetricsDiscovery::end(unsigned timeout) {
    if (!timeout || timeout > 60000)
        throw std::runtime_error("Counter timeout must be 1..60000 ms");
    impl_->open();
    impl_->checked(impl_->call<int>("End", impl_->handle, timeout));
    return result();
}
MetricResult MetricsDiscovery::result() {
    impl_->open();
    return impl_->parse(impl_->call<const char *>("Result", impl_->handle));
}
MetricResult MetricsDiscovery::decode(Bytes raw) {
    if (selected().is_null() || raw.size() != selected().at("report_size").get<size_t>())
        throw std::runtime_error("Raw counter report must match selected set size");
    impl_->open();
    impl_->checked(impl_->call<int>("Decode", impl_->handle, static_cast<const void *>(raw.data()),
                                    unsigned(raw.size())));
    return result();
}
Json MetricsDiscovery::clockPair() {
    impl_->require(impl_->supports({"Clock"}), "publisher clock calibration");
    auto value = impl_->json(impl_->call<const char *>("Clock", impl_->handle));
    for (const auto key : {"status", "gpu_ns", "cpu_ns", "cpu_id", "maximum_ns", "frequency_hz"})
        if (!value.contains(key) || !natural(value[key]))
            throw std::runtime_error(std::string("Invalid driver clock field: ") + key);
    if (value["maximum_ns"] == 0 || value["frequency_hz"] == 0)
        throw std::runtime_error("Invalid driver clock metadata");
    return value;
}
bool MetricsDiscovery::supportsDrain() const { return impl_->supports({"Submit", "Poll", "Discard"}); }
void MetricsDiscovery::submit() {
    impl_->require(supportsDrain(), "split counter draining");
    impl_->checked(impl_->call<int>("Submit", impl_->handle));
}
std::optional<MetricResult> MetricsDiscovery::poll(bool flush) {
    impl_->require(supportsDrain(), "split counter draining");
    const auto status = impl_->call<int>("Poll", impl_->handle, int(flush));
    if (status < 0)
        impl_->error();
    if (!status)
        return {};
    return result();
}
void MetricsDiscovery::discard() {
    impl_->require(supportsDrain(), "split counter draining");
    impl_->checked(impl_->call<int>("Discard", impl_->handle));
}
bool MetricsDiscovery::supportsSamples() const {
    return impl_->supports(
        {"SampleBegin", "SampleSubmit", "SamplePoll", "SampleResult", "SampleRelease", "SampleCount"});
}
uint64_t MetricsDiscovery::sampleBegin() {
    impl_->require(supportsSamples(), "independent counter samples");
    const auto value = impl_->call<uint64_t>("SampleBegin", impl_->handle);
    if (!value)
        impl_->error();
    return value;
}
void MetricsDiscovery::sampleSubmit(uint64_t id) {
    impl_->require(supportsSamples(), "independent counter samples");
    impl_->checked(impl_->call<int>("SampleSubmit", impl_->handle, token(id)));
}
std::optional<MetricResult> MetricsDiscovery::samplePoll(uint64_t id, bool flush) {
    impl_->require(supportsSamples(), "independent counter samples");
    const auto status = impl_->call<int>("SamplePoll", impl_->handle, token(id), int(flush));
    if (status < 0)
        impl_->error();
    if (!status)
        return {};
    return sampleResult(id);
}
MetricResult MetricsDiscovery::sampleResult(uint64_t id) {
    impl_->require(supportsSamples(), "independent counter samples");
    return impl_->parse(impl_->call<const char *>("SampleResult", impl_->handle, token(id)));
}
void MetricsDiscovery::sampleRelease(uint64_t id) {
    impl_->require(supportsSamples(), "independent counter samples");
    impl_->checked(impl_->call<int>("SampleRelease", impl_->handle, token(id)));
}
unsigned MetricsDiscovery::sampleCount() {
    impl_->require(supportsSamples(), "independent counter samples");
    const auto count = impl_->call<int>("SampleCount", impl_->handle);
    if (count < 0)
        impl_->error();
    return unsigned(count);
}
bool MetricsDiscovery::supportsReuse() const {
    return supportsSamples() && impl_->supports({"SampleReserve", "SampleRecycle", "SampleClearCache",
                                                 "SampleInfo", "SampleStats"});
}
void MetricsDiscovery::sampleReserve(unsigned count) {
    if (!count || count > 256)
        throw std::runtime_error("Reserve count must be 1..256");
    impl_->require(supportsReuse(), "counter object reuse");
    impl_->checked(impl_->call<int>("SampleReserve", impl_->handle, count));
}
void MetricsDiscovery::sampleRecycle(uint64_t id) {
    impl_->require(supportsReuse(), "counter object reuse");
    impl_->checked(impl_->call<int>("SampleRecycle", impl_->handle, token(id)));
}
void MetricsDiscovery::sampleClearCache() {
    impl_->require(supportsReuse(), "counter object reuse");
    impl_->checked(impl_->call<int>("SampleClearCache", impl_->handle));
}
uint64_t MetricsDiscovery::sampleInfo(uint64_t id) {
    impl_->require(supportsReuse(), "counter object reuse");
    const auto value = impl_->call<uint64_t>("SampleInfo", impl_->handle, token(id));
    if (!value)
        impl_->error();
    return value;
}
Json MetricsDiscovery::sampleStats() {
    impl_->require(supportsReuse(), "counter object reuse");
    auto value = impl_->json(impl_->call<const char *>("SampleStats", impl_->handle));
    for (auto key : {"created", "reused", "cached", "owned"})
        if (!value.contains(key) || !natural(value[key]))
            throw std::runtime_error("Invalid counter pool statistics");
    return value;
}
bool MetricsDiscovery::supportsRecorded() const {
    return impl_->supports({"RecordedBegin", "RecordedEnd", "RecordedExecute", "RecordedPoll",
                            "RecordedResult", "RecordedRelease", "RecordedCount"});
}
uint64_t MetricsDiscovery::recordedBegin(ID3D11DeviceContext *context) {
    impl_->require(supportsRecorded(), "recorded counters");
    token(reinterpret_cast<uintptr_t>(context));
    const auto value = impl_->call<uint64_t>("RecordedBegin", impl_->handle, static_cast<void *>(context));
    if (!value)
        impl_->error();
    return value;
}
void MetricsDiscovery::recordedEnd(uint64_t id) {
    impl_->require(supportsRecorded(), "recorded counters");
    impl_->checked(impl_->call<int>("RecordedEnd", impl_->handle, token(id)));
}
uint64_t MetricsDiscovery::recordedExecute(ID3D11CommandList *command, std::span<const uint64_t> tokens,
                                           bool restore) {
    impl_->require(supportsRecorded(), "recorded counters");
    for (auto id : tokens)
        token(id);
    if (tokens.empty() || tokens.size() > 256 ||
        std::set<uint64_t>(tokens.begin(), tokens.end()).size() != tokens.size())
        throw std::runtime_error("Execution requires 1..256 unique recorded tokens");
    token(reinterpret_cast<uintptr_t>(command));
    const auto value = impl_->call<uint64_t>("RecordedExecute", impl_->handle, static_cast<void *>(command),
                                             tokens.data(), unsigned(tokens.size()), int(restore));
    if (!value)
        impl_->error();
    return value;
}
std::optional<MetricResult> MetricsDiscovery::recordedPoll(uint64_t id, uint64_t execution, bool flush) {
    impl_->require(supportsRecorded(), "recorded counters");
    const auto status =
        impl_->call<int>("RecordedPoll", impl_->handle, token(id), token(execution), int(flush));
    if (status < 0)
        impl_->error();
    if (!status)
        return {};
    return recordedResult(id, execution);
}
MetricResult MetricsDiscovery::recordedResult(uint64_t id, uint64_t execution) {
    impl_->require(supportsRecorded(), "recorded counters");
    return impl_->parse(
        impl_->call<const char *>("RecordedResult", impl_->handle, token(id), token(execution)));
}
void MetricsDiscovery::recordedRelease(uint64_t id) {
    impl_->require(supportsRecorded(), "recorded counters");
    impl_->checked(impl_->call<int>("RecordedRelease", impl_->handle, token(id)));
}
unsigned MetricsDiscovery::recordedCount() {
    impl_->require(supportsRecorded(), "recorded counters");
    const auto count = impl_->call<int>("RecordedCount", impl_->handle);
    if (count < 0)
        impl_->error();
    return unsigned(count);
}
Json MetricsDiscovery::provenance() const {
    Json value;
    for (const auto &[name, path] :
         {std::pair{"bridge", impl_->bridge}, std::pair{"driver", impl_->driver}}) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            throw std::runtime_error(file.errorString().toStdString());
        QCryptographicHash hash(QCryptographicHash::Sha256);
        if (!hash.addData(&file))
            throw std::runtime_error("Cannot hash Metrics Discovery library");
        value[name] = {{"path", path.toStdString()}, {"sha256", hash.result().toHex().toStdString()}};
    }
    return value;
}
} // namespace flora
