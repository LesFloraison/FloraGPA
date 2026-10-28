#include "ClassCapture.h"
#include "application/DxbcExecutionTrace.h"
#include "application/DxbcExecutionTraceReader.h"
#include "application/DxbcInterfaceBindings.h"
#include "application/NativeExecutionDebugModel.h"
#include "core/Dxbc.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>
using namespace flora;
using namespace flora::dxbc_exec;
using Json = nlohmann::json;
namespace {
std::vector<uint8_t> read(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        throw std::runtime_error("Cannot read interface probe evidence");
    const auto bytes = file.readAll();
    return {bytes.begin(), bytes.end()};
}
std::vector<InterfaceClassInstance> classes(const Json &item) {
    std::vector<InterfaceClassInstance> result;
    for (const auto &binding : item.at("bindings"))
        result.push_back({uint64_t(100 + result.size()),
                          binding.at("descriptor").get<std::array<uint32_t, 8>>(), binding.at("name"),
                          binding.at("type")});
    return result;
}
Inputs inputs(const InterfaceBindings &bindings, uint32_t mode, const Json &report = Json::object()) {
    Inputs result;
    result.interfaceTables = bindings.tables;
    result.registers = bindings.registers;
    result.constantBuffers[0] = {Value::known({mode & 1, 0, 0, 0})};
    std::vector<uint32_t> words;
    if (report.contains("constant_words"))
        words = report.at("constant_words").get<std::vector<uint32_t>>();
    else
        for (uint32_t i = 0; i < 4; ++i)
            words.insert(words.end(), {(i + 1) * 10, 0, 0, 0});
    if (words.size() % 4)
        throw std::runtime_error("Incomplete interface constants");
    for (uint32_t cb = 1; cb < 4; ++cb)
        for (size_t i = 0; i < words.size(); i += 4)
            result.constantBuffers[cb].push_back(
                Value::known({words[i], words[i + 1], words[i + 2], words[i + 3]}));
    result.memory =
        std::make_shared<Memory>(std::map<uint64_t, Buffer>{{1, Buffer::known(std::vector<uint8_t>(16))}},
                                 std::map<Register, BufferView>{{{30, {3}}, {1, 0, 16, 4}}});
    return result;
}
} // namespace
class DxbcInterfaceTests final : public QObject {
    Q_OBJECT
    QTemporaryDir temporary;
    QString root;
    std::map<uint32_t, Json> reports;
    std::map<uint32_t, std::vector<uint8_t>> shaders;
    std::map<QString, Json> layoutReports;
  private slots:
    void initTestCase() {
        root = qEnvironmentVariable("FLORA_DXBC_INTERFACE_EVIDENCE", temporary.path());
        QVERIFY(QDir().mkpath(root));
        for (uint32_t vendor : {0u, 4318u, 32902u}) {
            const auto directory = QDir(root).filePath(QString::number(vendor));
            QProcess probe;
            probe.start(QCoreApplication::applicationDirPath() + "/FloraDxbcInterfaceProbe.exe",
                        {directory, QString::number(vendor)});
            if (!probe.waitForFinished(60000)) {
                probe.kill();
                probe.waitForFinished(5000);
                QFAIL("Independent interface probe timed out");
            }
            QVERIFY2(probe.exitStatus() == QProcess::NormalExit && probe.exitCode() == 0,
                     probe.readAllStandardError().constData());
            auto &report = reports[vendor];
            report = Json::parse(read(directory + "/result.json"));
            QCOMPARE(report.at("slots").get<unsigned>(), 2u);
            QCOMPARE(report.at("cases").size(), size_t(4));
            QVERIFY(report.at("passed").get<bool>());
            QCOMPARE(report.at("vendor").get<uint32_t>(), vendor ? vendor : 5140u);
            shaders[vendor] = read(directory + "/shader.dxbc");
        }
    }
    void originalGpu_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<uint32_t>("mode");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            for (uint32_t mode = 0; mode < 4; ++mode)
                QTest::newRow(qPrintable(QString("%1-%2").arg(vendor).arg(mode))) << vendor << mode;
    }
    void originalGpu() {
        QFETCH(uint32_t, vendor);
        QFETCH(uint32_t, mode);
        try {
            const auto &item = reports.at(vendor).at("cases").at(mode);
            const auto bindings = resolveInterfaceBindings(shaders.at(vendor), classes(item));
            QCOMPARE(bindings.tables.size(), size_t(2));
            QCOMPARE(bindings.registers.at({29, {0}}).valid, uint8_t(mode < 2 ? 3 : 15));
            QCOMPARE(bindings.registers.at({29, {1}}).valid, uint8_t(mode < 2 ? 3 : 15));
            const Program program(shaders.at(vendor));
            const auto input = inputs(bindings, mode);
            Group group(program, input);
            uint32_t calls = 0, returns = 0;
            while (!group.finished()) {
                const auto step = group.step();
                const auto opcode = program.instructions().at(step.step.instruction).opcode;
                if (opcode == 120) {
                    ++calls;
                    QVERIFY(step.step.callstack.empty());
                }
                if (opcode == 62 && step.step.nextInstruction) {
                    ++returns;
                    QCOMPARE(step.step.callstack.size(), size_t(1));
                }
            }
            QCOMPARE(calls, 8u);
            QCOMPARE(returns, 8u);
            const auto &output = input.memory->buffers().at(1);
            QVERIFY(std::all_of(output.valid.begin(), output.valid.end(),
                                [](uint8_t value) { return value != 0; }));
            std::array<uint32_t, 4> actual{};
            std::memcpy(actual.data(), output.bytes.data(), sizeof actual);
            QCOMPARE(actual, (item.at("actual").get<std::array<uint32_t, 4>>()));
            QCOMPARE(actual, (item.at("expected").get<std::array<uint32_t, 4>>()));
            const Json evidence{{"vendor", vendor}, {"mode", mode},
                                {"native", actual}, {"bindings", bindings.identity},
                                {"calls", calls},   {"returns", returns}};
            QFile file(QDir(root).filePath(QString("native-%1-%2.json").arg(vendor).arg(mode)));
            QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::NewOnly));
            const auto text = evidence.dump(2);
            QCOMPARE(file.write(text.data(), qint64(text.size())), qint64(text.size()));
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
    void invalidBindings() {
        const auto &shader = shaders.at(0);
        const auto valid = classes(reports.at(0).at("cases").at(0));
        for (unsigned mode = 0; mode < 5; ++mode) {
            auto broken = valid;
            if (mode == 0)
                broken.pop_back();
            if (mode == 1)
                broken[0].instanceName = "absent";
            if (mode == 2)
                broken[0].descriptor[1] = 2;
            if (mode == 3)
                broken[0].descriptor[7] = 2;
            if (mode == 4) {
                broken[0].descriptor[7] = 1;
                broken[0].typeName = "absent";
            }
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, resolveInterfaceBindings(shader, broken));
        }
        const auto resolved = resolveInterfaceBindings(shader, valid);
        const Program program(shader);
        for (unsigned mode = 0; mode < 2; ++mode) {
            auto broken = inputs(resolved, 0);
            if (mode == 0)
                broken.interfaceTables.clear();
            else
                broken.interfaceTables[0] = UINT32_MAX;
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, ([&] {
                                         Group group(program, broken);
                                         while (!group.finished())
                                             group.step();
                                     }()));
        }
    }
    void layouts_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<QString>("fixture");
        QTest::addColumn<uint32_t>("mode");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            for (QString fixture : {"wide-array", "multi-array", "wide-multi-array"})
                for (uint32_t mode = 0; mode < 4; ++mode)
                    QTest::newRow(qPrintable(QString("%1-%2-%3").arg(vendor).arg(fixture).arg(mode)))
                        << vendor << fixture << mode;
    }
    void layouts() {
        QFETCH(uint32_t, vendor);
        QFETCH(QString, fixture);
        QFETCH(uint32_t, mode);
        try {
            const auto key = QString("layout-%1-%2").arg(vendor).arg(fixture);
            const auto directory = QDir(root).filePath(key);
            if (!layoutReports.contains(key)) {
                QProcess probe;
                probe.start(QCoreApplication::applicationDirPath() + "/FloraDxbcInterfaceProbe.exe",
                            {directory, QString::number(vendor), fixture});
                if (!probe.waitForFinished(60000)) {
                    probe.kill();
                    probe.waitForFinished(5000);
                    QFAIL("Independent class layout probe timed out");
                }
                QCOMPARE(probe.exitStatus(), QProcess::NormalExit);
                auto report = Json::parse(read(directory + "/result.json"));
                QCOMPARE(probe.exitCode(), report.at("passed").get<bool>() ? 0 : 1);
                layoutReports.emplace(key, std::move(report));
            }
            const auto &report = layoutReports.at(key);
            const auto &item = report.at("cases").at(mode);
            const auto bytecode = read(directory + "/shader.dxbc");
            const auto observed = classes(item);
            const auto resolved = resolveInterfaceBindings(bytecode, observed);
            QCOMPARE(resolved.tables.size(), fixture.contains("multi") ? size_t(4) : size_t(2));
            for (size_t slot = 0; slot < observed.size(); ++slot) {
                const auto &instance = observed[slot];
                const auto pointer = resolved.registers.at({29, {uint32_t(slot)}});
                QCOMPARE(pointer.valid, uint8_t(mode < 2 ? 3 : 15));
                const auto stride = fixture.contains("wide") ? 2u : 1u;
                QCOMPARE(pointer.bits[0], mode < 2 ? 1u : instance.descriptor[3]);
                QCOMPARE(pointer.bits[1], mode < 2 ? (instance.instanceName == "first" ? 0u : 2u * stride) +
                                                         instance.descriptor[1] * stride
                                                   : instance.descriptor[4]);
            }
            const Program program(bytecode);
            const auto input = inputs(resolved, mode, report);
            Group group(program, input);
            while (!group.finished())
                group.step();
            std::array<uint32_t, 4> actual{};
            const auto &buffer = input.memory->buffers().at(1);
            std::memcpy(actual.data(), buffer.bytes.data(), sizeof actual);
            QVERIFY(std::all_of(buffer.valid.begin(), buffer.valid.end(), [](auto v) { return v != 0; }));
            const auto expected = item.at("expected").get<std::array<uint32_t, 4>>();
            const auto original = item.at("actual").get<std::array<uint32_t, 4>>();
            const Json evidence{{"vendor", vendor},
                                {"fixture", fixture.toStdString()},
                                {"mode", mode},
                                {"native", actual},
                                {"authored", expected},
                                {"original_gpu", original},
                                {"bindings", resolved.identity},
                                {"native_matches_authored", actual == expected},
                                {"native_matches_original_gpu", actual == original}};
            QFile file(QDir(root).filePath(key + QString("-native-%1.json").arg(mode)));
            QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::NewOnly));
            const auto text = evidence.dump(2);
            QCOMPARE(file.write(text.data(), qint64(text.size())), qint64(text.size()));
            QCOMPARE(actual, expected);
            QCOMPARE(actual, original);
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
    void capturedTrace_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<bool>("created");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            for (bool created : {false, true})
                QTest::newRow(qPrintable(QString("%1-%2").arg(vendor).arg(created))) << vendor << created;
    }
    void capturedTrace() {
        QFETCH(uint32_t, vendor);
        QFETCH(bool, created);
        try {
            const auto stem = QDir(root).filePath(QString("capture-%1-%2").arg(vendor).arg(created));
            testing::classCapture(created).save(stem + ".gpa_frame");
            Frame frame((stem + ".gpa_frame").toStdWString());
            ReplayOptions options;
            options.warp = !vendor;
            if (vendor)
                options.vendor = vendor;
            options.until = 100;
            options.before = true;
            options.debug = true;
            Replay replay(frame, options);
            const auto snapshot = snapshotResourceInputs(replay, 5);
            QCOMPARE(snapshot.classBindings.size(), size_t(1));
            QCOMPARE(snapshot.classBindings[0]["object"], Json(62));
            QCOMPARE(snapshot.classBindings[0]["this_bits"][0], Json(created ? 2 : 0));
            QCOMPARE(snapshot.classBindings[0]["this_bits"][1], Json(created ? 3 : 1));
            const auto original = replay.readBuffer(7);
            const auto path = stem + ".jsonl";
            const auto result = exportGroupExecutionTrace(path, snapshot);
            QVERIFY(result.at("completed").get<bool>());
            QCOMPARE(replay.readBuffer(7), original);
            const auto &output = snapshot.inputs.memory->buffers().at(7);
            for (uint32_t i = 0; i < 4; ++i) {
                uint32_t word = 0;
                std::memcpy(&word, output.bytes.data() + i * 4, 4);
                QCOMPARE(word, (created ? 40u : 20u) + i);
            }
            options.before = false;
            Replay reference(frame, options);
            reference.run();
            const auto referenceBytes = reference.readBuffer(7);
            const auto journal = read(path);
            const auto digest =
                QCryptographicHash::hash(
                    QByteArrayView(reinterpret_cast<const char *>(journal.data()), qsizetype(journal.size())),
                    QCryptographicHash::Sha256)
                    .toHex()
                    .toStdString();
            ExecutionTraceReader reader(path, digest);
            QCOMPARE(reader.header().at("class_bindings"), snapshot.classBindings);
            const auto registers = reader.registers();
            const auto storage = reader.storage().at({TraceSpace::Buffer, 7});
            reader.moveTo(reader.size());
            QCOMPARE(reader.storage().at({TraceSpace::Buffer, 7}).bytes, output.bytes);
            reader.moveTo(0);
            QCOMPARE(reader.registers(), registers);
            QCOMPARE(reader.storage().at({TraceSpace::Buffer, 7}).bytes, storage.bytes);
            QCOMPARE(reader.storage().at({TraceSpace::Buffer, 7}).valid, storage.valid);
            NativeExecutionDebugModel model(path, digest, snapshot.bytecode);
            QCOMPARE(model.result().at("execution_context").at("class_bindings"), snapshot.classBindings);
            bool pointerDisplayed = false;
            for (const auto &row : model.registerRows("uint"))
                if (row.at(0) == "this[0]") {
                    pointerDisplayed = true;
                    QCOMPARE(row.at(1), Json(created ? "2, 3, 0, 0" : "0, 1, unknown, unknown"));
                }
            QVERIFY(pointerDisplayed);
            size_t inside = model.size();
            for (size_t i = 0; i < model.size(); ++i)
                if (model.rows().at(i).at("call_depth") == 1) {
                    inside = i;
                    break;
                }
            QVERIFY(inside < model.size());
            const auto instruction = model.rows().at(inside).at("instruction").get<uint64_t>();
            model.toggleInstruction(instruction);
            QCOMPARE(model.seek(0), inside);
            model.move(inside);
            QCOMPARE(model.instructionState().at("call_depth"), Json(1));
            model.move(0);
            QCOMPARE(model.instructionState().at("call_depth"), Json(0));
            QCOMPARE(output.bytes, referenceBytes);
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
    void emptyFunctionTable() {
        try {
            const auto stem = QDir(root).filePath("empty-function-table");
            testing::graphicsClassCapture(true).save(stem + ".gpa_frame");
            Frame frame((stem + ".gpa_frame").toStdWString());
            ReplayOptions options;
            options.warp = true;
            options.until = 200;
            options.before = true;
            Replay replay(frame, options);
            const auto snapshot = snapshotResourceInputs(replay, 4);
            const Program program(snapshot.bytecode);
            bool empty = false;
            for (const auto &instruction : program.instructions())
                if (instruction.opcode == 145 && instruction.words.at(2) == 0)
                    empty = true;
            QVERIFY(empty);
            std::array<Inputs, 4> lanes{snapshot.inputs, snapshot.inputs, snapshot.inputs, snapshot.inputs};
            PixelQuad quad(program, lanes);
            while (!quad.finished())
                quad.step();
            for (uint32_t i = 0; i < 4; ++i) {
                const auto output = quad.lane(i).value({2, {0}});
                QCOMPARE(output, Value::known({0x3f800000, 0, 0, 0x3f800000}));
            }
            options.before = false;
            Replay original(frame, options);
            original.run();
            QCOMPARE(original.output(20).rgba, (std::vector<uint8_t>{255, 0, 0, 255}));
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
    void equivalentClassInstances_data() {
        QTest::addColumn<bool>("edited");
        QTest::newRow("captured-snapshot") << false;
        QTest::newRow("edited-setter") << true;
    }
    void equivalentClassInstances() {
        QFETCH(bool, edited);
        try {
            auto capture = testing::classCapture();
            const auto entry = *std::find_if(capture.entries.begin(), capture.entries.end(),
                                             [](const auto &e) { return e.id == 62; });
            const std::vector<uint8_t> alias(capture.bytes.begin() + entry.offset,
                                             capture.bytes.begin() + entry.offset + entry.size);
            capture.add(63, 5, 0x98, alias);
            capture.add(85, 7, 0x3523, testing::statePack(Id(0), Id(1), Id(15), 1u, uint8_t(1), Id(62)));
            capture.add(90, 7, 0x3523, testing::statePack(Id(0), Id(1), Id(15), 1u, uint8_t(1), Id(63)));
            const auto path = QDir(root).filePath(QString("class-alias-%1.gpa_frame").arg(edited));
            capture.save(path);
            Frame frame(path.toStdWString());
            ReplayOptions options;
            options.warp = true;
            options.until = 100;
            options.before = true;
            if (edited)
                options.pipelineSetters[90] = readPipelineSetter(0x3523, frame.payload(90));
            Replay replay(frame, options);
            const auto snapshot = snapshotResourceInputs(replay, 5);
            replay.inspectNativeState([&](auto *context, const auto &objects) {
                QVERIFY(objects.at(62).Get());
                QVERIFY(objects.at(63).Get());
                Com<ID3D11ComputeShader> shader;
                Com<ID3D11ClassInstance> instance;
                UINT count = 1;
                context->CSGetShader(&shader, &instance, &count);
                QCOMPARE(count, 1u);
                QCOMPARE(static_cast<IUnknown *>(instance.Get()), objects.at(edited ? 63 : 62).Get());
            });
            QCOMPARE(snapshot.classBindings.at(0).at("object"), Json(edited ? 63 : 62));
            QCOMPARE(snapshot.inputs.registers.at({29, {0}}), (Value{{0, 1, 0, 0}, 3}));
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
    void malformedMetadata() {
        const auto &shader = shaders.at(0);
        const auto valid = classes(reports.at(0).at("cases").at(0));
        auto parts = readDxbcParts(shader);
        const auto index = size_t(std::find_if(parts.begin(), parts.end(),
                                               [](const auto &part) { return part.first == 0x45434649; }) -
                                  parts.begin());
        QVERIFY(index < parts.size());
        const auto metadata = parts[index].second;
        size_t required = metadata.size();
        while (required && metadata[required - 1] == 0xab)
            --required;
        for (size_t size = 0; size < metadata.size(); ++size) {
            parts[index].second = metadata.first(size);
            const auto truncated = makeDxbc(parts);
            // A trailing alignment byte is not required by the IFCE reader.
            if (size < required)
                QVERIFY_THROWS_EXCEPTION(std::runtime_error, resolveInterfaceBindings(truncated, valid));
            else
                QCOMPARE(resolveInterfaceBindings(truncated, valid).identity,
                         resolveInterfaceBindings(shader, valid).identity);
        }
        parts[index].second = metadata;
        for (size_t offset : {0u, 4u, 8u, 12u, 16u, 20u, 24u, 36u, 40u, 44u, 48u}) {
            auto corrupt = parts;
            std::vector<uint8_t> changed(metadata.begin(), metadata.end());
            std::fill_n(changed.begin() + offset, 4, uint8_t(255));
            corrupt[index].second = changed;
            const auto bytes = makeDxbc(corrupt);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, resolveInterfaceBindings(bytes, valid));
        }
    }
    void malformedProgram() {
        const auto &shader = shaders.at(0);
        const auto original = readDxbcParts(shader);
        const auto index = size_t(std::find_if(original.begin(), original.end(),
                                               [](const auto &part) { return part.first == 0x58454853; }) -
                                  original.begin());
        QVERIFY(index < original.size());
        for (uint32_t mode = 0; mode < 12; ++mode) {
            auto program = readDxbcProgram(original[index].second);
            auto row = [&](uint32_t opcode) -> std::vector<uint32_t> & {
                return *std::find_if(program.instructions.begin(), program.instructions.end(),
                                     [&](const auto &words) { return (words[0] & 2047) == opcode; });
            };
            if (mode == 0)
                row(145)[3] = 999; // Absent function body.
            if (mode == 1)
                row(146)[4] = 999; // Absent function table.
            if (mode == 2)
                row(146)[5] = row(146)[4]; // Duplicate table.
            if (mode == 3)
                row(146)[2] = 3; // Call-site count mismatch.
            if (mode == 4)
                row(146)[1] = 1; // Noncontiguous interface numbering.
            if (mode == 5)
                row(146)[3] = 254u << 16 | 2; // Slot limit.
            if (mode == 6)
                row(120)[1] = 2; // Call-site index exceeds declaration.
            if (mode == 7)
                row(120)[3] = 99; // Undeclared interface.
            if (mode == 8)
                row(146)[0] &= ~0x800u; // Relative call requires dynamic indexing.
            if (mode == 9)
                row(44)[2] = 999; // Undeclared function label.
            if (mode == 10) {
                auto call = row(120);
                auto label = std::find_if(program.instructions.begin(), program.instructions.end(),
                                          [](const auto &words) { return (words[0] & 2047) == 44; });
                program.instructions.insert(label + 1, call); // Potential recursion through a table.
            }
            if (mode == 11) {
                auto &table = row(145);
                table.pop_back();
                table[2] = 1;
                table[0] = uint32_t(table.size()) << 24 | 145;
            }
            auto parts = original;
            const auto bytes = writeDxbcProgram(program);
            parts[index].second = bytes;
            const auto malformed = makeDxbc(parts);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, Program{malformed});
        }
    }
};
QTEST_GUILESS_MAIN(DxbcInterfaceTests)
#include "DxbcInterfaceTests.moc"
