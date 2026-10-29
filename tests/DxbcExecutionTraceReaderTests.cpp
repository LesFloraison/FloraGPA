#include "application/DxbcExecutionTrace.h"
#include "application/DxbcExecutionTraceReader.h"
#include "application/HlslCompilation.h"
#include <QCryptographicHash>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>
#include <windows.h>
using namespace flora;
using namespace flora::dxbc_exec;
using Json = nlohmann::json;
namespace {
Json reg(uint32_t index) { return {{"dxbc_operand_kind", 0}, {"indices", {index}}}; }
Json value(std::array<uint32_t, 4> bits = {}, uint32_t valid = 0) {
    return {{"bits", bits}, {"valid_components", valid}};
}
std::vector<Json> fixture() {
    // This fixture is authored independently of the journal writer and executor.
    // Two lanes share one buffer; lane 1 is selected. Lane 0 changes storage
    // before lane 1 reads it, and both lanes change the same hidden counter.
    std::vector<Json> rows{
        {{"record", "header"},
         {"format", "flora.dxbc-execution-trace"},
         {"version", 1},
         {"stage", "cs"},
         {"scope", "one_cooperative_group"},
         {"captured_gpu_schedule", false},
         {"group_size", {2, 1, 1}},
         {"thread", {1, 0, 0}},
         {"group", {7, 0, 0}},
         {"selected_lane", 1},
         {"max_steps", 10},
         {"max_bytes", 1000000},
         {"constant_buffer_lengths", {{{"slot", 0}, {"elements", 1}}, {{"slot", 1}, {"elements", 0}}}}},
        {{"record", "source"}, {"data", {{"files", Json::array()}}}},
        {{"record", "instruction"},
         {"index", 0},
         {"word_offset", 2},
         {"dxbc_opcode", 54},
         {"declaration", false},
         {"words", {0x01000036}},
         {"source", nullptr}},
        {{"record", "instruction"},
         {"index", 1},
         {"word_offset", 3},
         {"dxbc_opcode", 62},
         {"declaration", false},
         {"words", {0x0100003e}},
         {"source", {{"line", 17}}}},
        {{"record", "constant"},
         {"slot", 0},
         {"element", 0},
         {"value", value({0x80000000, 0x7fc0abcd, 0xffffffff, 7}, 5)}},
        {{"record", "register"},
         {"lane", 0},
         {"register", reg(7)},
         {"value", value({0x80000000, 0x7fc0abcd, 0xffffffff, 7}, 10)}},
        {{"record", "counter"}, {"view", 99}, {"value", {{"kind", "append"}, {"value", nullptr}}}},
        {{"record", "storage"}, {"space", "buffer"}, {"resource", 11}, {"bytes", 2}},
        {{"record", "storage_bytes"},
         {"space", "buffer"},
         {"resource", 11},
         {"offset", 0},
         {"bits_hex", "80ff"},
         {"valid_hex", "0100"}},
        {{"record", "storage"}, {"space", "shared"}, {"resource", 11}, {"bytes", 0}},
        {{"record", "storage"}, {"space", "texture"}, {"resource", 11}, {"bytes", 2}},
        {{"record", "storage_bytes"},
         {"space", "texture"},
         {"resource", 11},
         {"offset", 0},
         {"bits_hex", "1234"},
         {"valid_hex", "0001"}},
        {{"record", "unavailable_binding"}, {"register", reg(9)}, {"reason", "Not observed"}}};
    for (uint32_t i = 0; i < 4; ++i) {
        Json step{{"record", "step"},
                  {"sequence", i},
                  {"lane", i % 2},
                  {"lane_sequence", i / 2},
                  {"barrier_epoch", 0},
                  {"released_barrier", false},
                  {"instruction", i / 2},
                  {"word_offset", 2 + i / 2},
                  {"next_instruction", i < 2 ? Json(1) : Json(nullptr)},
                  {"return_instruction_stack", Json::array()},
                  {"register_changes", Json::array()},
                  {"buffer_changes", Json::array()},
                  {"shared_changes", Json::array()},
                  {"texture_changes", Json::array()},
                  {"counter_changes", Json::array()},
                  {"discarded", false},
                  {"memory_gpu_assisted", false},
                  {"instruction_gpu_assisted", false},
                  {"sync_flags", 0},
                  {"sync_loop_iterations", Json::array()},
                  {"atomic", false}};
        if (i == 0) {
            step["buffer_changes"] = {{{"resource", 11},
                                       {"offset", 0},
                                       {"before_hex", "80ff"},
                                       {"after_hex", "abcd"},
                                       {"before_valid_hex", "0100"},
                                       {"after_valid_hex", "0001"}}};
            step["counter_changes"] = {{{"view", 99},
                                        {"before", {{"kind", "append"}, {"value", nullptr}}},
                                        {"after", {{"kind", "append"}, {"value", 0xffffffffu}}}}};
        } else if (i == 1) {
            step["register_changes"] = {
                {{"register", reg(0)}, {"before", value()}, {"after", value({0xab, 0xcd, 0, 0}, 2)}},
                {{"register", reg(0)},
                 {"before", value({0xab, 0xcd, 0, 0}, 2)},
                 {"after", value({0x80000000, 0x7fc0abcd, 0xffffffff, 7}, 10)}}};
            step["counter_changes"] = {{{"view", 99},
                                        {"before", {{"kind", "append"}, {"value", 0xffffffffu}}},
                                        {"after", {{"kind", "append"}, {"value", 0}}}}};
        }
        rows.push_back(step);
    }
    rows.push_back({{"record", "complete"}, {"steps", 4}, {"selected_steps", 2}, {"completed", true}});
    return rows;
}
std::string save(const QString &path, const std::vector<Json> &rows) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        throw std::runtime_error("Cannot write test journal");
    QCryptographicHash hash(QCryptographicHash::Sha256);
    for (const auto &row : rows) {
        const auto line = QByteArray::fromStdString(row.dump() + "\n");
        if (file.write(line) != line.size())
            throw std::runtime_error("Cannot write test journal record");
        hash.addData(line);
    }
    return hash.result().toHex().toStdString();
}
std::string digest(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        throw std::runtime_error("Cannot hash test journal");
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file))
        throw std::runtime_error("Cannot hash test journal bytes");
    return hash.result().toHex().toStdString();
}
} // namespace
class DxbcExecutionTraceReaderTests : public QObject {
    Q_OBJECT
  private slots:
    void opaqueStatus() {
        QTemporaryDir dir;
        const auto path = dir.filePath("status.jsonl");
        for (unsigned mapping = 1; mapping <= 3; ++mapping) {
            auto rows = fixture();
            rows[0]["version"] = 2;
            for (auto &row : rows) {
                if (row.at("record") == "constant" || row.at("record") == "register")
                    row["value"]["mapping_status"] = {0, 0, 0, 0};
                if (row.at("record") == "step")
                    for (auto &change : row["register_changes"])
                        for (const auto *side : {"before", "after"})
                            change[side]["mapping_status"] = {0, 0, 0, 0};
            }
            auto &initial = rows.at(5).at("value");
            initial = value();
            initial["mapping_status"] = {mapping, 0, 0, 0};
            {
                ExecutionTraceReader trace(path, save(path, rows));
                QCOMPARE(trace.registerValue(0, {0, {7}}).mapping[0], MappingStatus(mapping));
                trace.moveTo(trace.size());
                trace.moveTo(0);
                QCOMPARE(trace.registerValue(0, {0, {7}}).mapping[0], MappingStatus(mapping));
            }
            initial["valid_components"] = 1;
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, ExecutionTraceReader(path, save(path, rows)));
            initial["valid_components"] = 0;
            initial["bits"][0] = 1;
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, ExecutionTraceReader(path, save(path, rows)));
            initial["bits"][0] = 0;
            initial["mapping_status"][0] = 4;
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, ExecutionTraceReader(path, save(path, rows)));
        }
    }
    void reversibleGroupState() {
        QTemporaryDir dir;
        const auto path = dir.filePath("trace.jsonl");
        ExecutionTraceReader trace(path, save(path, fixture()));
        QCOMPARE(trace.size(), uint64_t(4));
        QCOMPARE(trace.position(), uint64_t(0));
        QCOMPARE(trace.selectedSize(), uint64_t(2));
        QCOMPARE(trace.selectedBoundary(0), uint64_t(1));
        QCOMPARE(trace.selectedBoundary(1), uint64_t(3));
        QCOMPARE(trace.selectedBoundary(2), uint64_t(4));
        QVERIFY(trace.instruction(0).at("source").is_null());
        QCOMPARE(trace.instruction(1).at("source").at("line"), Json(17));
        QCOMPARE(trace.source().at("files"), Json::array());
        QCOMPARE(trace.observationCount(), size_t(1));
        QCOMPARE(trace.observation(0).at("reason"), Json("Not observed"));
        const auto initialRegisters = trace.registers();
        const auto initialStorage = trace.storage();
        const auto initialCounters = trace.counters();
        for (int repeat = 0; repeat < 3; ++repeat) {
            trace.moveTo(trace.selectedBoundary(0));
            QCOMPARE(trace.storage().at({TraceSpace::Buffer, 11}).bytes, std::vector<uint8_t>({0xab, 0xcd}));
            QCOMPARE(trace.storage().at({TraceSpace::Buffer, 11}).valid, std::vector<uint8_t>({0, 1}));
            QCOMPARE(trace.registerValue(1, {0, {0}}), Value{});
            QCOMPARE(trace.counters().at(99).value, std::optional<uint32_t>(UINT32_MAX));
            trace.moveTo(2);
            QCOMPARE(trace.registerValue(1, {0, {0}}), (Value{{0x80000000, 0x7fc0abcd, 0xffffffff, 7}, 10}));
            QCOMPARE(trace.counters().at(99).value, std::optional<uint32_t>(0));
            trace.moveTo(trace.size());
            trace.moveTo(0);
            QCOMPARE(trace.registers(), initialRegisters);
            QCOMPARE(trace.counters(), initialCounters);
            for (const auto &[key, buffer] : initialStorage) {
                QCOMPARE(trace.storage().at(key).bytes, buffer.bytes);
                QCOMPARE(trace.storage().at(key).valid, buffer.valid);
            }
        }
        QCOMPARE(trace.registerValue(0, {0, {999}}), Value{});
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, trace.registerValue(2, {0, {0}}));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, trace.moveTo(5));
        QVERIFY_THROWS_EXCEPTION(std::out_of_range, trace.selectedBoundary(3));
        QVERIFY_THROWS_EXCEPTION(std::out_of_range, trace.instruction(2));
        QCOMPARE(trace.position(), uint64_t(0));
    }
    void constantKnowledge() {
        QTemporaryDir dir;
        auto rows = fixture();
        const auto path = dir.filePath("new.jsonl"), oldPath = dir.filePath("old.jsonl");
        ExecutionTraceReader trace(path, save(path, rows));
        QCOMPARE(trace.constant(0, 0), (Value{{0x80000000, 0x7fc0abcd, 0xffffffff, 7}, 5}));
        QCOMPARE(trace.constant(0, 1), Value::known({}));
        QCOMPARE(trace.constant(1, 0), Value::known({}));
        QCOMPARE(trace.constant(2, 0), Value{});
        rows[0].erase("constant_buffer_lengths");
        ExecutionTraceReader old(oldPath, save(oldPath, rows));
        QCOMPARE(old.constant(0, 0), trace.constant(0, 0));
        QCOMPARE(old.constant(0, 1), Value{});
        QCOMPARE(old.constant(1, 0), Value{});
    }
    void cancellationAndLock() {
        QTemporaryDir dir;
        const auto path = dir.filePath("trace.jsonl");
        const auto hash = save(path, fixture());
        QVERIFY_THROWS_EXCEPTION(OperationCancelled,
                                 ExecutionTraceReader(path, hash, {}, [] { return true; }));
        {
            ExecutionTraceReader trace(path, hash);
            int checks = 0;
            QVERIFY_THROWS_EXCEPTION(OperationCancelled, trace.moveTo(4, [&] { return ++checks == 3; }));
            QCOMPARE(trace.position(), uint64_t(2));
            checks = 0;
            QVERIFY_THROWS_EXCEPTION(OperationCancelled, trace.moveTo(0, [&] { return ++checks == 2; }));
            QCOMPARE(trace.position(), uint64_t(1));
            trace.moveTo(4);
            trace.moveTo(0);
            HANDLE writer = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), GENERIC_WRITE,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            const auto error = GetLastError();
            if (writer != INVALID_HANDLE_VALUE)
                CloseHandle(writer);
            QCOMPARE(writer, INVALID_HANDLE_VALUE);
            QCOMPARE(error, DWORD(ERROR_SHARING_VIOLATION));
            QVERIFY(!DeleteFileW(reinterpret_cast<LPCWSTR>(path.utf16())));
            QCOMPARE(GetLastError(), DWORD(ERROR_SHARING_VIOLATION));
        }
        QFile writer(path);
        QVERIFY(writer.open(QIODevice::WriteOnly));
    }
    void corruptions_data() {
        QTest::addColumn<QString>("mutation");
        for (const char *name : {"digest",
                                 "format",
                                 "version",
                                 "stage",
                                 "schedule",
                                 "selected",
                                 "dimension",
                                 "lane",
                                 "global-sequence",
                                 "lane-sequence",
                                 "instruction",
                                 "word-offset",
                                 "next-instruction",
                                 "terminal",
                                 "barrier",
                                 "register-chain",
                                 "valid-mask",
                                 "negative-bits",
                                 "overflow-bits",
                                 "counter-chain",
                                 "storage-chain",
                                 "storage-validity",
                                 "storage-gap",
                                 "storage-overlap",
                                 "storage-missing",
                                 "storage-duplicate",
                                 "storage-extent",
                                 "storage-space",
                                 "constant-missing",
                                 "constant-duplicate",
                                 "constant-length",
                                 "register-duplicate",
                                 "instruction-offset",
                                 "instruction-opcode",
                                 "source-duplicate",
                                 "source-missing",
                                 "completion-missing",
                                 "completion-count",
                                 "selected-count",
                                 "trailing",
                                 "initial-after-step",
                                 "unknown-record",
                                 "call-stack",
                                 "loop-stack",
                                 "float-integer",
                                 "bool-type",
                                 "declared-step-budget",
                                 "declared-byte-budget",
                                 "empty",
                                 "truncated",
                                 "duplicate-key"})
            QTest::newRow(name) << QString::fromLatin1(name);
    }
    void corruptions() {
        QFETCH(QString, mutation);
        auto rows = fixture();
        auto &step = rows[13];
        if (mutation == "format")
            rows[0]["format"] = "external";
        else if (mutation == "version")
            rows[0]["version"] = 2;
        else if (mutation == "stage")
            rows[0]["stage"] = "ps";
        else if (mutation == "schedule")
            rows[0]["captured_gpu_schedule"] = true;
        else if (mutation == "selected")
            rows[0]["selected_lane"] = 0;
        else if (mutation == "dimension")
            rows[0]["group_size"][0] = 0;
        else if (mutation == "lane")
            step["lane"] = 2;
        else if (mutation == "global-sequence")
            step["sequence"] = 1;
        else if (mutation == "lane-sequence")
            step["lane_sequence"] = 1;
        else if (mutation == "instruction")
            step["instruction"] = 1;
        else if (mutation == "word-offset")
            step["word_offset"] = 3;
        else if (mutation == "next-instruction")
            step["next_instruction"] = 0;
        else if (mutation == "terminal")
            rows[15]["next_instruction"] = 0;
        else if (mutation == "barrier")
            step["barrier_epoch"] = 1;
        else if (mutation == "register-chain")
            rows[14]["register_changes"][0]["before"]["bits"][0] = 1;
        else if (mutation == "valid-mask")
            rows[5]["value"]["valid_components"] = 16;
        else if (mutation == "negative-bits")
            rows[5]["value"]["bits"][0] = -1;
        else if (mutation == "overflow-bits")
            rows[5]["value"]["bits"][0] = uint64_t(UINT32_MAX) + 1;
        else if (mutation == "counter-chain")
            step["counter_changes"][0]["before"]["value"] = 1;
        else if (mutation == "storage-chain")
            step["buffer_changes"][0]["before_hex"] = "00ff";
        else if (mutation == "storage-validity")
            rows[8]["valid_hex"] = "0102";
        else if (mutation == "storage-gap")
            rows[8]["offset"] = 1;
        else if (mutation == "storage-overlap")
            rows.insert(rows.begin() + 9, rows[8]);
        else if (mutation == "storage-missing")
            rows.erase(rows.begin() + 8);
        else if (mutation == "storage-duplicate")
            rows.insert(rows.begin() + 8, rows[7]);
        else if (mutation == "storage-extent")
            rows[8]["bits_hex"] = "001122";
        else if (mutation == "storage-space")
            rows[7]["space"] = "unknown";
        else if (mutation == "constant-missing")
            rows.erase(rows.begin() + 4);
        else if (mutation == "constant-duplicate")
            rows.insert(rows.begin() + 5, rows[4]);
        else if (mutation == "constant-length")
            rows[0]["constant_buffer_lengths"][0]["elements"] = 0;
        else if (mutation == "register-duplicate")
            rows.insert(rows.begin() + 6, rows[5]);
        else if (mutation == "instruction-offset")
            rows[3]["word_offset"] = 4;
        else if (mutation == "instruction-opcode")
            rows[2]["dxbc_opcode"] = 55;
        else if (mutation == "source-duplicate")
            rows.insert(rows.begin() + 2, rows[1]);
        else if (mutation == "source-missing")
            rows.erase(rows.begin() + 1);
        else if (mutation == "completion-missing")
            rows.pop_back();
        else if (mutation == "completion-count")
            rows.back()["steps"] = 3;
        else if (mutation == "selected-count")
            rows.back()["selected_steps"] = 1;
        else if (mutation == "trailing")
            rows.push_back(rows.back());
        else if (mutation == "initial-after-step")
            rows.insert(rows.begin() + 14, rows[12]);
        else if (mutation == "unknown-record")
            rows[12]["record"] = "unknown";
        else if (mutation == "call-stack")
            step["return_instruction_stack"] = {99};
        else if (mutation == "loop-stack")
            step["sync_loop_iterations"] = {{99, 0}};
        else if (mutation == "float-integer")
            step["sequence"] = 0.0;
        else if (mutation == "bool-type")
            step["discarded"] = 0;
        else if (mutation == "declared-step-budget")
            rows[0]["max_steps"] = 3;
        else if (mutation == "declared-byte-budget")
            rows[0]["max_bytes"] = 10;
        else if (mutation == "empty")
            rows.clear();
        QTemporaryDir dir;
        const auto path = dir.filePath("trace.jsonl");
        auto hash = save(path, rows);
        if (mutation == "digest")
            hash[0] = hash[0] == '0' ? '1' : '0';
        if (mutation == "truncated") {
            QFile file(path);
            QVERIFY(file.open(QIODevice::ReadWrite));
            QVERIFY(file.resize(file.size() - 1));
            file.close();
            hash = digest(path);
        }
        if (mutation == "duplicate-key") {
            QFile file(path);
            QVERIFY(file.open(QIODevice::ReadWrite));
            auto bytes = file.readAll();
            bytes.replace("\"sequence\":0", "\"sequence\":0,\"sequence\":0");
            QVERIFY(file.resize(0));
            QVERIFY(file.seek(0));
            QCOMPARE(file.write(bytes), bytes.size());
            file.close();
            hash = digest(path);
        }
        QVERIFY_THROWS_EXCEPTION(std::exception, ExecutionTraceReader(path, hash));
    }
    void budgets_data() {
        QTest::addColumn<int>("budget");
        for (int i = 0; i < 7; ++i)
            QTest::newRow(qPrintable(QString::number(i))) << i;
    }
    void budgets() {
        QFETCH(int, budget);
        QTemporaryDir dir;
        const auto path = dir.filePath("trace.jsonl");
        const auto hash = save(path, fixture());
        TraceReadLimits limits;
        switch (budget) {
        case 0:
            limits.maxBytes = 1;
            break;
        case 1:
            limits.maxRecordBytes = 10;
            break;
        case 2:
            limits.maxStorageBytes = 3;
            break;
        case 3:
            limits.maxSteps = 3;
            break;
        case 4:
            limits.maxRecords = 17;
            break;
        case 5:
            limits.maxRegisters = 1;
            break;
        case 6:
            limits.maxBytes = UINT64_MAX;
            break;
        }
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, ExecutionTraceReader(path, hash, limits));
    }
    void writerIntegration() {
        InputSnapshot input;
        input.stage = 5;
        input.event = 19;
        input.shader = 7;
        input.bytecode = compileHlsl(R"(
            RWByteAddressBuffer output:register(u0);
            groupshared uint g[4];
            [numthreads(2,2,1)]void main(uint i:SV_GroupIndex){
                g[i]=i;GroupMemoryBarrierWithGroupSync();
                output.Store(i*4,g[(i+1)%4]);
            })",
                                     "cs_5_0")
                             .bytecode;
        input.inputs.constantBuffers[1] = {};
        input.inputs.memory = std::make_shared<Memory>(
            std::map<uint64_t, Buffer>{{11, Buffer::known(std::vector<uint8_t>(16, 0x8d))}},
            std::map<Register, BufferView>{{{30, {0}}, {11, 0, 16, 0}}});
        QTemporaryDir dir;
        const auto path = dir.filePath("trace.jsonl");
        GroupTraceOptions options;
        options.thread = {1, 1, 0};
        const auto report = exportGroupExecutionTrace(path, input, options);
        ExecutionTraceReader trace(path, digest(path));
        QCOMPARE(trace.size(), report.at("steps").get<uint64_t>());
        QCOMPARE(trace.selectedSize(), report.at("selected_steps").get<uint64_t>());
        QCOMPARE(trace.constant(1, 0), Value::known({}));
        const auto initial = trace.registers();
        trace.moveTo(trace.size());
        QCOMPARE(trace.storage().at({TraceSpace::Buffer, 11}).bytes,
                 std::vector<uint8_t>({1, 0, 0, 0, 2, 0, 0, 0, 3, 0, 0, 0, 0, 0, 0, 0}));
        trace.moveTo(0);
        QCOMPARE(trace.registers(), initial);
        QCOMPARE(trace.storage().at({TraceSpace::Buffer, 11}).bytes, std::vector<uint8_t>(16, 0x8d));
    }
};
QTEST_GUILESS_MAIN(DxbcExecutionTraceReaderTests)
#include "DxbcExecutionTraceReaderTests.moc"
