// Independent completion observations. No replay, GPA or RenderDoc implementation is used.
#define NOMINMAX
#include <algorithm>
#include <array>
#include <chrono>
#include <d3d11_4.h>
#include <d3d11sdklayers.h>
#include <d3dcompiler.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <nlohmann/json.hpp>
#include <thread>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
namespace {
void check(HRESULT result) {
    if (FAILED(result))
        throw std::runtime_error("Completion probe D3D11 failure: " + std::to_string(result));
}
struct Event {
    HANDLE handle = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    Event() {
        if (!handle)
            throw std::runtime_error("Cannot create completion event");
    }
    ~Event() { CloseHandle(handle); }
    Event(const Event &) = delete;
    Event &operator=(const Event &) = delete;
};
uint64_t cpuTime() {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user))
        throw std::runtime_error("Cannot read probe thread CPU time");
    return ((uint64_t(kernel.dwHighDateTime) << 32) | kernel.dwLowDateTime) +
           ((uint64_t(user.dwHighDateTime) << 32) | user.dwLowDateTime);
}
double milliseconds(Clock::time_point begin) {
    return std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
}
enum class Outcome { Complete, Cancelled, Timeout };
Outcome waitFence(ID3D11Fence *fence, uint64_t value, HANDLE event, unsigned timeout,
                  const std::function<bool()> &cancelled, unsigned &waits) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout);
    for (;;) {
        if (cancelled && cancelled())
            return Outcome::Cancelled;
        const auto completed = fence->GetCompletedValue();
        if (completed == UINT64_MAX)
            throw std::runtime_error("Fence device removed");
        if (completed >= value) {
            const auto ready = WaitForSingleObject(event, 0);
            if (ready == WAIT_OBJECT_0)
                return Outcome::Complete;
            if (ready != WAIT_TIMEOUT)
                throw std::runtime_error("Completion event check failed");
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
        if (remaining.count() <= 0)
            return Outcome::Timeout;
        ++waits;
        const auto status = WaitForSingleObject(event, DWORD(std::min<int64_t>(10, remaining.count())));
        if (status != WAIT_OBJECT_0 && status != WAIT_TIMEOUT)
            throw std::runtime_error("Completion event wait failed");
    }
}
ComPtr<ID3DBlob> compile(const std::string &source, const char *profile) {
    ComPtr<ID3DBlob> code, errors;
    const auto status = D3DCompile(source.data(), source.size(), "Independent completion probe", nullptr,
                                   nullptr, "main", profile, D3DCOMPILE_ENABLE_STRICTNESS, 0, &code, &errors);
    if (FAILED(status) && errors)
        throw std::runtime_error(
            std::string(static_cast<const char *>(errors->GetBufferPointer()), errors->GetBufferSize()));
    check(status);
    return code;
}
void validation(ID3D11Device *device) {
    ComPtr<ID3D11InfoQueue> queue;
    check(device->QueryInterface(IID_PPV_ARGS(&queue)));
    for (UINT64 i = 0; i < queue->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
        SIZE_T size{};
        check(queue->GetMessage(i, nullptr, &size));
        std::vector<uint8_t> bytes(size);
        auto *message = reinterpret_cast<D3D11_MESSAGE *>(bytes.data());
        check(queue->GetMessage(i, message, &size));
        if (message->Severity <= D3D11_MESSAGE_SEVERITY_ERROR)
            throw std::runtime_error(message->pDescription);
    }
    queue->ClearStoredMessages();
}
} // namespace
int wmain(int argc, wchar_t **argv) {
    if (argc != 3) {
        std::cerr << "NEW_OUTPUT_DIRECTORY VENDOR_DECIMAL_OR_ZERO_FOR_WARP\n";
        return 2;
    }
    try {
        const auto directory = std::filesystem::absolute(argv[1]);
        if (!std::filesystem::create_directory(directory))
            throw std::runtime_error("Output directory must be new");
        const auto vendor = uint32_t(std::stoul(argv[2]));
        ComPtr<IDXGIAdapter1> selected;
        if (vendor) {
            ComPtr<IDXGIFactory1> factory;
            check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
            for (UINT i = 0;; ++i) {
                ComPtr<IDXGIAdapter1> candidate;
                const auto status = factory->EnumAdapters1(i, &candidate);
                if (status == DXGI_ERROR_NOT_FOUND)
                    break;
                check(status);
                DXGI_ADAPTER_DESC1 descriptor{};
                check(candidate->GetDesc1(&descriptor));
                if (descriptor.VendorId == vendor && !(descriptor.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
                    selected = candidate;
                    break;
                }
            }
            if (!selected)
                throw std::runtime_error("Requested completion probe adapter is unavailable");
        }
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        check(D3D11CreateDevice(selected.Get(), vendor ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_WARP,
                                nullptr, D3D11_CREATE_DEVICE_DEBUG, nullptr, 0, D3D11_SDK_VERSION, &device,
                                nullptr, &context));
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDXGIAdapter> adapter;
        check(device.As(&dxgi));
        check(dxgi->GetAdapter(&adapter));
        DXGI_ADAPTER_DESC actual{};
        check(adapter->GetDesc(&actual));
        LARGE_INTEGER driver{};
        check(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &driver));
        ComPtr<ID3D11Device5> device5;
        ComPtr<ID3D11DeviceContext4> context4;
        ComPtr<ID3D11Fence> fence;
        const auto deviceStatus = device.As(&device5), contextStatus = context.As(&context4);
        const auto fenceStatus =
            device5 ? device5->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)) : E_NOINTERFACE;
        const bool supported = SUCCEEDED(deviceStatus) && SUCCEEDED(contextStatus) && SUCCEEDED(fenceStatus);
        Json report{{"schema", 1},
                    {"scope", "independent_gpu_completion"},
                    {"vendor", vendor},
                    {"adapter_vendor", actual.VendorId},
                    {"adapter_device", actual.DeviceId},
                    {"adapter_luid",
                     (uint64_t(uint32_t(actual.AdapterLuid.HighPart)) << 32) | actual.AdapterLuid.LowPart},
                    {"driver_version", driver.QuadPart},
                    {"device5_hresult", deviceStatus},
                    {"context4_hresult", contextStatus},
                    {"create_fence_hresult", fenceStatus},
                    {"fence_supported", supported},
                    {"cases", Json::array()}};
        Event completion;
        uint64_t value = 0;
        auto csCode =
            compile("cbuffer Input:register(b0){uint seed;} RWStructuredBuffer<uint> output:register(u0);"
                    "[numthreads(64,1,1)] void main(uint "
                    "i:SV_DispatchThreadID){output[i]=((i*1664525u)^seed)+1013904223u;}",
                    "cs_5_0");
        ComPtr<ID3D11ComputeShader> shader;
        check(device->CreateComputeShader(csCode->GetBufferPointer(), csCode->GetBufferSize(), nullptr,
                                          &shader));
        std::ofstream shaderFile(directory / "compute.dxbc", std::ios::binary);
        shaderFile.write(static_cast<const char *>(csCode->GetBufferPointer()), csCode->GetBufferSize());
        shaderFile.close();
        if (!shaderFile)
            throw std::runtime_error("Cannot save completion shader");
        D3D11_BUFFER_DESC cbDescriptor{16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
        ComPtr<ID3D11Buffer> constants;
        check(device->CreateBuffer(&cbDescriptor, nullptr, &constants));
        for (unsigned workload = 0; workload < 4; ++workload) {
            const bool compute = workload >= 2;
            const UINT count = workload == 1 ? 262144 : workload == 3 ? 4096 : 64;
            D3D11_BUFFER_DESC descriptor{count * 4,
                                         D3D11_USAGE_DEFAULT,
                                         compute ? UINT(D3D11_BIND_UNORDERED_ACCESS) : 0,
                                         0,
                                         compute ? UINT(D3D11_RESOURCE_MISC_BUFFER_STRUCTURED) : 0,
                                         compute ? 4u : 0};
            ComPtr<ID3D11Buffer> output, staging;
            check(device->CreateBuffer(&descriptor, nullptr, &output));
            ComPtr<ID3D11UnorderedAccessView> uav;
            if (compute)
                check(device->CreateUnorderedAccessView(output.Get(), nullptr, &uav));
            descriptor.Usage = D3D11_USAGE_STAGING;
            descriptor.BindFlags = descriptor.MiscFlags = descriptor.StructureByteStride = 0;
            descriptor.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            check(device->CreateBuffer(&descriptor, nullptr, &staging));
            ComPtr<ID3D11Query> query;
            const D3D11_QUERY_DESC queryDescriptor{D3D11_QUERY_EVENT, 0};
            check(device->CreateQuery(&queryDescriptor, &query));
            for (unsigned iteration = 0; iteration < 36; ++iteration) {
                const UINT seed = 0x12345678u ^ (iteration * 113u + workload);
                std::vector<UINT> expected(count);
                for (UINT i = 0; i < count; ++i)
                    expected[i] = ((i * 1664525u) ^ seed) + 1013904223u;
                for (unsigned order = 0; order < (supported ? 3u : 1u); ++order) {
                    const auto mode = supported ? (order + iteration) % 3 : 0;
                    const bool eventMode = mode != 0, deferredFence = mode == 2;
                    unsigned waits = 0, maps = 0, sleeps = 0, queryPolls = 0;
                    HRESULT firstMap = S_OK;
                    const auto cpuBegin = cpuTime();
                    const auto begin = Clock::now();
                    if (compute) {
                        const std::array<UINT, 4> parameters{seed, 0, 0, 0};
                        context->UpdateSubresource(constants.Get(), 0, nullptr, parameters.data(), 0, 0);
                        auto *cb = constants.Get();
                        auto *view = uav.Get();
                        context->CSSetConstantBuffers(0, 1, &cb);
                        context->CSSetUnorderedAccessViews(0, 1, &view, nullptr);
                        context->CSSetShader(shader.Get(), nullptr, 0);
                        context->Dispatch(count / 64, 1, 1);
                        context->ClearState();
                    } else
                        context->UpdateSubresource(output.Get(), 0, nullptr, expected.data(), 0, 0);
                    context->End(query.Get());
                    context->CopyResource(staging.Get(), output.Get());
                    const auto signalCompletion = [&] {
                        if (!ResetEvent(completion.handle))
                            throw std::runtime_error("Cannot reset completion event");
                        check(context4->Signal(fence.Get(), ++value));
                        check(fence->SetEventOnCompletion(value, completion.handle));
                    };
                    if (eventMode && !deferredFence)
                        signalCompletion();
                    context->Flush();
                    const auto waitBegin = Clock::now();
                    if (eventMode && !deferredFence &&
                        waitFence(fence.Get(), value, completion.handle, 5000, {}, waits) !=
                            Outcome::Complete)
                        throw std::runtime_error("Submitted fence timed out");
                    D3D11_MAPPED_SUBRESOURCE mapped{};
                    for (;;) {
                        ++maps;
                        const auto status = context->Map(staging.Get(), 0, D3D11_MAP_READ,
                                                         D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
                        if (maps == 1)
                            firstMap = status;
                        if (status == S_OK)
                            break;
                        if (status != DXGI_ERROR_WAS_STILL_DRAWING)
                            check(status);
                        if (milliseconds(waitBegin) >= 5000)
                            throw std::runtime_error("Polling readback timed out");
                        if (deferredFence && maps == 1) {
                            signalCompletion();
                            context->Flush();
                            if (waitFence(fence.Get(), value, completion.handle, 5000, {}, waits) !=
                                Outcome::Complete)
                                throw std::runtime_error("Fence after initial Map timed out");
                            continue;
                        }
                        ++sleeps;
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    }
                    const auto waitMs = milliseconds(waitBegin);
                    const bool exact = std::memcmp(mapped.pData, expected.data(), count * 4) == 0;
                    context->Unmap(staging.Get(), 0);
                    BOOL queryReady = FALSE;
                    for (;;) {
                        ++queryPolls;
                        const auto queryStatus = context->GetData(query.Get(), &queryReady, sizeof queryReady,
                                                                  D3D11_ASYNC_GETDATA_DONOTFLUSH);
                        check(queryStatus);
                        if (queryStatus == S_OK && queryReady)
                            break;
                        if (milliseconds(waitBegin) >= 5000)
                            throw std::runtime_error("Preceding query timed out");
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    }
                    if (!exact)
                        throw std::runtime_error(
                            "Completion did not preserve exact data and preceding query availability");
                    report["cases"].push_back({{"workload", workload},
                                               {"iteration", iteration},
                                               {"warmup", iteration < 4},
                                               {"order", order},
                                               {"mode", deferredFence ? "map-fence"
                                                        : eventMode   ? "fence"
                                                                      : "poll"},
                                               {"words", count},
                                               {"seed", seed},
                                               {"exact", exact},
                                               {"query_ready", bool(queryReady)},
                                               {"maps", maps},
                                               {"first_map_hresult", firstMap},
                                               {"query_polls", queryPolls},
                                               {"sleeps", sleeps},
                                               {"wait_calls", waits},
                                               {"wait_ms", waitMs},
                                               {"elapsed_ms", milliseconds(begin)},
                                               {"cpu_ms", double(cpuTime() - cpuBegin) / 10000.0}});
                    validation(device.Get());
                }
            }
        }
        if (supported) {
            ComPtr<ID3D11Fence> pending;
            check(device5->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&pending)));
            Event pendingEvent;
            check(pending->SetEventOnCompletion(1, pendingEvent.handle));
            unsigned waits = 0, checks = 0;
            const auto timeoutBegin = Clock::now();
            const auto timed = waitFence(pending.Get(), 1, pendingEvent.handle, 4, {}, waits);
            const auto timeoutMs = milliseconds(timeoutBegin);
            const auto cancelBegin = Clock::now();
            const auto cancelled =
                waitFence(pending.Get(), 1, pendingEvent.handle, 5000, [&] { return ++checks == 2; }, waits);
            const auto cancelMs = milliseconds(cancelBegin);
            if (timed != Outcome::Timeout || cancelled != Outcome::Cancelled || checks != 2)
                throw std::runtime_error("Unsignaled fence timeout/cancellation was not bounded");
            // Resolve the independent, unsignaled fence before destroying its registered event.
            check(context4->Signal(pending.Get(), 1));
            context->Flush();
            const auto retry = waitFence(pending.Get(), 1, pendingEvent.handle, 5000, {}, waits);
            if (retry != Outcome::Complete)
                throw std::runtime_error("Fence recovery failed");
            Event submittedEvent;
            check(context4->Signal(fence.Get(), ++value));
            check(fence->SetEventOnCompletion(value, submittedEvent.handle));
            context->Flush();
            const auto afterSubmit =
                waitFence(fence.Get(), value, submittedEvent.handle, 5000, [] { return true; }, waits);
            const auto drained = waitFence(fence.Get(), value, submittedEvent.handle, 5000, {}, waits);
            if (afterSubmit != Outcome::Cancelled || drained != Outcome::Complete)
                throw std::runtime_error("Submitted cancellation did not preserve completion lifetime");
            report["recovery"] = {{"timeout", true},
                                  {"timeout_ms", timeoutMs},
                                  {"cancel_checks", checks},
                                  {"cancel_ms", cancelMs},
                                  {"cancelled", true},
                                  {"subsequent_completion", true},
                                  {"submitted_cancellation", true},
                                  {"registered_events_drained", true}};
        }
        validation(device.Get());
        report["unexpected_validation_errors"] = 0;
        report["completed"] = true;
        report["production_wait_changed"] = false;
        report["performance_acceptance"] = false;
        std::ofstream file(directory / "results.json");
        file << report.dump(2) << '\n';
        if (!file)
            throw std::runtime_error("Cannot save completion observations");
        std::cout << "Completion cases: " << report["cases"].size() << "; fence supported: " << supported
                  << '\n';
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
