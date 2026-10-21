#include "dx12/CpuBindings.h"
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QElapsedTimer>
#include <QtTest>
#include <wrl/client.h>
#include <d3d12.h>
using namespace flora;
using Json = nlohmann::json;
namespace {
template<class T> void append(QByteArray &b, T value) { b.append(reinterpret_cast<const char *>(&value), sizeof(value)); }
void qword(QByteArray &b, uint64_t v) { append(b, v); }
void dword(QByteArray &b, uint32_t v) { append(b, v); }
void save(const QString &path, const QByteArray &bytes) { QFile f(path); if (!f.open(QIODevice::WriteOnly) || f.write(bytes) != bytes.size()) throw std::runtime_error("Fixture write failed"); }
void output(QByteArray &b, const GUID &iid, uint64_t object) { append(b, iid); b.append(char(1)); qword(b, 1); qword(b, object); dword(b, 0); }
}
class CpuBindingTests : public QObject {
    Q_OBJECT
  private slots:
    void realCaptureIndex() {
        const auto root = qEnvironmentVariable("FLORA_DX12_CAPTURE_DIR");
        if (root.isEmpty()) QSKIP("Set capture directory for CPU index measurements");
        for (const auto &name : {"bf1_2025_12_10__14_29_55.gpa_frame", "Talos_2026_10_18__00_55_08.gpa_frame",
                                "Hellblade2-Win64-Shipping_2026_10_17__03_44_05.gpa_frame"}) {
          try {
            dx12::StreamDirectory directory(std::filesystem::path((root + '/' + name).toStdWString()));
            dx12::SubmissionIndex submissions(directory);
            QElapsedTimer timer; timer.start(); dx12::CpuBindings index(directory, submissions);
            const auto buildMs = timer.elapsed();
            std::vector<uint64_t> draws;
            for (const auto &list : submissions.lists()) if (list.recording && list.submission > directory.initializationCount())
                for (uint64_t i = 0; i < list.count; ++i) {
                    const auto record = submissions.recordings()[*list.recording].records[size_t(i)];
                    const auto type = directory.packets()[size_t(record - 1)].type;
                    if (type == 0x1ae3c190d8b1eULL || type == 0x1ae3c86b1c3b9ULL) draws.push_back(list.firstExecution + i);
                }
            QVERIFY(draws.size() >= 20);
            std::vector<double> milliseconds; size_t confirmed{};
            Json evidence = Json::array();
            for (size_t i = 0; i < 20; ++i) {
                const auto draw = draws[i * (draws.size() - 1) / 19];
                timer.restart(); const auto snapshot = index.snapshot(draw);
                evidence.push_back(snapshot);
                milliseconds.push_back(timer.nsecsElapsed() / 1e6);
                for (const auto &row : snapshot.at("pipeline_rows"))
                    if (row.at("resource").value("mapping_status", "") == "capture_resource_marker") ++confirmed;
            }
            std::sort(milliseconds.begin(), milliseconds.end());
            qInfo() << name << "index ms" << buildMs << "20 DC p50 ms" << milliseconds[9]
                    << "p95 ms" << milliseconds[18] << "confirmed rows" << confirmed;
            QVERIFY(confirmed > 0);
            const auto output = qEnvironmentVariable("FLORA_CPU_BINDING_OUTPUT");
            if (!output.isEmpty()) { QDir().mkpath(output); save(output + '/' + name + ".json", QByteArray::fromStdString(evidence.dump(2))); }
          } catch (const std::exception &e) { QFAIL(e.what()); }
        }
    }
    void recordingAndSubmissionSemantics() {
        QTemporaryDir temp; const auto path = temp.filePath("frame.gpa_frame"); QDir().mkpath(path + "/D3D12");
        QByteArray header(0x288, 0); const uint64_t type = 10, magic = 0x415047; const uint32_t version = 3;
        memcpy(header.data(), &type, 8); memcpy(header.data() + 8, &magic, 8); memcpy(header.data() + 16, &version, 4);
        save(path + "/metadata", header); save(path + "/resources", "resource");
        QByteArray packets; uint64_t serial{};
        auto packet = [&](uint64_t command, const QByteArray &body) {
            qword(packets, command); qword(packets, ++serial); qword(packets, 0); qword(packets, uint64_t(body.size())); packets += body; return serial;
        };
        auto resource = [&](uint64_t object) {
            QByteArray b; qword(b, 1); b.append(char(1));
            D3D12_HEAP_PROPERTIES properties{}; properties.Type = D3D12_HEAP_TYPE_DEFAULT; append(b, properties); dword(b, 0);
            D3D12_RESOURCE_DESC desc{}; desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; desc.Width = desc.Height = 16;
            desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1; desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            b.append(char(1)); append(b, desc); dword(b, 0); b.append(char(0)); output(b, __uuidof(ID3D12Resource), object);
            return packet(0x12793f58d8f79ULL, b);
        };
        const auto a = resource(100), b = resource(200);
        auto heap = [&](uint64_t object, D3D12_DESCRIPTOR_HEAP_TYPE kind, uint64_t cpu, uint64_t gpu) {
            QByteArray inc; qword(inc, 1); dword(inc, kind); dword(inc, 32); packet(0x127932ffcb166ULL, inc);
            QByteArray body; qword(body, 1); body.append(char(1));
            const D3D12_DESCRIPTOR_HEAP_DESC desc{kind, 1, gpu ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
            append(body, desc); output(body, __uuidof(ID3D12DescriptorHeap), object); packet(0x127932eb2eb14ULL, body);
            for (const auto &[address, command] : {std::pair{cpu, uint64_t(0x15a3eb3b8e877ULL)}, {gpu, uint64_t(0x15a3edf3ce2bbULL)}}) {
                if (!address) continue; QByteArray start; qword(start, object); start.append(char(1)); qword(start, address); packet(command, start);
            }
        };
        heap(50, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 0x1000, 0);
        heap(51, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 0x2000, 0x4000);
        auto view = [&](uint64_t object, bool srv) {
            QByteArray body; qword(body, 1); qword(body, object); body.append(char(1));
            if (srv) { D3D12_SHADER_RESOURCE_VIEW_DESC d{}; d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                d.Texture2D.MipLevels = 1; append(body, d); }
            else { D3D12_RENDER_TARGET_VIEW_DESC d{}; d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D; append(body, d); }
            qword(body, srv ? 0x2000 : 0x1000); packet(srv ? 0x127932c396f41ULL : 0x1279306e469b5ULL, body);
        };
        D3D12_DESCRIPTOR_RANGE1 range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_NONE, 0};
        D3D12_ROOT_PARAMETER1 param{}; param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        param.DescriptorTable = {1, &range}; param.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc{}; desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1; desc.Desc_1_1 = {1, &param, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        Microsoft::WRL::ComPtr<ID3DBlob> blob, errors;
        QVERIFY(SUCCEEDED(D3D12SerializeVersionedRootSignature(&desc, &blob, &errors)));
        QByteArray root; qword(root, 1); dword(root, 0); root.append(char(1)); qword(root, blob->GetBufferSize());
        root.append(static_cast<const char *>(blob->GetBufferPointer()), qsizetype(blob->GetBufferSize())); qword(root, blob->GetBufferSize());
        output(root, __uuidof(ID3D12RootSignature), 60); packet(0x127938051b445ULL, root);
        auto reset = [&] { QByteArray r; qword(r, 40); qword(r, 90); qword(r, 0); dword(r, 0); packet(0x1ae3c721c351aULL, r); };
        auto om = [&] { QByteArray r; qword(r, 40); dword(r, 1); r.append(char(1)); qword(r, 1); qword(r, 0x1000); dword(r, 1); r.append(char(0)); packet(0x1ae3c7884148bULL, r); };
        auto draw = [&] { QByteArray r; qword(r, 40); dword(r, 3); dword(r, 1); dword(r, 0); dword(r, 0); return packet(0x1ae3c190d8b1eULL, r); };
        auto close = [&] { QByteArray r; qword(r, 40); dword(r, 0); packet(0x1ae3cd6acf667ULL, r); };
        auto submit = [&] { QByteArray r; qword(r, 90); dword(r, 1); r.append(char(1)); qword(r, 1); qword(r, 40); packet(0x125482975df48ULL, r); };
        view(100, false); view(100, true); reset();
        QByteArray heaps; qword(heaps, 40); dword(heaps, 1); heaps.append(char(1)); qword(heaps, 1); qword(heaps, 51); packet(0x1ae3c4254a275ULL, heaps);
        QByteArray bindRoot; qword(bindRoot, 40); qword(bindRoot, 60); packet(0x1ae3c6959946eULL, bindRoot);
        QByteArray table; qword(table, 40); dword(table, 0); qword(table, 0x4000); packet(0x1ae3c0857cd39ULL, table);
        om(); const auto originalDraw = draw(); close();
        view(200, false); view(200, true); submit();
        view(100, true); submit(); // Same source recording, different descriptor execution state.
        reset(); om(); const auto nextDraw = draw(); close(); submit();
        const auto reused = resource(100); view(100, false); reset(); om(); const auto reusedDraw = draw(); close(); submit();
        save(path + "/thread-1-0", packets);
        dx12::StreamDirectory directory(std::filesystem::path(path.toStdWString())); dx12::SubmissionIndex submissions(directory);
        dx12::CpuBindings index(directory, submissions);
        std::vector<Json> original;
        for (uint64_t i = 1; i <= submissions.executionCount(); ++i) {
            const auto event = submissions.execution(i);
            if (event.record == originalDraw) original.push_back(index.snapshot(i));
            if (event.record == nextDraw) QCOMPARE(index.snapshot(i).at("pipeline_rows")[0].at("resource").at("resource_creation"), Json(std::to_string(b)));
            if (event.record == reusedDraw) QCOMPARE(index.snapshot(i).at("pipeline_rows")[0].at("resource").at("resource_creation"), Json(std::to_string(reused)));
        }
        QCOMPARE(original.size(), size_t(2));
        for (const auto &snapshot : original) QCOMPARE(snapshot.at("pipeline_rows")[0].at("resource").at("resource_creation"), Json(std::to_string(a)));
        QCOMPARE(original[0].at("pipeline_rows")[1].at("resource").at("resource_creation"), Json(std::to_string(b)));
        QCOMPARE(original[1].at("pipeline_rows")[1].at("resource").at("resource_creation"), Json(std::to_string(a)));
    }
};
QTEST_GUILESS_MAIN(CpuBindingTests)
#include "CpuBindingTests.moc"
