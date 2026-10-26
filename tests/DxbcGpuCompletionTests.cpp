#include "application/DxbcGpuCompletion.h"
#include "application/DxbcGpuOperations.h"
#include "application/HlslCompilation.h"
#include "replay/Replay.h"
#include <QtTest>
#include <chrono>
#include <d3d11sdklayers.h>
#include <thread>
using namespace flora;
using namespace flora::dxbc_exec;
using Json = nlohmann::json;
namespace {
struct Device {
    Com<ID3D11Device> device;
    Com<ID3D11DeviceContext> context;
    explicit Device(uint32_t vendor) {
        Com<IDXGIAdapter1> selected;
        if (vendor) {
            Com<IDXGIFactory1> factory;
            check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "Create completion test factory");
            for (UINT i = 0;; ++i) {
                Com<IDXGIAdapter1> candidate;
                const auto hr = factory->EnumAdapters1(i, &candidate);
                if (hr == DXGI_ERROR_NOT_FOUND)
                    break;
                check(hr, "Enumerate completion adapter");
                DXGI_ADAPTER_DESC1 desc{};
                check(candidate->GetDesc1(&desc), "Read completion adapter");
                if (desc.VendorId == vendor && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
                    selected = candidate;
                    break;
                }
            }
            if (!selected)
                throw std::runtime_error("Requested completion adapter is unavailable");
        }
        check(D3D11CreateDevice(selected.Get(), vendor ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_WARP,
                                nullptr, D3D11_CREATE_DEVICE_DEBUG, nullptr, 0, D3D11_SDK_VERSION, &device,
                                nullptr, &context),
              "Create completion test device");
    }
    void validate() {
        Com<ID3D11InfoQueue> queue;
        check(device.As(&queue), "Read completion validation queue");
        for (UINT64 i = 0; i < queue->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
            SIZE_T size{};
            check(queue->GetMessage(i, nullptr, &size), "Read completion validation size");
            std::vector<uint8_t> bytes(size);
            auto *message = reinterpret_cast<D3D11_MESSAGE *>(bytes.data());
            check(queue->GetMessage(i, message, &size), "Read completion validation message");
            if (message->Severity <= D3D11_MESSAGE_SEVERITY_ERROR)
                throw std::runtime_error(message->pDescription);
        }
        queue->ClearStoredMessages();
    }
};
struct Copy {
    Com<ID3D11Buffer> original, staging;
    std::array<uint32_t, 64> expected;
    explicit Copy(Device &device) {
        for (uint32_t i = 0; i < expected.size(); ++i)
            expected[i] = (i * 1664525u) ^ 0x89abcdefu;
        D3D11_BUFFER_DESC desc{UINT(sizeof expected), D3D11_USAGE_DEFAULT, 0, 0, 0, 0};
        const D3D11_SUBRESOURCE_DATA data{expected.data(), 0, 0};
        check(device.device->CreateBuffer(&desc, &data, &original), "Create completion test data");
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        check(device.device->CreateBuffer(&desc, nullptr, &staging), "Create completion staging");
    }
    void queue(Device &device) { device.context->CopyResource(staging.Get(), original.Get()); }
    bool verify(Device &device, GpuCompletion &completion) {
        const auto mapped = completion.map(staging.Get());
        const bool exact = std::memcmp(mapped.pData, expected.data(), sizeof expected) == 0;
        device.context->Unmap(staging.Get(), 0);
        return exact;
    }
};
void rows() {
    QTest::addColumn<uint32_t>("vendor");
    QTest::addColumn<bool>("poll");
    for (uint32_t vendor : {0u, 4318u, 32902u})
        for (bool poll : {false, true})
            QTest::newRow(qPrintable(QString("%1-%2").arg(vendor).arg(poll ? "poll" : "event")))
                << vendor << poll;
}
GpuCompletionMode mode(bool poll) { return poll ? GpuCompletionMode::Poll : GpuCompletionMode::Automatic; }
bool timesOut(const std::function<void()> &operation) {
    try {
        operation();
    } catch (const std::runtime_error &error) {
        return std::string(error.what()) == "Native GPU operation timed out";
    }
    return false;
}
} // namespace
class DxbcGpuCompletionTests final : public QObject {
    Q_OBJECT
    std::map<uint32_t, std::unique_ptr<Device>> devices;
  private slots:
    void initTestCase() {
        for (uint32_t vendor : {0u, 4318u, 32902u})
            devices[vendor] = std::make_unique<Device>(vendor);
    }
    void readbacks_data() { rows(); }
    void readbacks() {
        try {
            QFETCH(uint32_t, vendor);
            QFETCH(bool, poll);
            auto &device = *devices.at(vendor);
            Copy copy(device);
            GpuCompletion completion(device.device.Get(), device.context.Get(), {}, 5000, mode(poll));
            QCOMPARE(completion.usesFence(), !poll);
            Com<ID3D11Query> query;
            const D3D11_QUERY_DESC qd{D3D11_QUERY_EVENT, 0};
            check(device.device->CreateQuery(&qd, &query), "Create completion test query");
            for (unsigned i = 0; i < 32; ++i) {
                device.context->End(query.Get());
                copy.queue(device);
                completion.submit();
                QVERIFY(copy.verify(device, completion));
                BOOL ready = FALSE;
                completion.query(query.Get(), &ready, sizeof ready);
                QVERIFY(ready);
            }
            // Four subresources share a single submission and deadline.
            std::array<std::vector<uint32_t>, 4> contents;
            std::array<D3D11_SUBRESOURCE_DATA, 4> initial{};
            for (unsigned i = 0; i < 4; ++i) {
                const auto size = i % 2 ? 2u : 4u;
                contents[i].resize(size * size);
                for (unsigned j = 0; j < contents[i].size(); ++j)
                    contents[i][j] = 1000 * i + j;
                initial[i] = {contents[i].data(), size * 4, 0};
            }
            D3D11_TEXTURE2D_DESC td{4, 4, 2, 2, DXGI_FORMAT_R32_UINT, {1, 0}, D3D11_USAGE_DEFAULT, 0, 0, 0};
            Com<ID3D11Texture2D> original, staging;
            check(device.device->CreateTexture2D(&td, initial.data(), &original),
                  "Create multi-subresource test");
            td.Usage = D3D11_USAGE_STAGING;
            td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            check(device.device->CreateTexture2D(&td, nullptr, &staging), "Create multi-subresource staging");
            device.context->CopyResource(staging.Get(), original.Get());
            completion.submit();
            for (unsigned i = 0; i < 4; ++i) {
                const auto size = i % 2 ? 2u : 4u;
                const auto mapped = completion.map(staging.Get(), i, true);
                bool exact = true;
                for (unsigned y = 0; y < size; ++y)
                    exact &= std::memcmp(static_cast<const uint8_t *>(mapped.pData) + y * mapped.RowPitch,
                                         contents[i].data() + y * size, size * 4) == 0;
                device.context->Unmap(staging.Get(), i);
                QVERIFY(exact);
            }
            device.validate();
            qInfo().noquote() << QString::fromStdString(Json{
                {"completion_readbacks", true},
                {"vendor", vendor},
                {"poll", poll},
                {"copies", 32},
                {"queries", 32},
                {"subresources", 4},
                {"fence", completion.usesFence()}}.dump());
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
    void cancellation_data() { rows(); }
    void cancellation() {
        try {
            QFETCH(uint32_t, vendor);
            QFETCH(bool, poll);
            auto &device = *devices.at(vendor);
            Copy copy(device);
            Json control = Json::array();
            for (unsigned i = 0; i < 32; ++i) {
                {
                    GpuCompletion normal(device.device.Get(), device.context.Get(), {}, 5000, mode(poll));
                    copy.queue(device);
                    normal.submit();
                    QVERIFY(copy.verify(device, normal));
                }
                DWORD handles{};
                QVERIFY(GetProcessHandleCount(GetCurrentProcess(), &handles));
                control.push_back(handles);
            }
            DWORD before{}, after{};
            Json counts = Json::array();
            QVERIFY(GetProcessHandleCount(GetCurrentProcess(), &before));
            for (unsigned i = 0; i < 64; ++i) {
                {
                    GpuCompletion cancelled(
                        device.device.Get(), device.context.Get(), [] { return true; }, 5000, mode(poll));
                    copy.queue(device);
                    cancelled.submit();
                    QVERIFY_THROWS_EXCEPTION(OperationCancelled, cancelled.map(copy.staging.Get()));
                    QVERIFY_THROWS_EXCEPTION(std::runtime_error, cancelled.submit());
                }
                // Churn handle values while the abandoned registration may still be pending.
                const auto guard = CreateEventW(nullptr, TRUE, FALSE, nullptr);
                QVERIFY(guard != nullptr);
                bool exact = false;
                {
                    GpuCompletion retry(device.device.Get(), device.context.Get(), {}, 5000, mode(poll));
                    copy.queue(device);
                    retry.submit();
                    exact = copy.verify(device, retry);
                }
                const auto guardState = WaitForSingleObject(guard, 0);
                CloseHandle(guard);
                QVERIFY(exact);
                QCOMPARE(guardState, DWORD(WAIT_TIMEOUT));
                DWORD handles{};
                QVERIFY(GetProcessHandleCount(GetCurrentProcess(), &handles));
                counts.push_back(handles);
            }
            QVERIFY(GetProcessHandleCount(GetCurrentProcess(), &after));
            qInfo().noquote() << QString::fromStdString(Json{
                {"handle_observation", true},
                {"counts", counts},
                {"completed_control_counts", control},
                {"vendor", vendor},
                {"poll", poll},
                {"before", before},
                {"after", after}}.dump());
            QVERIFY2(after <= before + 4, "Cancellation retained an unbounded number of event handles");
            device.validate();
            qInfo().noquote() << QString::fromStdString(Json{
                {"completion_cancellation", true},
                {"vendor", vendor},
                {"poll", poll},
                {"cancelled", 64},
                {"retries", 64},
                {"handle_delta", int64_t(after) - before}}.dump());
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
    void deadlines_data() { rows(); }
    void deadlines() {
        std::string phase = "setup";
        try {
            QFETCH(uint32_t, vendor);
            QFETCH(bool, poll);
            auto &device = *devices.at(vendor);
            Copy copy(device);
            GpuCompletion delayed(
                device.device.Get(), device.context.Get(),
                [] {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    return false;
                },
                1, mode(poll));
            copy.queue(device);
            delayed.submit();
            QVERIFY(timesOut([&] { delayed.map(copy.staging.Get()); }));
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, delayed.submit());
            phase = "first shared-deadline readback";
            GpuCompletion shared(device.device.Get(), device.context.Get(), {}, 5000, mode(poll));
            copy.queue(device);
            shared.submit();
            QVERIFY(copy.verify(device, shared));
            phase = "expired shared-deadline readback";
            std::this_thread::sleep_for(std::chrono::milliseconds(5010));
            QVERIFY(timesOut([&] { shared.map(copy.staging.Get()); }));
            phase = "fresh deadline retry";
            GpuCompletion retry(device.device.Get(), device.context.Get(), {}, 5000, mode(poll));
            copy.queue(device);
            retry.submit();
            QVERIFY(copy.verify(device, retry));
            device.validate();
        } catch (const std::exception &error) {
            QFAIL((phase + ": " + error.what()).c_str());
        }
    }
    void stages_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<bool>("poll");
        QTest::addColumn<uint32_t>("stage");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            for (bool poll : {false, true})
                for (uint32_t stage : {0u, 1u, 5u})
                    QTest::newRow(
                        qPrintable(QString("%1-%2-%3").arg(vendor).arg(poll ? "poll" : "event").arg(stage)))
                        << vendor << poll << stage;
    }
    void stages() {
        try {
            QFETCH(uint32_t, vendor);
            QFETCH(bool, poll);
            QFETCH(uint32_t, stage);
            auto &device = *devices.at(vendor);
            auto gpu = std::make_shared<GpuOperations>(device.device.Get(), vendor == 0, CancelCheck{}, 5000,
                                                       mode(poll));
            const std::array<uint32_t, 4> expected{0xffffffffu, 0x80000000u, 0x7fc12345u, 17};
            std::vector<uint8_t> bytes(16);
            std::memcpy(bytes.data(), expected.data(), 16);
            Inputs input;
            input.gpu = gpu;
            input.memory = std::make_shared<Memory>(
                std::map<uint64_t, Buffer>{{1, Buffer::known(bytes)},
                                           {2, Buffer::known(std::vector<uint8_t>(16))}},
                std::map<Register, BufferView>{{{7, {0}}, {1, 0, 16, 0, {}, DXGI_FORMAT_R32G32B32A32_UINT}},
                                               {{30, {0}}, {2, 0, 16, 0, {}, DXGI_FORMAT_R32G32B32A32_UINT}}},
                1024, 1024, gpu, true);
            const std::string source =
                "Buffer<uint4> source:register(t0);" +
                (stage == 5 ? std::string("RWBuffer<uint4> output:register(u0);[numthreads(1,1,1)]void "
                                          "main(){output[0]=source[0];}")
                            : std::string("uint4 main():") + (stage == 0 ? "SV_Target0" : "TEXCOORD0") +
                                  "{return source[0];}");
            Program program(compileHlsl(source, stage == 5   ? "cs_5_0"
                                                : stage == 0 ? "ps_5_0"
                                                             : "vs_5_0")
                                .bytecode);
            Lane lane(program, input);
            unsigned assisted = 0;
            while (!lane.finished())
                assisted += lane.step().memoryGpuAssisted;
            QCOMPARE(assisted, stage == 5 ? 2u : 1u);
            if (stage == 5)
                QCOMPARE(input.memory->buffers().at(2).bytes, bytes);
            else
                QCOMPARE(lane.value({2, {0}}), Value::known(expected));
            QCOMPARE(input.memory->buffers().at(1).bytes, bytes);
            QCOMPARE(gpu->identity().at("completion_wait").get<std::string>(),
                     std::string(poll ? "native-poll/1" : "fence-event/1"));
            device.validate();
            qInfo().noquote() << QString::fromStdString(Json{
                {"completion_stage", true},
                {"vendor", vendor},
                {"poll", poll},
                {"stage", stage},
                {"assisted", assisted},
                {"helper", gpu->identity()}}.dump());
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
    void invalid() {
        try {
            auto &device = *devices.at(0);
            QVERIFY_THROWS_EXCEPTION(std::invalid_argument, GpuCompletion(nullptr, device.context.Get()));
            QVERIFY_THROWS_EXCEPTION(std::invalid_argument,
                                     GpuCompletion(device.device.Get(), device.context.Get(), {}, 0));
            QVERIFY_THROWS_EXCEPTION(std::invalid_argument,
                                     GpuCompletion(device.device.Get(), device.context.Get(), {}, 60001));
            QVERIFY_THROWS_EXCEPTION(std::invalid_argument,
                                     GpuCompletion(device.device.Get(), devices.at(4318)->context.Get()));
            Com<ID3D11DeviceContext> deferred;
            check(device.device->CreateDeferredContext(0, &deferred), "Create rejected completion context");
            QVERIFY_THROWS_EXCEPTION(std::invalid_argument,
                                     GpuCompletion(device.device.Get(), deferred.Get()));
            for (auto selected : {GpuCompletionMode::Automatic, GpuCompletionMode::Poll}) {
                GpuCompletion empty(device.device.Get(), device.context.Get(), {}, 5000, selected);
                QVERIFY_THROWS_EXCEPTION(std::invalid_argument, empty.map(nullptr));
                QVERIFY_THROWS_EXCEPTION(std::runtime_error, empty.submit());
                GpuCompletion pending(device.device.Get(), device.context.Get(), {}, 5000, selected);
                pending.submit();
                QVERIFY_THROWS_EXCEPTION(std::runtime_error, pending.submit());
            }
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
};
QTEST_GUILESS_MAIN(DxbcGpuCompletionTests)
#include "DxbcGpuCompletionTests.moc"
