#include "NativeDebugFixture.h"
#include "app/NativeAnalysisController.h"
#include <QDir>
#include <QSignalSpy>
#include <QtTest>

using namespace flora;
using Json = nlohmann::json;
class NativeDebugSessionTests final : public QObject {
    Q_OBJECT
  private slots:
    void vertexIndexProvenance_data() {
        QTest::addColumn<QString>("mutation");
        for (const auto *name :
             {"valid", "observed", "source", "status", "value", "request", "missing", "header",
              "header-missing", "header-negative", "header-overflow", "header-boolean"})
            QTest::newRow(name) << QString(name);
    }
    void vertexIndexProvenance() {
        QFETCH(QString, mutation);
        test::NativeDebugFixture f(true);
        f.job["options"]["index"] = 4;
        f.result["context"]["options"] = f.job["options"];
        f.result["data"]["version"] = 2;
        auto &invocation = f.result["data"]["invocation"];
        invocation["index_override"] = 4;
        invocation["vertex_index"] = 4;
        invocation["observed_vertex_index"] = 0;
        invocation["vertex_id"] = 0;
        invocation["vertex_index_source"] = "explicit_override";
        invocation["status"] = "explicit_vertex_index_override";
        if (mutation == "observed")
            invocation["observed_vertex_index"] = 1;
        if (mutation == "source")
            invocation["vertex_index_source"] = "draw";
        if (mutation == "status")
            invocation["status"] = "eligible_in_observed_replay";
        if (mutation == "value")
            invocation["index_override"] = 3;
        if (mutation == "missing")
            invocation.erase("index_override");
        if (mutation == "request") {
            f.job["options"].erase("index");
            f.result["context"]["options"] = f.job["options"];
        }
        QByteArray raw;
        for (const auto &line : f.artifacts["trace.jsonl"].split('\n')) {
            if (line.isEmpty())
                continue;
            auto row = Json::parse(line.toStdString());
            if (row.at("record") == "header") {
                row["index_override"] = 4;
                if (mutation == "header")
                    row["index_override"] = 3;
                if (mutation == "header-missing")
                    row.erase("index_override");
                if (mutation == "header-negative")
                    row["index_override"] = -1;
                if (mutation == "header-overflow")
                    row["index_override"] = uint64_t(UINT32_MAX) + 1;
                if (mutation == "header-boolean")
                    row["index_override"] = true;
            }
            raw += QByteArray::fromStdString(row.dump()) + '\n';
        }
        f.artifacts["trace.jsonl"] = raw;
        f.result["data"]["trace"]["bytes"] = raw.size();
        f.seal();
        if (mutation == "valid") {
            NativeDebugSession session(f.job, f.result, f.artifacts);
            QCOMPARE(session.job().at("options").at("index"), Json(4));
        } else
            QVERIFY_THROWS_EXCEPTION(std::exception, NativeDebugSession(f.job, f.result, f.artifacts));
    }
    void vertexTraceBoundaries_data() {
        QTest::addColumn<QString>("mutation");
        for (const auto *name : {"group", "lane", "scope", "model", "discard", "shared", "release", "sync"})
            QTest::newRow(name) << QString(name);
    }
    void vertexTraceBoundaries() {
        QFETCH(QString, mutation);
        test::NativeDebugFixture f(true);
        QByteArray raw;
        bool changed = false;
        for (const auto &line : f.artifacts["trace.jsonl"].split('\n')) {
            if (line.isEmpty())
                continue;
            auto row = Json::parse(line.toStdString());
            const auto kind = row.at("record");
            if (!changed && kind == "header") {
                if (mutation == "group") {
                    row["group_size"] = {1, 1, 1};
                    changed = true;
                }
                if (mutation == "lane") {
                    row["selected_lane"] = 1;
                    changed = true;
                }
                if (mutation == "scope") {
                    row["scope"] = "one_cooperative_group";
                    changed = true;
                }
                if (mutation == "model") {
                    row["execution_model"] = "other";
                    changed = true;
                }
            }
            if (!changed && kind == "storage" && mutation == "shared") {
                row["space"] = "shared";
                changed = true;
            }
            if (!changed && kind == "step") {
                if (mutation == "discard") {
                    row["discarded"] = true;
                    changed = true;
                }
                if (mutation == "release") {
                    row["released_barrier"] = true;
                    changed = true;
                }
                if (mutation == "sync") {
                    row["sync_flags"] = 1;
                    changed = true;
                }
            }
            raw += QByteArray::fromStdString(row.dump()) + '\n';
        }
        QVERIFY(changed);
        f.artifacts["trace.jsonl"] = raw;
        f.result["data"]["trace"]["bytes"] = raw.size();
        f.seal();
        QVERIFY_THROWS_EXCEPTION(std::exception, NativeDebugSession(f.job, f.result, f.artifacts));
    }
    void vertexSession() {
        test::NativeDebugFixture f(true);
        NativeDebugSession session(f.job, f.result, f.artifacts);
        auto &model = session.model();
        QCOMPARE(model.result().at("action"), Json("debug-vertex"));
        QVERIFY(!model.result().at("execution_context").contains("group_size"));
        const auto initial = model.registerValues();
        bool sourceSeen = false;
        model.addWatch("total");
        for (size_t i = 0; i < model.size(); ++i) {
            model.move(i);
            for (const auto &value : model.sourceValues())
                sourceSeen |= value.at("name") == "total" && value.at("status") == "available";
        }
        QVERIFY(sourceSeen);
        model.move(model.size());
        QCOMPARE(model.instructionState().at("state_boundary"), Json("terminal_vertex"));
        bool outputSeen = false;
        for (const auto &r : model.registerValues())
            if (r.at("name") == "o0") {
                QCOMPARE(r.at("bits"), Json({13, 0, 0, 1}));
                QCOMPARE(r.at("valid_components"), Json(15));
                outputSeen = true;
            }
        QVERIFY(outputSeen);
        model.move(0);
        QCOMPARE(model.registerValues(), initial);
        const auto config = model.configuration();
        model.importConfiguration(config);
        QCOMPARE(model.configuration(), config);
        for (const auto *key :
             {"input_reference", "instance", "vertex_index", "gpa_shader", "instance_count"}) {
            test::NativeDebugFixture invalid(true);
            if (std::string(key) == "vertex_index") {
                invalid.job["options"]["index"] = 9;
                invalid.result["context"]["options"] = invalid.job["options"];
            } else if (std::string(key) == "instance_count")
                invalid.result["data"]["invocation"]["draw_parameters"][key] = -1;
            else
                invalid.result["data"]["invocation"][key] = 9;
            invalid.seal();
            QVERIFY_THROWS_EXCEPTION(std::exception,
                                     NativeDebugSession(invalid.job, invalid.result, invalid.artifacts));
        }
    }
    void ownershipAndLock() {
        test::NativeDebugFixture fixture;
        auto session = std::make_shared<NativeDebugSession>(fixture.job, fixture.result, fixture.artifacts);
        const auto path = session->tracePath(), directory = QFileInfo(path).absolutePath();
        fixture.artifacts.clear();
        QVERIFY(QFileInfo::exists(path));
        QFile overwrite(path);
        QVERIFY(!overwrite.open(QIODevice::WriteOnly | QIODevice::Truncate));
        const auto initial = session->model().registerValues();
        session->model().move(session->model().size());
        session->model().move(0);
        QCOMPARE(session->model().registerValues(), initial);
        auto retained = session;
        session.reset();
        QVERIFY(QFileInfo::exists(path));
        retained.reset();
        QVERIFY(!QFileInfo::exists(directory));
    }
    void rejectedIdentities_data() {
        QTest::addColumn<QString>("mutation");
        for (const char *name :
             {"request", "context", "action",     "manifest",    "payload",   "metadata",
              "path",    "shader",  "schedule",   "eligibility", "predicate", "event",
              "group",   "thread",  "dimensions", "shader_id",   "frame",     "model",
              "adapter", "vendor",  "warp",       "size",        "records",   "completion"})
            QTest::newRow(name) << QString(name);
    }
    void rejectedIdentities() {
        QFETCH(QString, mutation);
        test::NativeDebugFixture f;
        auto &data = f.result["data"];
        if (mutation == "request")
            f.result["request_id"] = "other";
        if (mutation == "context")
            f.result["context"]["experiment_sha256"] = std::string(64, 'c');
        if (mutation == "action")
            f.job["action"] = f.result["context"]["action"] = "inventory";
        if (mutation == "path")
            data["trace_file"] = "../trace.jsonl";
        if (mutation == "shader")
            data["shader_sha256"] = std::string(64, 'c');
        if (mutation == "schedule")
            data["captured_gpu_schedule"] = true;
        if (mutation == "eligibility")
            data["invocation"]["status"] = "unknown";
        if (mutation == "predicate")
            data["invocation"]["predicate"]["allows_execution"] = false;
        if (mutation == "event")
            data["invocation"]["gpa_event"] = 20;
        if (mutation == "group")
            data["invocation"]["group"] = {1, 0, 0};
        if (mutation == "thread")
            data["invocation"]["thread"] = {1, 0, 0};
        if (mutation == "dimensions")
            data["invocation"]["dispatch_groups"] = {0, 1, 1};
        if (mutation == "shader_id")
            data["gpa_shader"] = 8;
        if (mutation == "frame")
            data["effective_frame_sha256"] = std::string(64, 'c');
        if (mutation == "model")
            data["execution_model"] = "other";
        if (mutation == "adapter")
            f.result["context"]["adapter"]["luid_low"] = 18;
        if (mutation == "vendor")
            f.job["options"]["gpu_vendor"] = f.result["context"]["options"]["gpu_vendor"] = 0x8086;
        if (mutation == "warp") {
            f.job["options"].erase("gpu_vendor");
            f.job["options"]["warp"] = true;
            f.result["context"]["options"] = f.job["options"];
        }
        if (mutation == "size")
            data["trace"]["bytes"] = 1;
        if (mutation == "records")
            data["trace"]["records"] = 1;
        if (mutation == "completion")
            data["trace"]["selected_steps"] = 1;
        f.seal();
        if (mutation == "manifest")
            f.result["artifacts"]["extra"] = Json::object();
        if (mutation == "payload")
            f.artifacts["trace.jsonl"][0] = ' ';
        if (mutation == "metadata")
            f.artifacts["debug.json"] = "{}";
        QVERIFY_THROWS_EXCEPTION(std::exception, NativeDebugSession(f.job, f.result, f.artifacts));
    }
    void budgetAndCancellation() {
        test::NativeDebugFixture f;
        QVERIFY_THROWS_EXCEPTION(std::exception, NativeDebugSession(f.job, f.result, f.artifacts, 1));
        int calls = 0;
        const auto before = QDir(QDir::tempPath()).entryList({"FloraGPA-debug-session-*"}, QDir::Dirs);
        QVERIFY_THROWS_EXCEPTION(OperationCancelled,
                                 NativeDebugSession(f.job, f.result, f.artifacts, 256 * 1024 * 1024,
                                                    [&] { return ++calls == 8; }));
        QCOMPARE(QDir(QDir::tempPath()).entryList({"FloraGPA-debug-session-*"}, QDir::Dirs), before);
    }
    void asynchronousTransferAndRetry() {
        test::NativeDebugFixture f;
        NativeAnalysisController controller;
        NativeAnalysisOutcome result;
        int completions = 0;
        connect(&controller, &NativeAnalysisController::finished, this, [&](const NativeAnalysisOutcome &v) {
            result = v;
            ++completions;
        });
        controller.loadDebugSession(f.job, f.result, f.artifacts);
        QVERIFY(controller.active());
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                 controller.loadDebugSession(f.job, f.result, f.artifacts));
        QTRY_COMPARE_WITH_TIMEOUT(completions, 1, 10000);
        QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
        QVERIFY(result.debugSession);
        QVERIFY(!controller.active());
        const auto path = result.debugSession->tracePath();
        result.debugSession->model().move(result.debugSession->model().size());
        QCOMPARE(result.debugSession->model().instructionState().at("state_boundary"),
                 Json("terminal_group"));
        result = {};
        QVERIFY(!QFileInfo::exists(path));
        controller.loadDebugSession(f.job, f.result, f.artifacts);
        controller.cancel();
        QTRY_COMPARE_WITH_TIMEOUT(completions, 2, 10000);
        QVERIFY(result.cancelled && !result.debugSession && result.artifacts.isEmpty());
        controller.loadDebugSession(f.job, f.result, f.artifacts, 0.0000001);
        QTRY_COMPARE_WITH_TIMEOUT(completions, 3, 10000);
        QVERIFY(result.error.contains("timed out") && !result.debugSession);
        controller.loadDebugSession(f.job, f.result, f.artifacts);
        QTRY_COMPARE_WITH_TIMEOUT(completions, 4, 10000);
        QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
        QVERIFY(result.debugSession);
    }
};
QTEST_GUILESS_MAIN(NativeDebugSessionTests)
#include "NativeDebugSessionTests.moc"
