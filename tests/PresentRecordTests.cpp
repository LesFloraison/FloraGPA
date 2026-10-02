#include "SyntheticCapture.h"
#include "application/FrameValidation.h"
#include "core/PresentRecords.h"
#include <QTemporaryDir>
#include <QtTest>
using namespace flora;
using namespace flora::testing;
namespace {
using Raw = std::vector<uint8_t>;
template <class... T> Raw pack(T... values) {
    Raw raw;
    (append(raw, values), ...);
    return raw;
}
Raw present(uint32_t flags = 0) { return pack(Id(0), Id(2), int32_t(0), 0u, flags); }
Capture makeCapture(uint32_t effect = 3, uint32_t flags = 0) {
    Capture c;
    c.add(1, 5, 0x127, Raw(24));
    DXGI_SWAP_CHAIN_DESC chain{};
    chain.BufferDesc.Width = chain.BufferDesc.Height = 8;
    chain.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    chain.SampleDesc.Count = 1;
    chain.BufferCount = 2;
    chain.Windowed = TRUE;
    chain.SwapEffect = DXGI_SWAP_EFFECT(effect);
    chain.Flags = 0x800;
    c.add(2, 5, 0x38, pack(Id(0), Id(0), chain));
    D3D11_TEXTURE2D_DESC td{8,
                            8,
                            1,
                            1,
                            DXGI_FORMAT_R8G8B8A8_UNORM,
                            {1, 0},
                            D3D11_USAGE_DEFAULT,
                            D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE,
                            0,
                            0};
    c.add(3, 5, 0x85, pack(Id(0), Id(2), td, Id(0)));
    c.add(5, 5, 0x85, pack(Id(0), Id(0), td, Id(0)));
    D3D11_RENDER_TARGET_VIEW_DESC view{};
    view.Format = td.Format;
    view.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    c.add(4, 5, 0x8d, pack(Id(0), Id(0), Id(3), view));
    c.add(6, 5, 0x8d, pack(Id(0), Id(0), Id(5), view));
    c.add(100, 7, 0x32, pack(Id(0), Id(1), Id(4), uint8_t(1), std::array<float, 4>{1, 0, 0, 1}));
    c.add(101, 7, 0x34ff, pack(Id(0), Id(1), 2u, uint8_t(1), Id(6), Id(4), Id(0)));
    c.add(200, 7, 0x3257, present(flags));
    return c;
}
void replace(Capture &c, Id id, Raw payload) {
    auto it = std::find_if(c.entries.begin(), c.entries.end(), [&](const Entry &e) { return e.id == id; });
    it->offset = c.bytes.size();
    it->size = uint32_t(payload.size());
    c.bytes.insert(c.bytes.end(), payload.begin(), payload.end());
}
Raw raw(const Capture &c, Id id) {
    auto it = std::find_if(c.entries.begin(), c.entries.end(), [&](const Entry &e) { return e.id == id; });
    return Raw(c.bytes.begin() + it->offset, c.bytes.begin() + it->offset + it->size);
}
} // namespace
class PresentRecordTests final : public QObject {
    Q_OBJECT
  private slots:
    void checkedLayoutsAndRejections() {
        QTemporaryDir dir;
        auto check = [&](Capture c, bool accepted) {
            auto path = dir.filePath("present.gpa_frame");
            c.save(path);
            Frame f(path.toStdWString());
            if (accepted) {
                auto p = validatePresentRecord(f, 200);
                QCOMPARE(p.chain, Id(2));
            } else {
                QVERIFY_EXCEPTION_THROWN(validatePresentRecord(f, 200), std::runtime_error);
                auto report = validateFrame(path.toStdWString());
                QCOMPARE(report["status"], nlohmann::json("blocked"));
                bool located = false;
                for (auto &finding : report["findings"])
                    located |= finding["event_id"] == 200 && finding["severity"] == "error";
                QVERIFY(located);
            }
        };
        for (auto effect : {0u, 1u, 3u, 4u})
            for (auto flags : {0u, 1u})
                check(makeCapture(effect, flags), true);
        check(makeCapture(3, 0x200), true);
        auto bytes = present();
        for (size_t n = 0; n < bytes.size(); ++n) {
            auto c = makeCapture();
            replace(c, 200, Raw(bytes.begin(), bytes.begin() + n));
            check(c, false);
        }
        bytes.push_back(0);
        auto c = makeCapture();
        replace(c, 200, bytes);
        check(c, false);
        for (auto [offset, value] : std::vector<std::pair<size_t, uint32_t>>{
                 {0, 1}, {8, 999}, {16, 1}, {16, 0x887a0001}, {20, 5}, {24, 2}, {24, 8}, {24, 0x201}}) {
            c = makeCapture();
            bytes = present();
            put(bytes, offset, value);
            replace(c, 200, bytes);
            check(c, false);
        }
        for (auto [offset, value] : std::vector<std::pair<size_t, uint32_t>>{
                 {16, 0}, {20, 0}, {44, 0}, {48, 1}, {56, 1}, {72, 2}, {76, 99}}) {
            c = makeCapture();
            bytes = raw(c, 2);
            put(bytes, offset, value);
            replace(c, 2, bytes);
            check(c, false);
        }
        c = makeCapture();
        bytes = raw(c, 2);
        bytes.pop_back();
        replace(c, 2, bytes);
        check(c, false);
        c = makeCapture();
        bytes = raw(c, 2);
        bytes.push_back(0);
        replace(c, 2, bytes);
        check(c, false);
        c = makeCapture();
        bytes = raw(c, 3);
        put(bytes, 8, Id(0));
        replace(c, 3, bytes);
        check(c, false);
        c = makeCapture();
        c.add(7, 5, 0x85, raw(c, 3));
        check(c, false);
        c = makeCapture();
        bytes = raw(c, 3);
        put(bytes, 16, 9u);
        replace(c, 3, bytes);
        check(c, false);
        c = makeCapture();
        c.add(201, 7, 0x242, pack(Id(0), Id(1)));
        check(c, false);
        c = makeCapture(3, 1);
        c.add(201, 7, 0x242, pack(Id(0), Id(1)));
        check(c, true);
        for (auto [offset, value] : std::vector<std::pair<size_t, uint32_t>>{{72, 0}, {80, 0}}) {
            c = makeCapture(3, 0x200);
            bytes = raw(c, 2);
            put(bytes, offset, value);
            replace(c, 2, bytes);
            check(c, false);
        }
    }
    void selectiveUnbinding_data() {
        QTest::addColumn<bool>("warp");
        QTest::addColumn<uint32_t>("effect");
        QTest::addColumn<uint32_t>("flags");
        for (bool warp : {false, true})
            for (auto effect : {0u, 1u, 3u, 4u})
                for (auto flags : {0u, 1u})
                    QTest::newRow(qPrintable(QString("%1-%2-%3").arg(warp).arg(effect).arg(flags)))
                        << warp << effect << flags;
    }
    void selectiveUnbinding() {
        QFETCH(bool, warp);
        QFETCH(uint32_t, effect);
        QFETCH(uint32_t, flags);
        auto c = makeCapture(effect, flags);
        QTemporaryDir dir;
        auto path = dir.filePath("present.gpa_frame");
        c.save(path);
        Frame f(path.toStdWString());
        ReplayOptions options;
        options.warp = warp;
        Replay replay(f, options);
        for (int repeat = 0; repeat < 2; ++repeat) {
            bool observed = false;
            replay.run({}, {}, [&](Id event, bool after, ID3D11DeviceContext *context, const auto &) {
                if (event != 200)
                    return;
                ID3D11RenderTargetView *views[2]{};
                context->OMGetRenderTargets(2, views, nullptr);
                Com<ID3D11RenderTargetView> a, b;
                a.Attach(views[0]);
                b.Attach(views[1]);
                QVERIFY(a);
                QCOMPARE(bool(b), !after || flags == 1 || effect < 3);
                if (after)
                    observed = true;
            });
            QVERIFY(observed);
            QCOMPARE(replay.counts.at(flags == 1 ? "present_tests" : "Present"), uint64_t(1));
            auto image = replay.readTexture(3);
            QCOMPARE(image.size(), size_t(256));
            for (size_t i = 0; i < 256; i += 4) {
                QCOMPARE(image[i], uint8_t(255));
                QCOMPARE(image[i + 1], uint8_t(0));
            }
        }
    }
    void malformedProductionFails() {
        auto c = makeCapture();
        auto bytes = present();
        bytes.push_back(0);
        replace(c, 200, bytes);
        QTemporaryDir dir;
        auto path = dir.filePath("bad.gpa_frame");
        c.save(path);
        Frame f(path.toStdWString());
        ReplayOptions options;
        options.warp = true;
        Replay replay(f, options);
        QVERIFY_EXCEPTION_THROWN(replay.run(), std::runtime_error);
    }
    void uavUnbindingPreservesCounters_data() {
        QTest::addColumn<bool>("warp");
        QTest::addColumn<bool>("om");
        for (bool warp : {false, true})
            for (bool om : {false, true})
                QTest::newRow(qPrintable(QString("%1-%2").arg(warp).arg(om))) << warp << om;
    }
    void uavUnbindingPreservesCounters() {
        QFETCH(bool, warp);
        QFETCH(bool, om);
        auto c = makeCapture();
        auto texture = raw(c, 3);
        put(texture, 48,
            UINT(D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS));
        replace(c, 3, texture);
        D3D11_UNORDERED_ACCESS_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        view.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        c.add(7, 5, 0x8f, pack(Id(0), Id(0), Id(3), view));
        c.buffer(10, 11, D3D11_BIND_UNORDERED_ACCESS, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, {});
        c.uav(12, 10, D3D11_BUFFER_UAV_FLAG_COUNTER);
        replace(c, 101, pack(Id(0), Id(1), 1u, uint8_t(1), Id(6), Id(0)));
        auto binding = pack(Id(0), Id(1));
        if (om) {
            append(binding, UINT_MAX);
            append(binding, uint8_t(0));
            append(binding, Id(0));
        }
        auto tail = pack(3u, 2u, uint8_t(1), Id(7), Id(12), uint8_t(1), UINT_MAX, 17u);
        binding.insert(binding.end(), tail.begin(), tail.end());
        c.add(102, 7, om ? 0x3500 : 0x3522, binding);
        QTemporaryDir dir;
        auto path = dir.filePath("uav.gpa_frame");
        c.save(path);
        Frame f(path.toStdWString());
        ReplayOptions options;
        options.warp = warp;
        Replay replay(f, options);
        bool observed = false;
        replay.run({}, {}, [&](Id id, bool after, ID3D11DeviceContext *context, const auto &) {
            if (id != 200 || !after)
                return;
            ID3D11UnorderedAccessView *views[2]{};
            if (om)
                context->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 3, 2, views);
            else
                context->CSGetUnorderedAccessViews(3, 2, views);
            Com<ID3D11UnorderedAccessView> a, b;
            a.Attach(views[0]);
            b.Attach(views[1]);
            QVERIFY(!a);
            QVERIFY(b);
            Com<ID3D11RenderTargetView> rt;
            context->OMGetRenderTargets(1, &rt, nullptr);
            QVERIFY(rt);
            Com<ID3D11Device> device;
            context->GetDevice(&device);
            D3D11_BUFFER_DESC desc{4, D3D11_USAGE_DEFAULT, 0, 0, 0, 0};
            Com<ID3D11Buffer> value, staging;
            QCOMPARE(device->CreateBuffer(&desc, nullptr, &value), S_OK);
            desc.Usage = D3D11_USAGE_STAGING;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            QCOMPARE(device->CreateBuffer(&desc, nullptr, &staging), S_OK);
            context->CopyStructureCount(value.Get(), 0, b.Get());
            context->CopyResource(staging.Get(), value.Get());
            D3D11_MAPPED_SUBRESOURCE mapped{};
            QCOMPARE(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), S_OK);
            auto count = *static_cast<uint32_t *>(mapped.pData);
            context->Unmap(staging.Get(), 0);
            QCOMPARE(count, 17u);
            observed = true;
        });
        QVERIFY(observed);
    }
    void originalCaptures() {
        auto root = qEnvironmentVariable("FLORA_PRESENT_CAPTURES");
        if (root.isEmpty())
            QSKIP("Set FLORA_PRESENT_CAPTURES to the unmodified producer output");
        for (int mode = 0; mode < 5; ++mode)
            for (bool warp : {false, true}) {
                auto path = root + QString("/%1/capture.gpa_frame").arg(mode);
                Frame f(path.toStdWString());
                ReplayOptions options;
                options.warp = warp;
                Replay replay(f, options);
                unsigned observed = 0;
                replay.run({}, {}, [&](Id id, bool after, ID3D11DeviceContext *context, const auto &) {
                    if (!after || f.entry(id).type != 0x3257)
                        return;
                    auto p = validatePresentRecord(f, id);
                    Com<ID3D11RenderTargetView> view;
                    context->OMGetRenderTargets(1, &view, nullptr);
                    QCOMPARE(bool(view), p.test || mode == 2);
                    ++observed;
                });
                QCOMPARE(observed, mode == 0 ? 2u : 1u);
            }
    }
};
QTEST_GUILESS_MAIN(PresentRecordTests)
#include "PresentRecordTests.moc"
