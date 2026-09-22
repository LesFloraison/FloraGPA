// Public Intel MD API + native D3D11 counters; no GPA runtime dependency.
#include "metrics_discovery_api.h"
#include <cmath>
#include <d3d11.h>
#include <dxgi.h>
#include <iomanip>
#include <limits>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <vector>
#include <windows.h>
using namespace MetricsDiscovery;
static thread_local std::string lastError;
static void require(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
static void hr(HRESULT value, const char *message) {
    if (FAILED(value)) {
        std::ostringstream s;
        s << message << " HRESULT 0x" << std::hex << unsigned(value);
        throw std::runtime_error(s.str());
    }
}
static void md(TCompletionCode value, const char *message) {
    if (value != CC_OK)
        throw std::runtime_error(std::string(message) + " MD status " + std::to_string(value));
}
static std::string q(const char *text) {
    std::ostringstream s;
    s << '"';
    if (text)
        for (unsigned char ch : std::string(text)) {
            if (ch == '"' || ch == '\\')
                s << '\\' << ch;
            else if (ch < 32)
                s << "\\u00" << std::hex << std::setw(2) << std::setfill('0') << unsigned(ch) << std::dec;
            else
                s << ch;
        }
    s << '"';
    return s.str();
}
struct Metrics {
    struct Sample {
        ID3D11DeviceContext *context;
        ID3D11Counter *counter = nullptr;
        bool begun = false;
        std::string result;
        uint64_t id = 0, uses = 0;
        explicit Sample(ID3D11DeviceContext *c) : context(c) {}
        ~Sample() {
            if (counter) {
                if (begun)
                    context->End(counter);
                counter->Release();
            }
        }
    };
    struct Recorded {
        std::unique_ptr<Sample> sample;
        ID3D11DeviceContext *context;
        bool recording = false, pending = false;
        uint64_t execution = 0;
        Recorded(std::unique_ptr<Sample> s, ID3D11DeviceContext *c) : sample(std::move(s)), context(c) {
            context->AddRef();
        }
        ~Recorded() {
            if (recording)
                context->End(sample->counter);
            sample.reset();
            context->Release();
        }
    };
    HMODULE library = nullptr;
    IAdapterGroup_1_6 *group = nullptr;
    IAdapter_1_6 *adapter = nullptr;
    IMetricsDevice_1_5 *metrics = nullptr;
    ID3D11Device *device = nullptr;
    ID3D11DeviceContext *context = nullptr;
    ID3D11Counter *counter = nullptr;
    IMetricSet_1_5 *selected = nullptr;
    bool begun = false;
    std::string catalog, result, clockResult;
    std::map<uint64_t, std::unique_ptr<Sample>> samples;
    uint64_t nextSample = 1, activeSample = 0;
    std::map<uint64_t, std::unique_ptr<Recorded>> recorded;
    uint64_t nextExecution = 1;
    std::vector<std::unique_ptr<Sample>> cached;
    uint64_t created = 0, reused = 0;
    std::string statsResult;
    ~Metrics() {
        recorded.clear();
        samples.clear();
        cached.clear();
        if (counter) {
            if (begun)
                context->End(counter);
            counter->Release();
        }
        if (context)
            context->Release();
        if (device)
            device->Release();
        if (metrics)
            adapter->CloseMetricsDevice(metrics);
        if (group)
            group->Close();
        if (library)
            FreeLibrary(library);
    }
    void open(ID3D11Device *supplied, const wchar_t *path) {
        require(supplied && path, "Device and driver path required");
        device = supplied;
        device->AddRef();
        device->GetImmediateContext(&context);
        IDXGIDevice *dxgi = nullptr;
        hr(device->QueryInterface(__uuidof(IDXGIDevice), (void **)&dxgi), "DXGI device");
        IDXGIAdapter *da = nullptr;
        auto code = dxgi->GetAdapter(&da);
        dxgi->Release();
        hr(code, "DXGI adapter");
        DXGI_ADAPTER_DESC dd{};
        code = da->GetDesc(&dd);
        da->Release();
        hr(code, "DXGI descriptor");
        require(dd.VendorId == 0x8086, "Metrics requires an Intel adapter");
        library =
            LoadLibraryExW(path, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        require(library, "Cannot load explicit Metrics Discovery driver library");
        using Open = TCompletionCode(__stdcall *)(IAdapterGroup_1_6 **);
        auto fn = reinterpret_cast<Open>(GetProcAddress(library, "OpenAdapterGroup"));
        require(fn, "Driver lacks OpenAdapterGroup");
        md(fn(&group), "OpenAdapterGroup");
        require(group, "Missing adapter group");
        auto gp = group->GetParams();
        require(gp && gp->Version.MajorNumber == 1 && gp->Version.MinorNumber >= 6 && gp->AdapterCount <= 64,
                "Unsupported MD API or adapter count");
        for (unsigned a = 0; a < gp->AdapterCount; ++a) {
            auto candidate = group->GetAdapter(a);
            require(candidate, "Missing MD adapter");
            auto p = candidate->GetParams();
            if (p && p->SystemId.Type == ADAPTER_ID_TYPE_LUID &&
                p->SystemId.Luid.LowPart == dd.AdapterLuid.LowPart &&
                p->SystemId.Luid.HighPart == dd.AdapterLuid.HighPart) {
                adapter = candidate;
                break;
            }
        }
        require(adapter, "No Metrics Discovery adapter matches the D3D11 device LUID");
        md(adapter->OpenMetricsDevice(&metrics), "OpenMetricsDevice");
        require(metrics, "Missing MD device");
        auto dp = metrics->GetParams();
        require(dp && dp->Version.MajorNumber == 1 && dp->Version.MinorNumber >= 5 &&
                    dp->ConcurrentGroupsCount < 256,
                "Unsupported MD device");
        std::ostringstream out;
        out << "{\"schema_version\":2,\"version\":[" << dp->Version.MajorNumber << ','
            << dp->Version.MinorNumber << ',' << dp->Version.BuildNumber << "],\"vendor\":" << dd.VendorId
            << ",\"device\":" << dd.DeviceId << ",\"luid\":[" << dd.AdapterLuid.LowPart << ','
            << dd.AdapterLuid.HighPart << "],\"sets\":[";
        bool first = true;
        for (unsigned g = 0; g < dp->ConcurrentGroupsCount; ++g) {
            auto cg = metrics->GetConcurrentGroup(g);
            auto cp = cg->GetParams();
            require(cp->MetricSetsCount < 4096, "Invalid MD set count");
            for (unsigned i = 0; i < cp->MetricSetsCount; ++i) {
                auto set = cg->GetMetricSet(i);
                auto p = set->GetParams();
                if (!(p->ApiMask & API_TYPE_DX11) || !p->ApiSpecificId.D3D1XDevDependentId)
                    continue;
                md(set->SetApiFiltering(API_TYPE_DX11), "Filter DX11");
                p = set->GetParams();
                require(p->MetricsCount < 4096 && p->InformationCount < 4096, "Invalid MD metric count");
                if (!first)
                    out << ',';
                first = false;
                out << "{\"group\":" << g << ",\"index\":" << i << ",\"group_name\":" << q(cp->SymbolName)
                    << ",\"name\":" << q(p->SymbolName) << ",\"label\":" << q(p->ShortName)
                    << ",\"report_size\":" << p->QueryReportSize
                    << ",\"counter_id\":" << p->ApiSpecificId.D3D1XDevDependentId
                    << ",\"information_count\":" << p->InformationCount << ",\"metrics\":[";
                for (unsigned m = 0; m < p->MetricsCount; ++m) {
                    if (m)
                        out << ',';
                    auto mp = set->GetMetric(m)->GetParams();
                    out << "{\"id\":" << mp->IdInSet << ",\"name\":" << q(mp->SymbolName)
                        << ",\"label\":" << q(mp->ShortName) << ",\"description\":" << q(mp->LongName)
                        << ",\"unit\":" << q(mp->MetricResultUnits) << ",\"result_type\":" << mp->ResultType
                        << ",\"metric_type\":" << mp->MetricType << '}';
                }
                out << "],\"information\":[";
                for (unsigned n = 0; n < p->InformationCount; ++n) {
                    if (n)
                        out << ',';
                    auto ip = set->GetInformation(n)->GetParams();
                    out << "{\"id\":" << ip->IdInSet << ",\"name\":" << q(ip->SymbolName)
                        << ",\"label\":" << q(ip->ShortName) << ",\"description\":" << q(ip->LongName)
                        << ",\"unit\":" << q(ip->InfoUnits) << ",\"info_type\":" << ip->InfoType << '}';
                }
                out << "]}";
            }
        }
        out << "]}";
        catalog = out.str();
    }
    void select(unsigned g, unsigned s) {
        require(!counter && samples.empty() && recorded.empty(), "Counter samples still owned");
        require(g < metrics->GetParams()->ConcurrentGroupsCount, "Unknown MD group");
        auto cg = metrics->GetConcurrentGroup(g);
        require(s < cg->GetParams()->MetricSetsCount, "Unknown MD set");
        auto set = cg->GetMetricSet(s);
        auto p = set->GetParams();
        require((p->ApiMask & API_TYPE_DX11) && p->ApiSpecificId.D3D1XDevDependentId,
                "Not a DX11 vendor counter set");
        md(set->SetApiFiltering(API_TYPE_DX11), "Filter selected set");
        p = set->GetParams();
        require(p->QueryReportSize > 0 && p->QueryReportSize <= 1024 * 1024 && p->MetricsCount > 0 &&
                    p->MetricsCount + p->InformationCount < 8192,
                "Invalid MD report shape");
        if (selected != set)
            cached.clear();
        selected = set;
    }
    const char *clock() {
        require(metrics && !begun && !activeSample, "Clock calibration requires an open inactive sample");
        auto integer = [&](const char *name) {
            auto v = metrics->GetGlobalSymbolValueByName(name);
            require(v && (v->ValueType == VALUE_TYPE_UINT32 || v->ValueType == VALUE_TYPE_UINT64),
                    "Missing integer clock metadata");
            return v->ValueType == VALUE_TYPE_UINT32 ? uint64_t(v->ValueUInt32) : v->ValueUInt64;
        };
        auto maximum = integer("MaxTimestamp"), frequency = integer("GpuTimestampFrequency");
        require(maximum && frequency, "Invalid clock metadata");
        uint64_t gpu = 0, cpu = 0;
        uint32_t cpuId = 0;
        auto status = metrics->GetGpuCpuTimestamps(&gpu, &cpu, &cpuId);
        std::ostringstream out;
        out << "{\"status\":" << status << ",\"gpu_ns\":" << gpu << ",\"cpu_ns\":" << cpu
            << ",\"cpu_id\":" << cpuId << ",\"maximum_ns\":" << maximum << ",\"frequency_hz\":" << frequency
            << '}';
        clockResult = out.str();
        return clockResult.c_str();
    }
    void begin() {
        require(selected && !counter && samples.empty() && recorded.empty(),
                "Select a metric set before beginning an inactive sample");
        cached.clear();
        result.clear();
        D3D11_COUNTER_DESC desc{D3D11_COUNTER(selected->GetParams()->ApiSpecificId.D3D1XDevDependentId), 0};
        md(selected->Activate(), "Activate metric set");
        auto code = device->CreateCounter(&desc, &counter);
        auto deactivate = selected->Deactivate();
        hr(code, "CreateCounter");
        md(deactivate, "Deactivate metric set");
        require(counter->GetDataSize() == sizeof(void *), "Unexpected driver counter result ABI");
        context->Begin(counter);
        begun = true;
    }
    void submit() {
        require(counter && begun, "No active counter");
        context->End(counter);
        begun = false;
    }
    bool poll(bool flush) {
        require(counter && !begun, "Counter must be ended before polling");
        void *data = nullptr;
        auto code =
            context->GetData(counter, &data, sizeof(data), flush ? 0 : D3D11_ASYNC_GETDATA_DONOTFLUSH);
        hr(code, "Counter GetData");
        if (code == S_FALSE)
            return false;
        require(data, "Driver returned null counter report");
        calculate((uint8_t *)data, selected->GetParams()->QueryReportSize);
        counter->Release();
        counter = nullptr;
        return true;
    }
    void discard() {
        if (counter) {
            if (begun)
                context->End(counter);
            counter->Release();
            counter = nullptr;
        }
        begun = false;
        result.clear();
    }
    Sample &sample(uint64_t token) {
        auto i = samples.find(token);
        require(i != samples.end(), "Unknown or released sample token");
        return *i->second;
    }
    std::unique_ptr<Sample> createSample() {
        auto value = std::make_unique<Sample>(context);
        D3D11_COUNTER_DESC desc{D3D11_COUNTER(selected->GetParams()->ApiSpecificId.D3D1XDevDependentId), 0};
        md(selected->Activate(), "Activate metric set");
        auto code = device->CreateCounter(&desc, &value->counter);
        auto deactivate = selected->Deactivate();
        hr(code, "CreateCounter");
        md(deactivate, "Deactivate metric set");
        require(value->counter && value->counter->GetDataSize() == sizeof(void *),
                "Unexpected driver counter result ABI");
        value->id = ++created;
        return value;
    }
    void sampleReserve(unsigned count) {
        require(selected && !counter && samples.empty() && recorded.empty() && cached.empty(),
                "Reserve requires an empty inactive sample pool");
        require(count > 0 && count <= 256, "Reserve count must be 1..256");
        std::vector<std::unique_ptr<Sample>> values;
        values.reserve(count);
        for (unsigned i = 0; i < count; ++i)
            values.push_back(createSample());
        cached = std::move(values);
    }
    uint64_t sampleBegin() {
        require(selected && !counter && !activeSample && recorded.empty(),
                "Select a set, release recorded counters and end the active sample before Begin");
        require(samples.size() < 256, "Sample ownership limit reached (256)");
        require(nextSample < (std::numeric_limits<uint64_t>::max)(), "Sample token space exhausted");
        std::unique_ptr<Sample> value;
        if (cached.empty())
            value = createSample();
        else {
            value = std::move(cached.back());
            cached.pop_back();
        }
        if (value->uses++)
            ++reused;
        auto token = nextSample++;
        auto &entry = *samples.emplace(token, std::move(value)).first->second;
        context->Begin(entry.counter);
        entry.begun = true;
        activeSample = token;
        return token;
    }
    void sampleSubmit(uint64_t token) {
        auto &s = sample(token);
        require(s.begun && activeSample == token, "Sample is not begun");
        context->End(s.counter);
        s.begun = false;
        activeSample = 0;
    }
    bool samplePoll(uint64_t token, bool flush) {
        auto &s = sample(token);
        require(!activeSample && !s.begun, "End the active sample before polling");
        if (!s.result.empty())
            return true;
        void *data = nullptr;
        auto code =
            context->GetData(s.counter, &data, sizeof(data), flush ? 0 : D3D11_ASYNC_GETDATA_DONOTFLUSH);
        hr(code, "Sample GetData");
        if (code == S_FALSE)
            return false;
        require(data, "Driver returned null counter report");
        s.result = calculateReport((uint8_t *)data, selected->GetParams()->QueryReportSize);
        return true;
    }
    const char *sampleResult(uint64_t token) {
        auto &s = sample(token);
        require(!s.result.empty(), "Sample has no decoded result");
        return s.result.c_str();
    }
    void sampleRelease(uint64_t token) {
        sample(token);
        samples.erase(token);
        if (activeSample == token)
            activeSample = 0;
    }
    void sampleRecycle(uint64_t token) {
        auto &s = sample(token);
        require(!s.begun && !s.result.empty(), "Only a decoded sample can be recycled");
        cached.reserve(cached.size() + 1);
        s.result.clear();
        cached.push_back(std::move(samples.at(token)));
        samples.erase(token);
    }
    void sampleClearCache() {
        require(samples.empty(), "Release owned samples before clearing the cache");
        cached.clear();
    }
    const char *sampleStats() {
        std::ostringstream out;
        out << "{\"created\":" << created << ",\"reused\":" << reused << ",\"cached\":" << cached.size()
            << ",\"owned\":" << samples.size() << '}';
        statsResult = out.str();
        return statsResult.c_str();
    }
    void sameDevice(ID3D11DeviceChild *child) {
        require(child, "Missing D3D11 object");
        ID3D11Device *owner = nullptr;
        child->GetDevice(&owner);
        bool matches = owner == device;
        if (owner)
            owner->Release();
        require(matches, "D3D11 object belongs to another device");
    }
    Recorded &recording(uint64_t token) {
        auto i = recorded.find(token);
        require(i != recorded.end(), "Unknown recorded counter token");
        return *i->second;
    }
    uint64_t recordedBegin(ID3D11DeviceContext *target) {
        require(selected && !counter && samples.empty(),
                "Recorded counters require a selected set and no immediate samples");
        sameDevice(target);
        require(target->GetType() == D3D11_DEVICE_CONTEXT_DEFERRED, "Recording requires a deferred context");
        for (auto &entry : recorded)
            require(entry.second->context != target || !entry.second->recording,
                    "End the active counter on this recording context");
        require(recorded.size() < 256, "Recorded counter limit reached (256)");
        require(nextSample < (std::numeric_limits<uint64_t>::max)(), "Counter token space exhausted");
        cached.clear();
        auto value = std::make_unique<Recorded>(createSample(), target);
        auto token = nextSample++;
        auto &entry = *recorded.emplace(token, std::move(value)).first->second;
        target->Begin(entry.sample->counter);
        entry.recording = true;
        return token;
    }
    void recordedEnd(uint64_t token) {
        auto &r = recording(token);
        require(r.recording, "Recorded counter is already ended");
        r.context->End(r.sample->counter);
        r.recording = false;
    }
    uint64_t recordedExecute(ID3D11CommandList *command, const uint64_t *tokens, unsigned count,
                             bool restore) {
        require(!counter && samples.empty(), "Immediate counter samples still owned");
        sameDevice(command);
        require(tokens && count && count <= 256, "Execution requires 1..256 recorded tokens");
        require(nextExecution < (std::numeric_limits<uint64_t>::max)(), "Execution number space exhausted");
        std::vector<Recorded *> selectedRecords;
        selectedRecords.reserve(count);
        for (unsigned i = 0; i < count; ++i) {
            auto &r = recording(tokens[i]);
            require(!r.recording && !r.pending,
                    "Finish recording and read the previous execution before replay");
            for (auto prior : selectedRecords)
                require(prior != &r, "Duplicate recorded token");
            selectedRecords.push_back(&r);
        }
        auto execution = nextExecution++;
        for (auto r : selectedRecords) {
            r->execution = execution;
            r->sample->result.clear();
            r->pending = true;
        }
        context->ExecuteCommandList(command, restore ? TRUE : FALSE);
        return execution;
    }
    Recorded &recordedExecution(uint64_t token, uint64_t execution) {
        auto &r = recording(token);
        require(execution && r.execution == execution && !r.recording, "Unknown or stale recorded execution");
        return r;
    }
    bool recordedPoll(uint64_t token, uint64_t execution, bool flush) {
        auto &r = recordedExecution(token, execution);
        if (!r.sample->result.empty())
            return true;
        require(r.pending, "Recorded counter has no submitted execution");
        void *data = nullptr;
        auto code = context->GetData(r.sample->counter, &data, sizeof(data),
                                     flush ? 0 : D3D11_ASYNC_GETDATA_DONOTFLUSH);
        hr(code, "Recorded counter GetData");
        if (code == S_FALSE)
            return false;
        require(data, "Driver returned null recorded counter report");
        r.sample->result = calculateReport((uint8_t *)data, selected->GetParams()->QueryReportSize);
        r.pending = false;
        return true;
    }
    const char *recordedResult(uint64_t token, uint64_t execution) {
        auto &r = recordedExecution(token, execution);
        require(!r.sample->result.empty(), "Recorded result is not ready");
        return r.sample->result.c_str();
    }
    void recordedRelease(uint64_t token) {
        recording(token);
        recorded.erase(token);
    }
    void finish(unsigned timeoutMs) {
        require(timeoutMs > 0 && timeoutMs <= 60000, "Invalid counter timeout");
        submit();
        auto start = GetTickCount64();
        while (!poll(true)) {
            require(GetTickCount64() - start < timeoutMs, "Hardware counter timeout");
            Sleep(1);
        }
    }
    void calculate(const uint8_t *data, unsigned size) { result = calculateReport(data, size); }
    std::string calculateReport(const uint8_t *data, unsigned size) {
        require(selected && data, "Missing selected set or report");
        auto params = selected->GetParams();
        require(size == params->QueryReportSize, "Raw report size does not match selected metric set");
        std::vector<uint8_t> raw(data, data + size);
        std::vector<TTypedValue_1_0> values(params->MetricsCount + params->InformationCount);
        unsigned reports = 0;
        md(static_cast<IMetricSet_1_1 *>(selected)->CalculateMetrics(
               raw.data(), unsigned(raw.size()), values.data(), unsigned(values.size() * sizeof(values[0])),
               &reports, false),
           "CalculateMetrics");
        require(reports == 1, "Expected one calculated counter report");
        std::ostringstream out;
        out << "{\"reports\":" << reports << ",\"raw_hex\":\"";
        for (auto b : raw)
            out << std::hex << std::setw(2) << std::setfill('0') << unsigned(b);
        out << std::dec << "\",\"values\":[";
        for (size_t i = 0; i < values.size(); ++i) {
            if (i)
                out << ',';
            auto v = values[i];
            out << "{\"type\":" << v.ValueType << ",\"value\":";
            switch (v.ValueType) {
            case VALUE_TYPE_UINT32:
                out << v.ValueUInt32;
                break;
            case VALUE_TYPE_UINT64:
                out << v.ValueUInt64;
                break;
            case VALUE_TYPE_FLOAT:
                if (std::isfinite(v.ValueFloat))
                    out << std::setprecision(9) << v.ValueFloat;
                else
                    out << "null";
                break;
            case VALUE_TYPE_BOOL:
                out << (v.ValueBool ? "true" : "false");
                break;
            default:
                out << "null";
                break;
            }
            out << '}';
        }
        out << "]}";
        return out.str();
    }
};
#define API extern "C" __declspec(dllexport)
API const char *FloraMdError() { return lastError.c_str(); }
API const char *FloraMdClock(void *p) {
    try {
        require(p, "Missing metrics handle");
        return ((Metrics *)p)->clock();
    } catch (const std::exception &e) {
        lastError = e.what();
        return nullptr;
    }
}
API void *FloraMdOpen(void *device, const wchar_t *path) {
    try {
        lastError.clear();
        auto p = std::make_unique<Metrics>();
        p->open((ID3D11Device *)device, path);
        return p.release();
    } catch (const std::exception &e) {
        lastError = e.what();
        return nullptr;
    }
}
API void FloraMdClose(void *p) { delete static_cast<Metrics *>(p); }
API const char *FloraMdCatalog(void *p) { return p ? static_cast<Metrics *>(p)->catalog.c_str() : nullptr; }
API const char *FloraMdResult(void *p) { return p ? static_cast<Metrics *>(p)->result.c_str() : nullptr; }
API int FloraMdSelect(void *p, unsigned group, unsigned set) {
    try {
        require(p, "Missing MD handle");
        static_cast<Metrics *>(p)->select(group, set);
        return 0;
    } catch (const std::exception &e) {
        lastError = e.what();
        return -1;
    }
}
API int FloraMdBegin(void *p) {
    try {
        require(p, "Missing MD handle");
        static_cast<Metrics *>(p)->begin();
        return 0;
    } catch (const std::exception &e) {
        lastError = e.what();
        return -1;
    }
}
API int FloraMdEnd(void *p, unsigned timeoutMs) {
    try {
        require(p, "Missing MD handle");
        static_cast<Metrics *>(p)->finish(timeoutMs);
        return 0;
    } catch (const std::exception &e) {
        lastError = e.what();
        return -1;
    }
}
API int FloraMdSubmit(void *p) {
    try {
        require(p, "Missing MD handle");
        static_cast<Metrics *>(p)->submit();
        return 0;
    } catch (const std::exception &e) {
        lastError = e.what();
        return -1;
    }
}
API int FloraMdPoll(void *p, int flush) {
    try {
        require(p, "Missing MD handle");
        require(flush == 0 || flush == 1, "Invalid polling flush flag");
        return static_cast<Metrics *>(p)->poll(flush != 0) ? 1 : 0;
    } catch (const std::exception &e) {
        lastError = e.what();
        return -1;
    }
}
API int FloraMdDiscard(void *p) {
    try {
        require(p, "Missing MD handle");
        static_cast<Metrics *>(p)->discard();
        return 0;
    } catch (const std::exception &e) {
        lastError = e.what();
        return -1;
    }
}
API uint64_t FloraMdSampleBegin(void *p) {
    try {
        require(p, "Missing MD handle");
        return static_cast<Metrics *>(p)->sampleBegin();
    } catch (const std::exception &e) {
        lastError = e.what();
        return 0;
    }
}
API int FloraMdSampleSubmit(void *p, uint64_t token) {
    try {
        require(p, "Missing MD handle");
        static_cast<Metrics *>(p)->sampleSubmit(token);
        return 0;
    } catch (const std::exception &e) {
        lastError = e.what();
        return -1;
    }
}
API int FloraMdSamplePoll(void *p, uint64_t token, int flush) {
    try {
        require(p, "Missing MD handle");
        require(flush == 0 || flush == 1, "Invalid polling flush flag");
        return static_cast<Metrics *>(p)->samplePoll(token, flush != 0) ? 1 : 0;
    } catch (const std::exception &e) {
        lastError = e.what();
        return -1;
    }
}
API const char *FloraMdSampleResult(void *p, uint64_t token) {
    try {
        require(p, "Missing MD handle");
        return static_cast<Metrics *>(p)->sampleResult(token);
    } catch (const std::exception &e) {
        lastError = e.what();
        return nullptr;
    }
}
API int FloraMdSampleRelease(void *p, uint64_t token) {
    try {
        require(p, "Missing MD handle");
        static_cast<Metrics *>(p)->sampleRelease(token);
        return 0;
    } catch (const std::exception &e) {
        lastError = e.what();
        return -1;
    }
}
API int FloraMdSampleCount(void *p) {
    try {
        require(p, "Missing MD handle");
        return int(static_cast<Metrics *>(p)->samples.size());
    } catch (const std::exception &e) {
        lastError = e.what();
        return -1;
    }
}
API int FloraMdSampleReserve(void *p, unsigned count) {
    try {
        require(p, "Missing MD handle");
        static_cast<Metrics *>(p)->sampleReserve(count);
        return 0;
    } catch (const std::exception &e) {
        lastError = e.what();
        return -1;
    }
}
API int FloraMdSampleRecycle(void *p, uint64_t token) {
    try {
        require(p, "Missing MD handle");
        static_cast<Metrics *>(p)->sampleRecycle(token);
        return 0;
    } catch (const std::exception &e) {
        lastError = e.what();
        return -1;
    }
}
API int FloraMdSampleClearCache(void *p) {
    try {
        require(p, "Missing MD handle");
        static_cast<Metrics *>(p)->sampleClearCache();
        return 0;
    } catch (const std::exception &e) {
        lastError = e.what();
        return -1;
    }
}
API uint64_t FloraMdSampleInfo(void *p, uint64_t token) {
    try {
        require(p, "Missing MD handle");
        return static_cast<Metrics *>(p)->sample(token).id;
    } catch (const std::exception &e) {
        lastError = e.what();
        return 0;
    }
}
API const char *FloraMdSampleStats(void *p) {
    try {
        require(p, "Missing MD handle");
        return static_cast<Metrics *>(p)->sampleStats();
    } catch (const std::exception &e) {
        lastError = e.what();
        return nullptr;
    }
}
API uint64_t FloraMdRecordedBegin(void *p, void *context) {
    try {
        require(p, "Missing MD handle");
        return static_cast<Metrics *>(p)->recordedBegin(static_cast<ID3D11DeviceContext *>(context));
    } catch (const std::exception &e) {
        lastError = e.what();
        return 0;
    }
}
API int FloraMdRecordedEnd(void *p, uint64_t token) {
    try {
        require(p, "Missing MD handle");
        static_cast<Metrics *>(p)->recordedEnd(token);
        return 0;
    } catch (const std::exception &e) {
        lastError = e.what();
        return -1;
    }
}
API uint64_t FloraMdRecordedExecute(void *p, void *command, const uint64_t *tokens, unsigned count,
                                    int restore) {
    try {
        require(p, "Missing MD handle");
        require(restore == 0 || restore == 1, "Invalid restore flag");
        return static_cast<Metrics *>(p)->recordedExecute(static_cast<ID3D11CommandList *>(command), tokens,
                                                          count, restore != 0);
    } catch (const std::exception &e) {
        lastError = e.what();
        return 0;
    }
}
API int FloraMdRecordedPoll(void *p, uint64_t token, uint64_t execution, int flush) {
    try {
        require(p, "Missing MD handle");
        require(flush == 0 || flush == 1, "Invalid polling flush flag");
        return static_cast<Metrics *>(p)->recordedPoll(token, execution, flush != 0) ? 1 : 0;
    } catch (const std::exception &e) {
        lastError = e.what();
        return -1;
    }
}
API const char *FloraMdRecordedResult(void *p, uint64_t token, uint64_t execution) {
    try {
        require(p, "Missing MD handle");
        return static_cast<Metrics *>(p)->recordedResult(token, execution);
    } catch (const std::exception &e) {
        lastError = e.what();
        return nullptr;
    }
}
API int FloraMdRecordedRelease(void *p, uint64_t token) {
    try {
        require(p, "Missing MD handle");
        static_cast<Metrics *>(p)->recordedRelease(token);
        return 0;
    } catch (const std::exception &e) {
        lastError = e.what();
        return -1;
    }
}
API int FloraMdRecordedCount(void *p) {
    try {
        require(p, "Missing MD handle");
        return int(static_cast<Metrics *>(p)->recorded.size());
    } catch (const std::exception &e) {
        lastError = e.what();
        return -1;
    }
}
API int FloraMdDecode(void *p, const void *data, unsigned size) {
    try {
        require(p, "Missing MD handle");
        auto m = static_cast<Metrics *>(p);
        require(!m->counter && m->samples.empty(), "Cannot decode while counter samples are owned");
        m->calculate((const uint8_t *)data, size);
        return 0;
    } catch (const std::exception &e) {
        lastError = e.what();
        return -1;
    }
}
