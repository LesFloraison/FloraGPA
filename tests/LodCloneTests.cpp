#include "SyntheticCapture.h"
#include "application/Experiment.h"
#include "application/FrameValidation.h"
#include "core/ResourceLod.h"
#include <QTemporaryDir>
#include <QtTest>
using namespace flora;
namespace {
std::vector<uint8_t> read(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        throw std::runtime_error("Missing clone oracle");
    const auto bytes = file.readAll();
    return {bytes.begin(), bytes.end()};
}
Com<ID3D11Resource> nativeResource(Replay &replay, Id id) {
    Com<ID3D11Resource> result;
    replay.inspectNativeState(
        [&](auto *, const auto &objects) { check(objects.at(id).As(&result), "Clone oracle resource"); });
    return result;
}
UINT flags(ID3D11Resource *r) {
    D3D11_RESOURCE_DIMENSION kind;
    r->GetType(&kind);
    if (kind == D3D11_RESOURCE_DIMENSION_TEXTURE1D) {
        Com<ID3D11Texture1D> t;
        check(r->QueryInterface(IID_PPV_ARGS(&t)), "Clone Texture1D");
        D3D11_TEXTURE1D_DESC d;
        t->GetDesc(&d);
        return d.MiscFlags;
    }
    if (kind == D3D11_RESOURCE_DIMENSION_TEXTURE3D) {
        Com<ID3D11Texture3D> t;
        check(r->QueryInterface(IID_PPV_ARGS(&t)), "Clone Texture3D");
        D3D11_TEXTURE3D_DESC d;
        t->GetDesc(&d);
        return d.MiscFlags;
    }
    Com<ID3D11Texture2D> t;
    check(r->QueryInterface(IID_PPV_ARGS(&t)), "Clone Texture2D");
    D3D11_TEXTURE2D_DESC d;
    t->GetDesc(&d);
    return d.MiscFlags;
}
void cases() {
    QTest::addColumn<int>("mode");
    QTest::addColumn<bool>("warp");
    for (int mode = 0; mode < 12; ++mode)
        for (bool warp : {false, true})
            QTest::newRow(qPrintable(QString("%1-%2").arg(mode).arg(warp))) << mode << warp;
}
} // namespace
class LodCloneTests final : public QObject {
    Q_OBJECT
  private slots:
    void originals_data() { cases(); }
    void originals() {
        QFETCH(int, mode);
        QFETCH(bool, warp);
        const auto root = qEnvironmentVariable("FLORA_LOD_CLONE_CAPTURES");
        if (root.isEmpty())
            QSKIP("Set original clone corpus");
        try {
            const auto folder = root + QString("/%1/").arg(mode);
            Frame frame((folder + "capture.gpa_frame").toStdWString());
            const auto report = validateFrame(frame.path());
            QVERIFY2(report["errors"] == 0, report.dump(2).c_str());
            std::vector<Id> draws;
            for (const auto &[id, e] : frame.entries())
                if (e.category == 7 && e.type == 0x37)
                    draws.push_back(id);
            QCOMPARE(draws.size(), size_t(6));
            for (unsigned v = 0; v < 6; ++v) {
                ReplayOptions options;
                options.warp = warp;
                options.until = draws[v];
                Replay replay(frame, options);
                replay.run();
                const auto expected =
                    read(folder + "hardware/" +
                         (v < 5 ? QString("expected-clone-%1.rgba").arg(v) : "expected.rgba"));
                QCOMPARE(replay.output().rgba, expected);
                const auto event = frame.event(draws[v]);
                const auto state = frame.state(event.state);
                Reader view(frame.payload(state.stages[4].srv[0], 5, 0x8c));
                view.skip(16);
                const auto id = view.read<Id>();
                const auto bytes =
                    read(folder + "hardware/" +
                         (v < 5 ? QString("expected-clone-%1.bin").arg(v) : "expected-storage.bin"));
                QCOMPARE(replay.readTexture(id), bytes);
                const auto texture = nativeResource(replay, id);
                QVERIFY(flags(texture.Get()) & D3D11_RESOURCE_MISC_RESOURCE_CLAMP);
                const float lods[]{0, .75f, 1, 4};
                replay.inspectNativeState([&](auto *context, const auto &) {
                    QCOMPARE(context->GetResourceMinLOD(texture.Get()), v == 4 ? 0.f : lods[mode % 4]);
                });
            }
        } catch (const std::exception &e) {
            QFAIL(e.what());
        }
    }
    void experiments_data() { cases(); }
    void experiments() {
        QFETCH(int, mode);
        QFETCH(bool, warp);
        const auto root = qEnvironmentVariable("FLORA_LOD_CLONE_CAPTURES");
        const auto base = qEnvironmentVariable("FLORA_LOD_STORAGE_CAPTURES");
        if (root.isEmpty() || base.isEmpty())
            QSKIP("Set original storage and clone corpora");
        try {
            const auto folder = root + QString("/%1/hardware/").arg(mode);
            Frame frame((base + QString("/%1/capture.gpa_frame").arg(mode)).toStdWString());
            const auto original = read(folder + "expected-storage.bin"),
                       image = read(folder + "expected.rgba");
            const auto subs = textureSubresources(frame.resource(10));
            const float lods[]{0, .75f, 1, 4};
            const float lod = lods[mode % 4];
            QTemporaryDir directory;
            for (unsigned v = 0; v < 4; ++v) {
                const auto editedStorage = read(folder + QString("expected-clone-%1.bin").arg(v));
                const auto editedImage = read(folder + QString("expected-clone-%1.rgba").arg(v));
                TexturePatch patch;
                patch.mip = v ? v - 1 : 0;
                const auto sub = subs[patch.mip];
                patch.bytes.assign(editedStorage.begin() + size_t(sub.offset),
                                   editedStorage.begin() + size_t(sub.offset + sub.size));
                Experiment experiment(frame);
                experiment.setTexturePatch(frame, 25, 10, patch, false);
                const auto document = experiment.document();
                auto invalid = patch;
                invalid.bytes.pop_back();
                QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                         experiment.setTexturePatch(frame, 25, 10, invalid, false));
                QCOMPARE(experiment.document(), document);
                experiment.save(directory.path() + "/project.flora.json");
                Experiment loaded(frame);
                loaded.load(directory.path() + "/project.flora.json", frame);
                QCOMPARE(loaded.document(), document);
                ReplayOptions options;
                loaded.apply(frame, options);
                options.warp = warp;
                options.until = 25;
                options.before = true;
                Replay preview(frame, options);
                preview.run();
                const auto source = nativeResource(preview, 10);
                auto originalState = [&] {
                    QCOMPARE(nativeResource(preview, 10).Get(), source.Get());
                    QCOMPARE(preview.readTexture(10), original);
                    preview.inspectNativeState([&](auto *context, const auto &) {
                        QCOMPARE(context->GetResourceMinLOD(source.Get()), lod);
                    });
                };
                originalState();
                preview.inspectEventInputs(25, [&] {
                    const auto clone = nativeResource(preview, 10);
                    QVERIFY(clone.Get() != source.Get());
                    QVERIFY(flags(clone.Get()) & D3D11_RESOURCE_MISC_RESOURCE_CLAMP);
                    QCOMPARE(preview.readTexture(10), editedStorage);
                    preview.inspectNativeState([&](auto *context, const auto &) {
                        QCOMPARE(context->GetResourceMinLOD(clone.Get()), lod);
                        QCOMPARE(context->GetResourceMinLOD(source.Get()), lod);
                    });
                });
                originalState();
                QVERIFY_THROWS_EXCEPTION(std::runtime_error, preview.inspectEventInputs(25, [] {
                    throw std::runtime_error("Inspection control");
                }));
                originalState();
                options.until = 0;
                options.before = false;
                Replay replay(frame, options);
                replay.run();
                QCOMPARE(replay.output().rgba, editedImage);
                QCOMPARE(replay.readTexture(10), original);
                replay.run();
                QCOMPARE(replay.output().rgba, editedImage);
                loaded.undo();
                loaded.apply(frame, options);
                Replay undo(frame, options);
                undo.run();
                QCOMPARE(undo.output().rgba, image);
                loaded.redo();
                loaded.apply(frame, options);
                Replay redo(frame, options);
                redo.run();
                QCOMPARE(redo.output().rgba, editedImage);
                for (bool after : {false, true}) {
                    Replay failed(frame, options);
                    QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                             failed.run({}, [&](Id, bool submitted, auto *, const auto &) {
                                                 if (submitted == after)
                                                     throw std::runtime_error("Submission control");
                                             }));
                    QCOMPARE(failed.readTexture(10), original);
                    // Native inspection deliberately rejects incomplete runs. Verify recovery
                    // through the supported retry path before inspecting the original resource.
                    QVERIFY_THROWS_EXCEPTION(std::runtime_error, nativeResource(failed, 10));
                    failed.run();
                    QCOMPARE(failed.output().rgba, editedImage);
                    QCOMPARE(failed.readTexture(10), original);
                    auto restored = nativeResource(failed, 10);
                    failed.inspectNativeState([&](auto *context, const auto &) {
                        QCOMPARE(context->GetResourceMinLOD(restored.Get()), lod);
                    });
                }
                options.disabled.insert(25);
                Replay disabled(frame, options);
                disabled.run();
                std::vector<uint8_t> clear(8 * 8 * 4, 0);
                for (size_t p = 3; p < clear.size(); p += 4)
                    clear[p] = 255;
                QCOMPARE(disabled.output().rgba, clear);
                QCOMPARE(disabled.readTexture(10), original);
            }
        } catch (const std::exception &e) {
            QFAIL(e.what());
        }
    }
    void liveState_data() {
        QTest::addColumn<bool>("warp");
        QTest::addColumn<bool>("disableSetter");
        for (bool warp : {false, true})
            for (bool disabled : {false, true})
                QTest::newRow(qPrintable(QString("%1-%2").arg(warp).arg(disabled))) << warp << disabled;
    }
    void liveState() {
        QFETCH(bool, warp);
        QFETCH(bool, disableSetter);
        const auto root = qEnvironmentVariable("FLORA_MINLOD_BOUNDARIES");
        if (root.isEmpty())
            QSKIP("Set original resource LOD boundary corpus");
        try {
            Frame frame((root + "/8/capture.gpa_frame").toStdWString());
            const auto audit = auditResourceLod(frame);
            QCOMPARE(audit.clamped.size(), size_t(1));
            const auto resource = *audit.clamped.begin();
            Id draw = 0, setter = 0;
            for (const auto &[id, e] : frame.entries())
                if (e.category == 7 && e.type == 0x37)
                    draw = id;
            for (const auto &[id, r] : audit.records)
                if (r.setter && r.value == 1)
                    setter = id;
            QVERIFY(draw && setter);
            const auto sub = textureSubresources(frame.resource(resource)).front();
            TexturePatch patch;
            patch.bytes.resize(size_t(sub.size));
            for (size_t p = 0; p < patch.bytes.size(); p += 4) {
                patch.bytes[p] = 255;
                patch.bytes[p + 3] = 255;
            }
            ReplayOptions options;
            options.warp = warp;
            options.until = draw;
            options.before = true;
            options.textureInputs[draw][resource] = {patch};
            if (disableSetter)
                options.disabled.insert(setter);
            Replay before(frame, options);
            before.run();
            before.inspectEventInputs(draw, [&] {
                const auto clone = nativeResource(before, resource);
                before.inspectNativeState([&](auto *context, const auto &) {
                    QCOMPARE(context->GetResourceMinLOD(clone.Get()), disableSetter ? 0.f : 1.f);
                });
            });
            options.until = 0;
            options.before = false;
            Replay replay(frame, options);
            replay.run();
            std::vector<uint8_t> expected(8 * 8 * 4, 0);
            for (size_t p = 0; p < expected.size(); p += 4) {
                expected[p + (disableSetter ? 0 : 1)] = 255;
                expected[p + 3] = 255;
            }
            QCOMPARE(replay.output().rgba, expected);
            const auto original = nativeResource(replay, resource);
            replay.inspectNativeState([&](auto *context, const auto &) {
                QCOMPARE(context->GetResourceMinLOD(original.Get()), 0.f);
            });
            Frame missing((root + "/6/capture.gpa_frame").toStdWString());
            const auto missingAudit = auditResourceLod(missing);
            QCOMPARE(missingAudit.clamped.size(), size_t(1));
            for (const auto &[id, e] : missing.entries())
                if (e.category == 7 && e.type == 0x37)
                    draw = id;
            ReplayOptions unknown;
            unknown.warp = warp;
            unknown.textureInputs[draw][*missingAudit.clamped.begin()] = {patch};
            Replay rejected(missing, unknown);
            try {
                rejected.run();
                QFAIL("Missing initial LOD accepted by an input clone");
            } catch (const std::runtime_error &e) {
                QVERIFY(QString::fromUtf8(e.what()).contains("initial minimum LOD is unresolved"));
            }
        } catch (const std::exception &e) {
            QFAIL(e.what());
        }
    }
};
QTEST_GUILESS_MAIN(LodCloneTests)
#include "LodCloneTests.moc"
