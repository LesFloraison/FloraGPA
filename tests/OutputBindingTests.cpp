#include "StateCapture.h"
#include "StreamCapture.h"
#include "application/Experiment.h"
#include "application/OutputEdits.h"
#include "core/OutputBindings.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QtTest>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
using namespace flora;
using namespace flora::testing;
using Json = nlohmann::json;
namespace {
Json json(const BindingValues &values) {
    Json out = Json::object();
    for (auto &[key, value] : values)
        out[key] = value ? Json(*value) : Json(nullptr);
    return out;
}
std::string hash(const BindingValues &values) {
    auto text = json(values).dump();
    return sha256(Bytes(reinterpret_cast<const uint8_t *>(text.data()), text.size()));
}
State stateFromValues(const Json &v) {
    State s;
    auto get = [&](const std::string &key) { return v.at(key).get<Id>(); };
    s.ib = get("ib");
    s.ibFormat = uint32_t(get("ib_format"));
    s.ibOffset = uint32_t(get("ib_offset"));
    for (unsigned i = 0; i < 32; ++i) {
        auto index = std::to_string(i);
        s.vb[i] = get("vb." + index);
        s.strides[i] = uint32_t(get("strides." + index));
        s.offsets[i] = uint32_t(get("offsets." + index));
    }
    constexpr const char *stages[]{"vs", "hs", "ds", "gs", "ps", "cs"};
    for (unsigned st = 0; st < 6; ++st)
        for (unsigned i = 0; i < 128; ++i)
            s.stages[st].srv[i] = get(std::string(stages[st]) + ".srv." + std::to_string(i));
    s.omStart = 64;
    for (unsigned i = 0; i < 64; ++i) {
        auto index = std::to_string(i);
        Id rt = i < 8 ? get("rtv." + index) : 0, om = get("om.uav." + index);
        if (om)
            s.omStart = std::min(s.omStart, i);
        auto merged = rt ? rt : om;
        if (merged)
            s.rtCount = i + 1;
        (i < 8 ? s.rtv[i] : s.omExtended[i - 8]) = merged;
        (i < 8 ? s.csUav[i] : s.csExtended[i - 8]) = get("cs.uav." + index);
    }
    s.dsv = get("dsv");
    for (unsigned i = 0; i < 4; ++i) {
        s.so[i] = get("so.targets." + std::to_string(i));
        if (s.so[i])
            s.soCount = i + 1;
    }
    return s;
}
void oracle(const QString &path) {
    std::ifstream stream(path.toStdWString());
    Json request;
    stream >> request;
    Frame frame(QString::fromStdString(request.at("frame").get<std::string>()).toStdWString());
    OutputBindingModel model(frame, request.value("context", Id(1)));
    std::map<Id, std::vector<uint8_t>> replacements;
    for (auto &edit : request.value("edits", Json::array())) {
        auto bytes =
            QByteArray::fromHex(QByteArray::fromStdString(edit.at("replacement").get<std::string>()));
        replacements[edit.at("event").get<Id>()] = std::vector<uint8_t>(bytes.begin(), bytes.end());
    }
    OutputBindingHistory history(frame, std::move(replacements));
    Json result = Json::array();
    for (const auto &step : request.at("steps")) {
        Json row;
        if (request.value("mode", "model") == "arguments") {
            try {
                const auto event = step.at("event").get<Id>();
                row["captured"] = capturedOutputSetter(frame, event);
                const auto bytes = validateOutputSetter(frame, event, step.at("values"));
                row["encoded"] =
                    QByteArray(reinterpret_cast<const char *>(bytes.data()), qsizetype(bytes.size()))
                        .toHex()
                        .toStdString();
                row["status"] = "ok";
            } catch (const std::exception &e) {
                row["status"] = "error";
                row["error"] = e.what();
            }
            result.push_back(std::move(row));
            continue;
        }
        if (request.value("mode", "model") == "history") {
            try {
                const auto event = step.at("event").get<Id>();
                row["delta"] = json(history.delta(event));
                if (step.contains("state")) {
                    State state = stateFromValues(step.at("state"));
                    state.soOffsets = step.value("so_offsets", std::array<uint32_t, 4>{});
                    const auto changed = history.state(event, state);
                    row["state"] = hash(OutputBindingModel::snapshot(changed.state));
                    row["so_offsets"] = changed.state.soOffsets;
                    std::vector<unsigned> retained;
                    for (unsigned i = 0; i < 4; ++i)
                        if (changed.retainedSo[i])
                            retained.push_back(i);
                    row["so_retained_slots"] = retained;
                }
                row["status"] = "ok";
            } catch (const MissingOutputResource &e) {
                row["status"] = "missing";
                row["missing"] = e.resources;
            } catch (const std::exception &e) {
                row["status"] = "error";
                row["error"] = e.what();
            }
            row["gaps"] = Json::object();
            for (auto &[id, gap] : history.gaps())
                row["gaps"][std::to_string(id)] = {{"event", id},
                                                   {"missing_views", gap.missingViews},
                                                   {"affected_fields", gap.fields.affected},
                                                   {"affected_edited_fields", gap.fields.edited}};
            result.push_back(std::move(row));
            continue;
        }
        try {
            if (step.contains("anchor"))
                model.anchor(stateFromValues(step.at("anchor")));
            else {
                const auto &entry = frame.entry(step.at("event").get<Id>());
                if (step.value("gap", false)) {
                    auto gap = model.gap(entry);
                    row["gap"] = {{"affected_fields", gap.affected}, {"affected_edited_fields", gap.edited}};
                } else {
                    QByteArray bytes;
                    std::optional<Bytes> replacement;
                    if (step.contains("replacement")) {
                        bytes = QByteArray::fromHex(
                            QByteArray::fromStdString(step.at("replacement").get<std::string>()));
                        replacement =
                            Bytes(reinterpret_cast<const uint8_t *>(bytes.data()), size_t(bytes.size()));
                    }
                    model.step(entry, replacement);
                }
            }
            row["status"] = "ok";
        } catch (const MissingOutputResource &e) {
            row["status"] = "missing";
            row["missing"] = e.resources;
        } catch (const std::exception &e) {
            row["status"] = "error";
            row["error"] = e.what();
        }
        row["original"] = hash(model.original());
        row["changed"] = hash(model.changed());
        row["dirty"] = model.dirty();
        try {
            row["overlay"] = hash(OutputBindingModel::snapshot(model.overlay(State{})));
        } catch (const std::exception &) {
            row["overlay"] = nullptr;
        }
        if (step.value("verbose", false)) {
            row["original_values"] = json(model.original());
            row["changed_values"] = json(model.changed());
        }
        result.push_back(std::move(row));
    }
    std::cout << result.dump() << '\n';
}
} // namespace
class OutputBindingTests final : public QObject {
    Q_OBJECT
  private slots:
    void wire() {
        auto keep = statePack(Id(0), Id(1), keepOutput, uint8_t(0), Id(UINT64_MAX), keepOutput, keepOutput,
                              uint8_t(0), uint8_t(0));
        auto c = readOutputCommand(0x3500, keep);
        QVERIFY(!c.rtvs);
        QVERIFY(!c.uavs);
        QVERIFY(!c.initialCounts);
        QVERIFY(outputChanges(0x3500, c).empty());
        for (size_t n = 0; n < keep.size(); ++n)
            QVERIFY_THROWS_EXCEPTION(std::exception, readOutputCommand(0x3500, Bytes(keep).first(n)));
        keep.push_back(0);
        QVERIFY_THROWS_EXCEPTION(std::exception, readOutputCommand(0x3500, keep));
        QVERIFY_THROWS_EXCEPTION(
            std::exception,
            readOutputCommand(0x3522, statePack(Id(0), Id(1), 0u, 1u, uint8_t(0), uint8_t(0))));
        auto empty = readOutputCommand(0x3522, statePack(Id(0), Id(1), 64u, 0u, uint8_t(1), uint8_t(0)));
        QVERIFY(empty.uavs);
        QVERIFY(empty.uavs->empty());
        QVERIFY(!empty.initialCounts);
        QVERIFY(outputChanges(0x3522, empty).empty());
        auto high =
            readOutputCommand(0x25e, statePack(Id(0), Id(1), 63u, 1u, uint8_t(1), Id(20), uint8_t(1), 9u));
        QCOMPARE(outputChanges(0x25e, high).at(135), Id(20));
        QVERIFY_THROWS_EXCEPTION(std::exception, validateOutputRange(0x25e, high, 8));
        high.uavs.reset();
        QVERIFY_THROWS_EXCEPTION(std::exception, outputChanges(0x25e, high));
        high.uavs = std::vector<Id>{20};
        high.initialCounts = std::vector<uint32_t>{};
        QVERIFY_THROWS_EXCEPTION(std::exception, outputChanges(0x25e, high));
    }
    void history() {
        QTemporaryDir dir;
        auto c = stateCapture();
        for (const auto &entry : c.entries)
            if (entry.id == 20)
                put(c.bytes, size_t(entry.offset) + 24, 136u);
        c.save(dir.path() + "/frame.gpa_frame");
        Frame frame((dir.path() + "/frame.gpa_frame").toStdWString());
        OutputBindingModel model(frame, 1);
        auto replacement = statePack(Id(0), Id(1), 63u, 1u, uint8_t(1), Id(0), uint8_t(0));
        QVERIFY_THROWS_EXCEPTION(std::exception, model.step(frame.entry(140), Bytes(replacement)));
        QVERIFY(model.dirty().empty());
        model.step(frame.entry(139));
        model.step(frame.entry(140), Bytes(replacement));
        model.step(frame.entry(141));
        QCOMPARE(model.original().at("ps.srv.0"), std::optional<Id>(0));
        QCOMPARE(model.changed().at("ps.srv.0"), std::optional<Id>(23));
        QVERIFY(model.dirty().contains("cs.uav.63"));
        auto original = stateFromValues(json(model.original()));
        model.anchor(original);
        QCOMPARE(model.overlay(original).stages[4].srv[0], Id(23));
        model.step(frame.entry(142));
        QVERIFY(!model.dirty().contains("cs.uav.63"));
        model.step(frame.entry(158));
        QVERIFY(model.dirty().empty());
    }
    void atomicTargets() {
        QTemporaryDir dir;
        auto c = stateCapture();
        c.save(dir.path() + "/frame.gpa_frame");
        Frame frame((dir.path() + "/frame.gpa_frame").toStdWString());
        OutputBindingModel model(frame, 1);
        model.step(frame.entry(100));
        const auto before = model.changed();
        auto overlap = statePack(Id(0), Id(1), 2u, uint8_t(1), Id(8), Id(8), Id(0));
        QVERIFY_THROWS_EXCEPTION(std::exception, model.step(frame.entry(161), Bytes(overlap)));
        QCOMPARE(model.changed(), before);
        QCOMPARE(model.original(), before);
        QVERIFY(model.dirty().empty());
        auto dimensions = statePack(Id(0), Id(1), 2u, uint8_t(1), Id(7), Id(8), Id(0));
        QVERIFY_THROWS_EXCEPTION(std::exception, model.step(frame.entry(161), Bytes(dimensions)));
        QCOMPARE(model.changed(), before);
        auto missing = statePack(Id(0), Id(1), 1u, uint8_t(1), Id(999999), Id(0));
        QVERIFY_THROWS_EXCEPTION(MissingOutputResource, model.step(frame.entry(161), Bytes(missing)));
        QCOMPARE(model.changed(), before);
    }
    void historyCache() {
        QTemporaryDir dir;
        auto c = stateCapture();
        c.add(170, 7, 0x34ff, statePack(Id(0), Id(1), 1u, uint8_t(1), Id(999999), Id(0)));
        c.add(171, 7, 0x242, statePack(Id(0), Id(1)));
        c.save(dir.path() + "/frame.gpa_frame");
        Frame f((dir.path() + "/frame.gpa_frame").toStdWString());
        // Earlier unrelated malformed captured records would prevent a full
        // traversal: create the history capture from only the relevant entries.
        Capture historyCapture;
        for (const auto &[id, entry] : f.entries())
            if (entry.category != 7 || id == 100 || id == 160 || id == 161 || id == 170 || id == 171) {
                auto raw = f.payload(id);
                historyCapture.add(id, entry.category, entry.type, {raw.begin(), raw.end()});
            }
        historyCapture.save(dir.path() + "/history.gpa_frame");
        Frame frame((dir.path() + "/history.gpa_frame").toStdWString());
        const auto edit = statePack(Id(0), Id(1), 0u, uint8_t(1), Id(0));
        OutputBindingHistory history(frame, {{161, edit}});
        QCOMPARE(history.delta(161).at("rtv.0"), std::optional<Id>(0));
        QVERIFY(history.delta(171).empty());
        QCOMPARE(history.delta(161).at("rtv.0"), std::optional<Id>(0));
        QVERIFY(history.delta(162).empty());
        QVERIFY(history.gaps().contains(170));
        auto result = history.state(161, State{});
        QCOMPARE(result.state.stages[4].srv[0], Id(25));
        QVERIFY_THROWS_EXCEPTION(std::exception, OutputBindingHistory(frame, {{100, edit}}));
    }
    void counterExperimentHistory() {
        try {
            QTemporaryDir dir;
            auto c = computeCapture(true);
            c.add(70, 7, 0x242, statePack(Id(0), Id(1)));
            c.buffer(34, 35, D3D11_BIND_UNORDERED_ACCESS, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED,
                     {1, 2, 3, 4});
            c.uav(36, 34, D3D11_BUFFER_UAV_FLAG_COUNTER);
            c.add(91, 7, 0x3522, statePack(Id(0), Id(1), 1u, 1u, uint8_t(1), Id(12), uint8_t(1), 0u));
            c.save(dir.path() + "/counter.gpa_frame");
            Frame f((dir.path() + "/counter.gpa_frame").toStdWString());
            Experiment e(f);
            Json values{{"start_slot", 1}, {"uav_count", 1}, {"uavs", {36}}, {"initial_counts", {1}}};
            e.setSetter(f, 91, values);
            QVERIFY(e.setUavCounter(f, 36, 2, 100));
            e.setBuffer(f, 100, 34, 0, statePack(99u));
            const auto saved = e.document();
            auto invalid = values;
            invalid["uavs"] = {12};
            QVERIFY_THROWS_EXCEPTION(std::exception, e.setSetter(f, 91, invalid));
            QCOMPARE(e.document(), saved);
            e.save(dir.path() + "/project.json");
            Experiment loaded(f);
            loaded.load(dir.path() + "/project.json", f);
            QCOMPARE(loaded.document(), saved);
            for (bool warp : {false, true}) {
                ReplayOptions options;
                options.warp = warp;
                options.until = 100;
                loaded.apply(f, options);
                Replay replay(f, options);
                for (int i = 0; i < 2; ++i) {
                    replay.run();
                    QCOMPARE(replay.readCounter(36), 3u);
                    QCOMPARE(replay.readBuffer(34), statePack(99u, 2u, 7u, 4u));
                }
                QVERIFY(loaded.undo());
                QVERIFY(loaded.undo());
                QVERIFY(loaded.undo());
                loaded.apply(f, options);
                QVERIFY(options.outputSetters.empty());
                QVERIFY(options.buffers.empty());
                QVERIFY(options.uavCounters.empty());
                QVERIFY(loaded.redo());
                QVERIFY(loaded.redo());
                QVERIFY(loaded.redo());
            }
        } catch (const std::exception &e) {
            QFAIL(e.what());
        }
    }
    void retainedStreamOutputReplay() {
        try {
            QTemporaryDir dir;
            auto c = streamCapture();
            c.add(120, 7, 0x3503, statePack(Id(0), Id(1), 1u, uint8_t(1), Id(70), uint8_t(1), 16u));
            c.save(dir.path() + "/stream.gpa_frame");
            Frame f((dir.path() + "/stream.gpa_frame").toStdWString());
            Experiment e(f);
            e.setSetter(f, 120, {{"count", 1}, {"buffers", {70}}, {"offsets", {128}}});
            for (bool warp : {false, true}) {
                ReplayOptions options;
                options.warp = warp;
                options.until = 150;
                e.apply(f, options);
                Replay replay(f, options);
                replay.run();
                auto first = replay.readBuffer(70);
                QVERIFY(std::all_of(first.begin() + 64, first.begin() + 128,
                                    [](uint8_t v) { return v == 0xcd; }));
                QVERIFY(!std::all_of(first.begin() + 128, first.begin() + 176,
                                     [](uint8_t v) { return v == 0xcd; }));
                replay.run();
                QCOMPARE(replay.readBuffer(70), first);
                QVERIFY(e.undo());
                e.apply(f, options);
                Replay baseline(f, options);
                baseline.run();
                QVERIFY(baseline.readBuffer(70) != first);
                QVERIFY(e.redo());
            }
        } catch (const std::exception &e) {
            QFAIL(e.what());
        }
    }
    void arguments() {
        QTemporaryDir dir;
        auto c = stateCapture();
        c.buffer(290, 291, 16, 0, {0, 0, 0, 0});
        c.add(292, 7, 0x3503, statePack(Id(0), Id(1), 1u, uint8_t(1), Id(290), uint8_t(0)));
        c.save(dir.path() + "/frame.gpa_frame");
        Frame f((dir.path() + "/frame.gpa_frame").toStdWString());
        auto so = capturedOutputSetter(f, 292);
        QVERIFY(so["offsets"].is_null());
        QCOMPARE(readStreamOutputTargets(validateOutputSetter(f, 292, so)).buffers->at(0), Id(290));
        so["offsets"] = {3};
        QVERIFY_THROWS_EXCEPTION(std::exception, validateOutputSetter(f, 292, so));
        so = {{"count", 2}, {"buffers", {290, 290}}, {"offsets", nullptr}};
        QVERIFY_THROWS_EXCEPTION(std::exception, validateOutputSetter(f, 292, so));
        auto output = capturedOutputSetter(f, 161);
        output["rtvs"] = Json::array();
        QVERIFY_THROWS_EXCEPTION(std::exception, validateOutputSetter(f, 161, output));
        output["rtv_count"] = 0;
        auto raw = validateOutputSetter(f, 161, output);
        QCOMPARE(readOutputCommand(0x34ff, raw).rtvCount, 0u);
        output["rtv_count"] = false;
        QVERIFY_THROWS_EXCEPTION(std::exception, validateOutputSetter(f, 161, output));
        output["rtv_count"] = 0;
        output["extra"] = 0;
        QVERIFY_THROWS_EXCEPTION(std::exception, validateOutputSetter(f, 161, output));
    }
};
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (app.arguments().size() == 3 && app.arguments()[1] == "--oracle") {
        try {
            oracle(app.arguments()[2]);
            return 0;
        } catch (const std::exception &e) {
            std::cerr << e.what();
            return 1;
        }
    }
    OutputBindingTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "OutputBindingTests.moc"
