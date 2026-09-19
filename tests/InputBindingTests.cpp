#include "StateCapture.h"
#include <QDir>
#include <QTemporaryDir>
#include <QtTest>
#include <nlohmann/json.hpp>
using namespace flora;
using namespace flora::testing;
using Json = nlohmann::json;
namespace {
void command(Capture &c, Id id, uint16_t type, std::vector<uint8_t> tail = {}, Id owner = 1) {
    auto raw = statePack(Id(0), owner);
    raw.insert(raw.end(), tail.begin(), tail.end());
    c.add(id, 7, type, raw);
}
Capture inputs() {
    auto c = computeCapture();
    std::erase_if(c.entries, [](const Entry &e) { return e.category == 7; });
    D3D11_SAMPLER_DESC desc{};
    desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    desc.AddressU = desc.AddressV = desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    desc.MaxLOD = D3D11_FLOAT32_MAX;
    c.add(20, 5, 0x88, statePack(Id(0), Id(0), desc));
    c.buffer(22, 23, D3D11_BIND_VERTEX_BUFFER, 0, {});
    c.buffer(24, 25, D3D11_BIND_INDEX_BUFFER, 0, {});
    c.buffer(26, 27, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
             D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, {});
    D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    srv.Buffer.NumElements = 4;
    c.add(28, 5, 0x8c, statePack(Id(0), Id(0), Id(26), srv));
    c.uav(29, 26, 0);
    const char source[] = "float4 main(float4 p:POSITION):SV_Position{return p;}";
    Com<ID3DBlob> binary;
    check(D3DCompile(source, sizeof(source) - 1, nullptr, nullptr, nullptr, "main", "vs_5_0", 0, 0, &binary,
                     nullptr),
          "Compile IA signature");
    c.add(30, 9, 1, statePack(9u, std::array<char, 9>{'P', 'O', 'S', 'I', 'T', 'I', 'O', 'N', 0}));
    auto layout = statePack(1u, Id(30), 0u, UINT(DXGI_FORMAT_R32G32B32A32_FLOAT), 31u, 0u, 0u, 0u,
                            UINT(binary->GetBufferSize()));
    auto first = static_cast<const uint8_t *>(binary->GetBufferPointer());
    layout.insert(layout.end(), first, first + binary->GetBufferSize());
    c.add(31, 9, 0x84, layout);
    c.add(32, 5, 0x82, statePack(Id(0), Id(0), Id(31)));
    command(c, 100, 0x242);
    command(c, 101, 0x34f0, statePack(31u, 1u, uint8_t(1), Id(22), uint8_t(1), 16u, uint8_t(1), 4u));
    command(c, 102, 0x34f1, statePack(Id(24), UINT(DXGI_FORMAT_R16_UINT), 2u));
    command(c, 103, 0x34ef, statePack(Id(32)));
    const unsigned samplers[]{26, 61, 65, 32, 10, 70}, srvs[]{25, 59, 63, 31, 8, 67};
    for (unsigned s = 0; s < 6; ++s) {
        command(c, 110 + s * 2, uint16_t(0x34de + samplers[s]),
                statePack(14u, 2u, uint8_t(1), Id(20), Id(0)));
        command(c, 111 + s * 2, uint16_t(0x34de + srvs[s]), statePack(126u, 2u, uint8_t(1), Id(6), Id(0)));
        command(c, 130 + s, uint16_t(0x24f + s),
                statePack(13u, 1u, uint8_t(1), Id(2), uint8_t(1), 16u, uint8_t(1), 16u));
        command(c, 140 + s, uint16_t(0x249 + s), statePack(13u, 1u, uint8_t(1), Id(2)));
    }
    command(c, 150, 0x242);
    command(c, 151, 0x34f0, statePack(31u, 0u, uint8_t(0), uint8_t(0), uint8_t(0)));
    command(c, 152, 0x34e6, statePack(127u, 0u, uint8_t(0)));
    command(c, 153, 0x34e8, statePack(15u, 0u, uint8_t(1)));
    return c;
}
Capture gaps() {
    auto c = inputs();
    command(c, 200, 0x34e6, statePack(2u, 2u, uint8_t(1), Id(999), Id(6)));
    command(c, 201, 0x34e6, statePack(2u, 1u, uint8_t(1), Id(0)));
    command(c, 202, 0x34f7, statePack(3u, 1u, uint8_t(1), Id(0)));
    command(c, 203, 0x34e6, statePack(3u, 1u, uint8_t(1), Id(6)));
    command(c, 204, 0x34ef, statePack(Id(999)));
    command(c, 205, 0x34ef, statePack(Id(0)));
    command(c, 206, 0x34ff, statePack(1u, uint8_t(1), Id(999), Id(0)));
    command(c, 207, 0x34ff, statePack(0u, uint8_t(0), Id(0)));
    command(c, 208, 0x242);
    command(c, 209, 0x34e6, statePack(127u, 1u, uint8_t(1), Id(999)));
    c.add(210, 7, 0x35, statePack(Id(17), Id(0), Id(1), 1u, 1u, 1u));
    command(c, 211, 0x242);
    command(c, 220, 0x3522, statePack(0u, 1u, uint8_t(1), Id(29), uint8_t(0)));
    command(c, 221, 0x34e6, statePack(127u, 1u, uint8_t(1), Id(28)));
    command(c, 222, 0x3522, statePack(0u, 1u, uint8_t(1), Id(0), uint8_t(0)));
    command(c, 223, 0x34e6, statePack(127u, 1u, uint8_t(1), Id(28)));
    command(c, 224, 0x3522, statePack(0u, 1u, uint8_t(1), Id(29), uint8_t(0)));
    command(c, 225, 0x242);
    command(c, 226, 0x3500,
            statePack(UINT32_MAX, uint8_t(0), Id(999), 0u, UINT32_MAX, uint8_t(0), uint8_t(0)));
    return c;
}
Json bindings(Replay &replay) {
    Json result = Json::object();
    replay.inspectNativeState([&](ID3D11DeviceContext *ctx, const auto &objects) {
        auto identity = [&](IUnknown *value) -> Id {
            if (!value)
                return 0;
            Com<IUnknown> identity;
            check(value->QueryInterface(IID_PPV_ARGS(&identity)), "Get identity");
            for (const auto &[id, object] : objects) {
                Com<IUnknown> other;
                check(object.As(&other), "Get captured identity");
                if (identity == other)
                    return id;
            }
            throw std::runtime_error("Unmapped native binding in test");
        };
        Com<ID3D11InputLayout> layout;
        ctx->IAGetInputLayout(&layout);
        result["input_layout"] = identity(layout.Get());
        Com<ID3D11Buffer> ib;
        DXGI_FORMAT format;
        UINT offset;
        ctx->IAGetIndexBuffer(&ib, &format, &offset);
        result["ib"] = identity(ib.Get());
        result["ib_format"] = UINT(format);
        result["ib_offset"] = offset;
        for (UINT i = 0; i < 32; ++i) {
            Com<ID3D11Buffer> vb;
            UINT stride;
            ctx->IAGetVertexBuffers(i, 1, &vb, &stride, &offset);
            result["vb." + std::to_string(i)] = identity(vb.Get());
            result["strides." + std::to_string(i)] = stride;
            result["offsets." + std::to_string(i)] = offset;
        }
        using Sam = void (STDMETHODCALLTYPE ID3D11DeviceContext::*)(UINT, UINT, ID3D11SamplerState **);
        using Srv = void (STDMETHODCALLTYPE ID3D11DeviceContext::*)(UINT, UINT, ID3D11ShaderResourceView **);
        using Cb =
            void (STDMETHODCALLTYPE ID3D11DeviceContext1::*)(UINT, UINT, ID3D11Buffer **, UINT *, UINT *);
        const Sam sam[]{&ID3D11DeviceContext::VSGetSamplers, &ID3D11DeviceContext::HSGetSamplers,
                        &ID3D11DeviceContext::DSGetSamplers, &ID3D11DeviceContext::GSGetSamplers,
                        &ID3D11DeviceContext::PSGetSamplers, &ID3D11DeviceContext::CSGetSamplers};
        const Srv srv[]{
            &ID3D11DeviceContext::VSGetShaderResources, &ID3D11DeviceContext::HSGetShaderResources,
            &ID3D11DeviceContext::DSGetShaderResources, &ID3D11DeviceContext::GSGetShaderResources,
            &ID3D11DeviceContext::PSGetShaderResources, &ID3D11DeviceContext::CSGetShaderResources};
        const Cb cb[]{
            &ID3D11DeviceContext1::VSGetConstantBuffers1, &ID3D11DeviceContext1::HSGetConstantBuffers1,
            &ID3D11DeviceContext1::DSGetConstantBuffers1, &ID3D11DeviceContext1::GSGetConstantBuffers1,
            &ID3D11DeviceContext1::PSGetConstantBuffers1, &ID3D11DeviceContext1::CSGetConstantBuffers1};
        Com<ID3D11DeviceContext1> ctx1;
        check(ctx->QueryInterface(IID_PPV_ARGS(&ctx1)), "Get context1");
        const std::string stages[]{"vs", "hs", "ds", "gs", "ps", "cs"};
        for (size_t stage = 0; stage < 6; ++stage) {
            for (UINT slot = 0; slot < 16; ++slot) {
                Com<ID3D11SamplerState> value;
                (ctx->*sam[stage])(slot, 1, &value);
                result[stages[stage] + ".samplers." + std::to_string(slot)] = identity(value.Get());
            }
            for (UINT slot = 0; slot < 128; ++slot) {
                Com<ID3D11ShaderResourceView> value;
                (ctx->*srv[stage])(slot, 1, &value);
                result[stages[stage] + ".srv." + std::to_string(slot)] = identity(value.Get());
            }
            for (UINT slot = 0; slot < 14; ++slot) {
                Com<ID3D11Buffer> value;
                UINT first, count;
                (ctx1.Get()->*cb[stage])(slot, 1, &value, &first, &count);
                result[stages[stage] + ".cb." + std::to_string(slot)] = identity(value.Get());
                result[stages[stage] + ".cb_range." + std::to_string(slot)] = {first, count};
            }
        }
    });
    return result;
}
void evidence(Capture &capture, const QString &name, const Json &rows) {
    auto directory = qEnvironmentVariable("FLORA_BINDING_ARTIFACT_DIR");
    if (directory.isEmpty())
        return;
    QDir().mkpath(directory);
    capture.save(directory + "/" + name + ".gpa_frame");
    QFile file(directory + "/" + name + ".json");
    if (!file.open(QIODevice::WriteOnly))
        throw std::runtime_error("Cannot export native bindings");
    file.write(QByteArray::fromStdString(rows.dump(2)));
}
} // namespace
class InputBindingTests final : public QObject {
    Q_OBJECT
  private slots:
    void realBoundaries() {
        auto directory = qEnvironmentVariable("FLORA_TEST_CAPTURE_DIR");
        if (directory.isEmpty())
            QSKIP("External captures not configured");
        struct Case {
            const char *file;
            std::vector<Id> events;
        };
        const Case cases[]{{"GF2_Exilium_2026_03_03__00_19_35.gpa_frame", {79, 430}},
                           {"bf1_2026_01_21__16_53_05.gpa_frame", {20434, 25572}}};
        Json rows = Json::array();
        for (const auto &test : cases) {
            auto path = directory + "/" + test.file;
            Frame frame(path.toStdWString());
            for (Id event : test.events) {
                for (bool before : {true, false}) {
                    ReplayOptions options;
                    options.until = event;
                    options.before = before;
                    Replay replay(frame, options);
                    Json row{{"capture", test.file}, {"event", event}, {"before", before}};
                    if (event == 25572 && !before) {
                        QVERIFY_THROWS_EXCEPTION(std::runtime_error, replay.run());
                        row["error"] = true;
                    } else {
                        replay.run();
                        row["values"] = bindings(replay);
                    }
                    rows.push_back(std::move(row));
                }
            }
        }
        auto output = qEnvironmentVariable("FLORA_BINDING_ARTIFACT_DIR");
        if (!output.isEmpty()) {
            QDir().mkpath(output);
            QFile file(output + "/real.json");
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write(QByteArray::fromStdString(rows.dump(2)));
        }
    }
    void nativeBoundaries_data() {
        QTest::addColumn<bool>("warp");
        QTest::newRow("hardware") << false;
        QTest::newRow("warp") << true;
    }
    void nativeBoundaries() {
        QFETCH(bool, warp);
        auto capture = inputs();
        QTemporaryDir dir;
        capture.save(dir.path() + "/inputs.gpa_frame");
        Frame frame((dir.path() + "/inputs.gpa_frame").toStdWString());
        Json rows = Json::array();
        for (Id event : {101, 102, 103, 121, 135, 145, 150, 153}) {
            for (bool before : {true, false}) {
                ReplayOptions options;
                options.warp = warp;
                options.until = event;
                options.before = before;
                Replay replay(frame, options);
                QVERIFY_THROWS_EXCEPTION(std::runtime_error, bindings(replay));
                replay.run();
                auto values = bindings(replay);
                rows.push_back({{"event", event}, {"before", before}, {"values", values}});
                if (event == 101)
                    QCOMPARE(values["vb.31"], Json(before ? 0 : 22));
                if (event == 102)
                    QCOMPARE(values["ib"], Json(before ? 0 : 24));
                if (event == 103)
                    QCOMPARE(values["input_layout"], Json(before ? 0 : 32));
                if (event == 121 && !before) {
                    for (std::string stage : {"vs", "hs", "ds", "gs", "ps", "cs"}) {
                        QCOMPARE(values[stage + ".srv.126"], Json(6));
                        QCOMPARE(values[stage + ".srv.127"], Json(0));
                        QCOMPARE(values[stage + ".samplers.14"], Json(20));
                    }
                }
                if (event == 135 && !before)
                    QCOMPARE(values["cs.cb_range.13"], Json({16, 16}));
                if (event == 145 && !before)
                    QCOMPARE(values["cs.cb_range.13"], Json({0, 4096}));
                if (event == 153) {
                    QCOMPARE(values["input_layout"], Json(0));
                    QCOMPARE(values["vb.31"], Json(0));
                    QCOMPARE(values["ps.srv.126"], Json(0));
                }
                replay.run();
                QCOMPARE(bindings(replay), values);
            }
        }
        evidence(capture, warp ? "inputs-warp" : "inputs-hardware", rows);
    }
    void unresolvedAndHazards_data() { nativeBoundaries_data(); }
    void unresolvedAndHazards() {
        QFETCH(bool, warp);
        auto capture = gaps();
        QTemporaryDir dir;
        capture.save(dir.path() + "/gaps.gpa_frame");
        Frame frame((dir.path() + "/gaps.gpa_frame").toStdWString());
        Json rows = Json::array();
        for (Id event : {200, 201, 202, 203, 204, 205, 206, 207, 208, 209, 210, 221, 223, 224, 225, 226}) {
            for (bool before : {true, false}) {
                ReplayOptions options;
                options.warp = warp;
                options.until = event;
                options.before = before;
                Replay replay(frame, options);
                // A selected dispatch prepares its complete snapshot even at the before boundary.
                const Id last = before && event != 210 ? event - 1 : event;
                const bool unresolved =
                    (last >= 200 && last <= 202) || last == 204 || last == 206 || last == 207 || last == 209;
                if (unresolved) {
                    QVERIFY_THROWS_EXCEPTION(std::runtime_error, replay.run());
                    QVERIFY_THROWS_EXCEPTION(std::runtime_error, bindings(replay));
                    rows.push_back({{"event", event}, {"before", before}, {"error", true}});
                } else {
                    replay.run();
                    auto values = bindings(replay);
                    rows.push_back({{"event", event}, {"before", before}, {"values", values}});
                    if (!before && (event == 221 || event == 224))
                        QCOMPARE(values["ps.srv.127"], Json(0));
                    if (!before && event == 223)
                        QCOMPARE(values["ps.srv.127"], Json(28));
                    if (event == 210)
                        QCOMPARE(values["cs.srv.0"], Json(6));
                }
            }
        }
        // Full-frame compatibility is retained; inspectors cannot read an unresolved final boundary.
        auto incomplete = inputs();
        command(incomplete, 300, 0x34ef, statePack(Id(999)));
        incomplete.save(dir.path() + "/incomplete.gpa_frame");
        Frame missing((dir.path() + "/incomplete.gpa_frame").toStdWString());
        ReplayOptions options;
        options.warp = warp;
        Replay whole(missing, options);
        whole.run();
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, bindings(whole));
        evidence(capture, warp ? "gaps-warp" : "gaps-hardware", rows);
    }
    void malformedBindings() {
        struct Case {
            uint16_t type;
            std::vector<uint8_t> raw;
        };
        const std::vector<Case> cases{
            {0x34ff, statePack(UINT32_MAX, uint8_t(0), Id(0))},
            {0x3522, statePack(0u, UINT32_MAX, uint8_t(0), uint8_t(0))},
            {0x3500, statePack(1u, uint8_t(1), Id(0), Id(0), 0u, 0u, uint8_t(0), uint8_t(0))},
            {0x34ff, statePack(1u, uint8_t(1), Id(20), Id(0))},
            {0x34e6, statePack(128u, 0u, uint8_t(0))},
            {0x34e6, statePack(127u, 2u, uint8_t(1), Id(6), Id(6))},
            {0x34e6, statePack(0u, 1u, uint8_t(0))},
            {0x34e6, statePack(0u, 1u, uint8_t(2), Id(6))},
            {0x34e6, statePack(0u, 1u, uint8_t(1), Id(20))},
            {0x34e6, statePack(0u, 1u, uint8_t(1), Id(6), uint8_t(0))},
            {0x34e8, statePack(16u, 0u, uint8_t(0))},
            {0x34e8, statePack(0u, 1u, uint8_t(0))},
            {0x34e8, statePack(0u, 1u, uint8_t(1), Id(6))},
            {0x34e8, statePack(0u, 1u, uint8_t(1), Id(999))},
            {0x34ef, statePack(Id(6))},
            {0x34ef, statePack(Id(32), uint8_t(0))},
            {0x34f1, statePack(Id(24), 28u, 0u)},
            {0x34f1, statePack(Id(22), 57u, 0u)},
            {0x34f0, statePack(31u, 2u, uint8_t(0), uint8_t(0), uint8_t(0))},
            {0x34f0, statePack(0u, 1u, uint8_t(1), Id(22), uint8_t(0), uint8_t(1), 0u)},
            {0x34f0, statePack(0u, 1u, uint8_t(1), Id(22), uint8_t(1), 2049u, uint8_t(1), 0u)},
            {0x34f0, statePack(0u, 1u, uint8_t(1), Id(24), uint8_t(1), 4u, uint8_t(1), 0u)},
            {0x249, statePack(0u, 1u, uint8_t(1), Id(22))},
            {0x24f, statePack(0u, 0u, uint8_t(0), uint8_t(1), uint8_t(0))},
            {0x24f, statePack(0u, 1u, uint8_t(1), Id(2), uint8_t(1), 1u, uint8_t(1), 16u)},
            {0x24f, statePack(0u, 1u, uint8_t(1), Id(2), uint8_t(1), 16u, uint8_t(1), 4112u)}};
        QTemporaryDir dir;
        unsigned index = 0;
        for (const auto &test : cases) {
            auto capture = inputs();
            command(capture, 300, test.type, test.raw);
            auto path = dir.path() + "/bad.gpa_frame";
            capture.save(path);
            Frame frame(path.toStdWString());
            ReplayOptions options;
            options.warp = true;
            Replay replay(frame, options);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, replay.run());
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, bindings(replay));
            evidence(capture, "invalid-" + QString::number(index++), Json::array());
        }
        auto deferred = inputs();
        deferred.add(99, 5, 0x99, statePack(Id(0), Id(0), 1u, 0u));
        command(deferred, 300, 0x34e8, statePack(0u, 0u, uint8_t(0)), 99);
        deferred.save(dir.path() + "/deferred.gpa_frame");
        Frame frame((dir.path() + "/deferred.gpa_frame").toStdWString());
        ReplayOptions options;
        options.warp = true;
        Replay replay(frame, options);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, replay.run());
        evidence(deferred, "deferred", Json::array());
    }
};
QTEST_GUILESS_MAIN(InputBindingTests)
#include "InputBindingTests.moc"
