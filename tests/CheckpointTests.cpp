#include "CheckpointGpu.h"
#include "application/CheckpointInspection.h"
#include "application/DxbcCheckpoint.h"
#include "application/DxbcCheckpointModel.h"
#include "application/InvocationSelector.h"
#include "application/ShaderDebugData.h"
#include "application/ShaderSourceLines.h"
#include "application/SourceStack.h"
#include "application/SourceVariables.h"
#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>
using namespace flora;
using namespace flora::checkpoint;
namespace {
std::vector<uint8_t> read(const std::string &path) {
    QFile f(QString::fromStdString(path));
    if (!f.open(QIODevice::ReadOnly))
        throw std::runtime_error("Cannot read checkpoint test input");
    auto bytes = f.readAll();
    return {bytes.begin(), bytes.end()};
}
Json accessJson(const ArrayAccess &a) {
    return Json::array({a.array, a.offset, a.relative ? Json(*a.relative) : Json(nullptr)});
}
Json inputsJson(const DeclaredInputs &inputs, bool domain) {
    Json operands = Json::array();
    for (const auto &slot : inputs.inputSlots) {
        Json row = Json::array({inputOperand(slot)});
        for (unsigned c = 0; c < 4; ++c)
            row.push_back(inputOperand(slot, c));
        operands.push_back(row);
    }
    Json result{{"slots", inputs.inputSlots},
                {"known", inputs.known},
                {"identity", inputs.identity},
                {"operands", operands}};
    if (domain)
        result["domain"] = inputs.domain;
    return result;
}
Json evaluate(const Json &j) {
    const auto op = j.at("op").get<std::string>();
    if (op == "source-stack") {
        const auto raw = read(j.at("input"));
        auto symbols = j.contains("symbols") ? j.at("symbols") : sourceVariables(raw);
        auto result =
            sourceStack(raw, symbols, j.contains("source") ? j.at("source") : shaderSourceLines(raw));
        Json queries = Json::array();
        for (const auto &q : j.value("queries", Json::array())) {
            auto selected = sourceStackAt(result, q.at("offset"), q.value("depth", 0u));
            Json locations = Json::object();
            for (const auto &frame : result.at("frames"))
                locations[frame.at("id").get<std::string>()] =
                    sourceFrameLocation(result, frame.at("id"), q.at("offset"));
            queries.push_back({{"stack", selected}, {"locations", locations}});
        }
        return {{"model", result}, {"symbols", symbols}, {"queries", queries}};
    }
    if (op == "inline-compressed") {
        const auto bytes = j.at("bytes").get<std::vector<uint8_t>>();
        const auto [value, next] = codeview::compressed(bytes, j.value("offset", size_t(0)));
        return Json::array({value, next});
    }
    if (op == "inline-ranges")
        return codeview::statementRanges(j.at("bytes").get<std::vector<uint8_t>>(), j.at("base"),
                                         j.at("boundaries").get<std::set<uint64_t>>(),
                                         j.value("details", false));
    if (op == "stack-at")
        return sourceStackAt(j.at("model"), j.at("offset"), j.value("depth", 0u));
    if (op == "frame-locals")
        return sourceFrameLocals(j.at("symbols"), j.at("values"), j.at("frame"));
    if (op == "frame-location")
        return sourceFrameLocation(j.at("model"), j.at("frame"), j.at("offset"));
    if (op == "source-variables")
        return sourceVariables(read(j.at("input")));
    if (op == "resolve-source-variables" && j.contains("input")) {
        // Cache only immutable probe fixture models; production builds one per capture.
        static std::map<std::string, Json> models;
        const auto path = j.at("input").get<std::string>();
        if (!models.contains(path)) {
            const auto raw = read(path);
            auto symbols = sourceVariables(raw);
            sourceStack(raw, symbols, shaderSourceLines(raw));
            models[path] = std::move(symbols);
        }
        return resolveSourceVariables(models.at(path), j.at("registers"), j.at("metadata"), j.at("hit"),
                                      j.at("offset"));
    }
    if (op == "resolve-source-variables")
        return resolveSourceVariables(j.at("model"), j.at("registers"), j.at("metadata"), j.at("hit"),
                                      j.at("offset"));
    if (op == "codeview-numeric") {
        const auto bytes = j.at("bytes").get<std::vector<uint8_t>>();
        const auto [value, next] = codeview::numeric(bytes, j.value("offset", size_t(0)));
        return Json::array({value, next});
    }
    if (op == "source-type") {
        codeview::Records records;
        for (const auto &row : j.at("records"))
            records[row.at("id").get<uint32_t>()] = {row.at("kind").get<uint16_t>(),
                                                     row.at("bytes").get<std::vector<uint8_t>>()};
        return codeview::Types(std::move(records)).get(j.at("index"));
    }
    if (op == "source-text") {
        const auto bytes = j.at("bytes").get<std::vector<uint8_t>>();
        const auto text = shader_debug::decodeSourceText(bytes);
        return {{"text", text.text}, {"encoding", text.encoding}, {"text_valid", text.valid}};
    }
    if (op == "source-lines") {
        auto report = shaderSourceLines(read(j.at("input")),
                                        j.contains("assembly") ? std::optional<std::string>(j.at("assembly"))
                                                               : std::nullopt);
        return {{"report", report}, {"by_offset", shaderLinesByOffset(report)}};
    }
    if (op == "source-path")
        return shader_debug::sourcePathKey(j.at("path"));
    if (op == "source-offsets")
        return shaderLinesByOffset(j.at("report"));
    if (op == "inlinee-sources") {
        std::map<uint32_t, Json> checksums;
        for (const auto &[key, value] : j.at("checksums").items())
            checksums[std::stoul(key)] = value;
        return shader_debug::inlineeSources(j.at("tables").get<std::vector<std::vector<uint8_t>>>(),
                                            checksums);
    }
    if (op == "instrument") {
        CheckpointOptions options;
        options.stage = j.at("stage");
        options.slot = j.at("slot");
        options.capacity = j.at("capacity");
        if (j.at("checkpoint") != "trace") {
            const auto &token = j.at("checkpoint");
            if (!token.is_number_integer() || token.get<int64_t>() < 0 || token.get<uint64_t>() > UINT32_MAX)
                throw std::runtime_error("Invalid checkpoint token");
            options.token = token.get<uint32_t>();
        }
        if (j.contains("phase") && !j.at("phase").is_null())
            options.hullPhase = j.at("phase").get<uint32_t>();
        options.inputSelector = j.value("selector", Json(nullptr));
        auto patched = instrumentCheckpoint(read(j.at("input")), options);
        QFile output(QString::fromStdString(j.at("output").get<std::string>()));
        if (!output.open(QIODevice::WriteOnly) ||
            output.write(reinterpret_cast<const char *>(patched.bytes.data()), patched.bytes.size()) !=
                qint64(patched.bytes.size()))
            throw std::runtime_error("Cannot write checkpoint probe DXBC");
        return patched.metadata;
    }
    if (op == "domain")
        return inputsJson(domainInputs(j.at("rows").get<Rows>()), true);
    if (op == "hull")
        return inputsJson(hullInputs(j.at("globals").get<Rows>(), j.at("rows").get<Rows>(), j.at("phase")),
                          false);
    if (op == "phases")
        return hullPhases(j.at("rows").get<Rows>());
    if (op == "array-operand")
        return indexableOperand(
            j.at("array"), j.at("index"), j.value("mask", 15u),
            j.contains("swizzle") ? std::optional<uint32_t>(j.at("swizzle")) : std::nullopt,
            j.contains("component") ? std::optional<uint32_t>(j.at("component")) : std::nullopt,
            j.contains("relative") ? std::optional<Words>(j.at("relative").get<Words>()) : std::nullopt);
    if (op == "program") {
        auto p = program(read(j.at("input")), j.at("stage"));
        return {{"profile", p.profile}, {"split", p.split}, {"catalog", p.catalog}};
    }
    if (op == "calls") {
        auto g = subroutines(j.at("rows").get<Rows>(), j.at("split"));
        Json owners = Json::object(), targets = Json::object(), labels = Json::object();
        for (const auto &[i, label] : g.owners)
            owners[std::to_string(i)] = label ? Json(*label) : Json(nullptr);
        for (const auto &[i, target] : g.targets)
            targets[std::to_string(i)] = target;
        for (const auto &[label, i] : g.labels)
            labels[std::to_string(label)] = i;
        return {{"owners", owners}, {"targets", targets}, {"labels", labels}, {"depth", g.depth}};
    }
    if (op == "instruction") {
        auto parsed = instructionOperands(j.at("words").get<Words>());
        return {{"prefix", parsed.prefix},
                {"ranges", parsed.ranges},
                {"dependent", dependentResult(j.at("words").get<Words>(),
                                              indexableDeclarations(j.at("declarations").get<Rows>()))}};
    }
    if (op == "arrays") {
        const auto arrays = indexableDeclarations(j.at("declarations").get<Rows>());
        Json decls = Json::array();
        for (const auto &a : arrays)
            decls.push_back({{"array", a.array},
                             {"elements", a.elements},
                             {"components", a.components},
                             {"mask", a.mask}});
        Json result{{"declarations", decls}};
        if (j.contains("words")) {
            const auto words = j.at("words").get<Words>();
            Json accesses = Json::array(), replacements = Json::array();
            for (const auto &a : indexableAccesses(words, arrays))
                accesses.push_back(accessJson(a));
            unsigned index = 0;
            auto rewritten = rewriteIndices(words, [&](const auto &a) {
                replacements.push_back(accessJson(a));
                return Words{0x10000a | ((index++ % 4) << 4), 91};
            });
            result.update({{"accesses", accesses}, {"rewritten", rewritten}, {"replacements", replacements}});
            if (j.value("destination", false)) {
                const auto d = indexableDestination(words, arrays);
                result["destination"] =
                    Json::array({d.access.array, d.mask, d.access.offset,
                                 d.access.relative ? Json(*d.access.relative) : Json(nullptr)});
            }
        }
        return result;
    }
    if (op == "changes")
        return changesIndex(j.at("destination").get<Words>(), j.at("relative").is_null()
                                                                  ? std::optional<Words>{}
                                                                  : j.at("relative").get<Words>());
    if (op == "selector")
        return validateSelector(j.at("selector"), j.at("shader").get<std::vector<uint8_t>>(), j.at("stage"),
                                j.at("slots"), j.at("known"), j.value("phase", Json(nullptr)));
    if (op == "snapshot")
        return selectorFromSnapshot(j.at("result"), j.at("registers"), j.at("row"),
                                    j.value("policy", std::string("unique")));
    if (op == "verify") {
        verifySelectorRecords(j.at("data").get<std::vector<uint8_t>>(), j.at("metadata"));
        return true;
    }
    throw std::runtime_error("Unknown checkpoint probe operation");
}
int probe(const std::string &path) {
    const auto jobs = Json::parse(read(path));
    Json results = Json::array();
    for (const auto &j : jobs) {
        Json result{{"name", j.at("name")}};
        try {
            result["value"] = evaluate(j);
            result["success"] = true;
        } catch (const std::exception &error) {
            result["success"] = false;
            result["error"] = error.what();
        }
        results.push_back(result);
    }
    QFile output(QString::fromStdString(path + ".results.json"));
    const auto text = results.dump(2);
    if (!output.open(QIODevice::WriteOnly) || output.write(text.data(), text.size()) != qint64(text.size()))
        throw std::runtime_error("Cannot write checkpoint probe results");
    return 0;
}
} // namespace
class CheckpointTests final : public QObject {
    Q_OBJECT
  private slots:
    void sourceStackBoundaries() {
        const std::vector<uint8_t> annotations{3, 0, 6, 2, 3, 4, 4, 4};
        const auto ranges = codeview::statementRanges(annotations, 8, {8, 12, 16}, true);
        QCOMPARE(ranges.size(), size_t(2));
        QVERIFY(ranges[0]["start"] == 8 && ranges[0]["end"] == 12 && ranges[0]["line_delta"] == 0);
        QVERIFY(ranges[1]["start"] == 12 && ranges[1]["end"] == 16 && ranges[1]["line_delta"] == 1);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, codeview::statementRanges(annotations, 8, {8, 16}));
        const std::vector<uint8_t> unfinished{3, 0};
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, codeview::statementRanges(unfinished, 8, {8, 12}));
        auto model = Json::parse(R"({"status":"available","frames":[
          {"id":"main","name":"main","kind":"function","parent":null,"ranges":[{"start":8,"end":32}]},
          {"id":"leaf","name":"leaf","kind":"inline","parent":"main","ranges":[{"start":12,"end":20}]}]})");
        QVERIFY(sourceStackAt(model, 12)["frames"].size() == 2);
        QVERIFY(sourceStackAt(model, 20)["frames"].size() == 1);
        QVERIFY(sourceStackAt(model, 32)["status"] == "unmapped");
        QVERIFY(sourceStackAt(model, 12, 1)["status"] == "unmapped_subroutine");
        model["hs_phases"] = Json::array(
            {{{"id", 1}, {"kind", "fork"}, {"start", 8}, {"end", 32}, {"frame_ids", Json::array({"leaf"})}}});
        model["scope_semantics"] = "phase_local";
        QVERIFY(sourceStackAt(model, 12)["frames"].size() == 1);
        QVERIFY(sourceStackAt(model, 12)["frames"][0]["id"] == "leaf");
        QVERIFY(sourceStackAt(model, 20)["status"] == "unmapped_hs_phase_scope");
        model["frames"][0]["parent"] = "leaf";
        QVERIFY(sourceStackAt(model, 12)["status"] == "ambiguous");
        Json symbols = Json::parse(R"({"scopes":[{"id":"main","parent":null,"kind":"function"},
          {"id":"block","parent":"main","kind":"block"},{"id":"leaf","parent":"block","kind":"inline"}]})");
        const auto values = Json::parse(R"([{"scope_id":"main"},{"scope_id":"block"},{"scope_id":"leaf"}])");
        QVERIFY(sourceFrameLocals(symbols, values, "main") == Json::array({values[0], values[1]}));
        QVERIFY(sourceFrameLocals(symbols, values, "leaf") == Json::array({values[2]}));
    }
    void sourceValueValidity() {
        auto model = Json::parse(R"({"scopes":[{"id":"main","name":"main","kind":"function"}],
          "variables":[{"id":"wide","scope":"main","name":"wide",
          "type":{"name":"double","size":8,"leaves":[{"path":"","offset":0,"size":8,"type":"double"}]},
          "ranges":[{"register_type":0,"flags":1,"variable_offset":0,"size":8,
          "start":8,"end":32,"gaps":[[12,4]],"register_offsets":[0]}]}]})");
        auto regs =
            Json::parse(R"([{"name":"r0","bits":[0,1074003968,1,2],"written":[true,true,true,true]}])");
        auto values = resolveSourceVariables(model, regs, Json::object(), Json::object(), 16);
        QVERIFY(values.at(0).at("value") == "2.5");
        QVERIFY(values.at(0).at("references") == Json::array({"r0.x", "r0.y"}));
        for (auto offset : {7u, 20u, 23u, 32u}) {
            values = resolveSourceVariables(model, regs, Json::object(), Json::object(), offset);
            QVERIFY(values.at(0).at("status") == "unavailable");
            QVERIFY(values.at(0).at("bits").is_null());
        }
        regs[0]["written"][1] = false;
        values = resolveSourceVariables(model, regs, Json::object(), Json::object(), 16);
        QVERIFY(values.at(0).at("status") == "unavailable");
        regs[0]["written"][1] = true;
        auto alias = model["variables"][0]["ranges"][0];
        alias["register_offsets"] = Json::array({8});
        model["variables"][0]["ranges"].push_back(alias);
        values = resolveSourceVariables(model, regs, Json::object(), Json::object(), 16);
        QVERIFY(values.at(0).at("status") == "ambiguous");
        QVERIFY(values.at(0).at("bits").is_null());
        // Without phase ownership, an HS snapshot cannot borrow arbitrary locals.
        values = resolveSourceVariables(model, regs, Json{{"shader_stage", "hs"}, {"hs_phase", {{"id", 0}}}},
                                        Json::object(), 16);
        QVERIFY(values.empty());
    }
    void sourceTypeBounds() {
        const std::vector<uint8_t> negative{0, 128, 255};
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, codeview::numeric(negative, 0));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, codeview::numeric(negative, 99));
        codeview::Types recursive({{0x1000, {0x1001, {0, 16, 0, 0}}}});
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, recursive.get(0x1000));
        const std::vector<uint8_t> truncated(55);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, codeview::records(truncated));
    }
    void decodedRecordsAndExports() {
        Json meta = Json::parse(R"({"record_stride":64,"registers":1,"records":3,
          "invocations":1,"instance_count":1,"shader_stage":"gs","trace":true,
          "call_depth_offset":24,"known_inputs":{},
          "register_slots":[{"name":"r0","kind":"temporary","register":0,"mask":15}],
          "checkpoints":[{"token":0,"opcode":4,"call_target":9,"function_label":null},
                         {"token":1,"opcode":62,"function_label":9},
                         {"token":2,"opcode":62,"function_label":null}]})");
        std::vector<uint8_t> bytes(3 * 64);
        auto put = [&](size_t offset, uint32_t value) { std::memcpy(bytes.data() + offset, &value, 4); };
        for (unsigned r = 0; r < 3; ++r) {
            put(r * 64 + 4, r);
            put(r * 64 + 16, r);
            put(r * 64 + 20, r ? 62 : 4);
            put(r * 64 + 24, r == 1 ? 1 : 0);
            for (unsigned c = 0; c < 3; ++c)
                put(r * 64 + 48 + c * 4, 1);
            put(r * 64 + 32, 0x7fc12345); // Preserve NaN payloads even when float formatting is "nan".
            put(r * 64 + 36, 0xff800000);
            put(r * 64 + 40, 0x80000000);
            put(r * 64 + 44, 42); // Unwritten raw storage must not appear as a defined CSV value.
        }
        auto rows = checkpointHeaders(bytes, meta);
        QCOMPARE(rows.size(), size_t(3));
        QVERIFY(rows[0]["primitive_id"].is_null());
        QVERIFY(rows[0]["call_stack"].empty());
        QVERIFY(rows[1]["call_stack"] == Json::array({{{"label", 9}, {"call_token", 0}}}));
        QVERIFY(rows[2]["call_stack"].empty());
        const auto regs = checkpointRegisters(bytes, meta, 1);
        QCOMPARE(regs[0]["bits"][0].get<uint32_t>(), 0x7fc12345u);
        QVERIFY(regs[0]["written"] == Json::array({true, true, true, false}));
        for (auto [offset, bad] : std::vector<std::pair<size_t, uint32_t>>{
                 {0, 1}, {4, 1}, {16, 3}, {20, 62}, {24, 33}, {64 + 24, 0}, {128 + 24, 1}}) {
            uint32_t saved;
            std::memcpy(&saved, bytes.data() + offset, 4);
            put(offset, bad);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, checkpointHeaders(bytes, meta));
            put(offset, saved);
        }
        put(48, 2);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, checkpointRegisters(bytes, meta, 0));
        put(48, 1);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, checkpointRegisters(bytes, meta, 3));
        auto badMeta = meta;
        badMeta["record_stride"] = 0;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, checkpointHeaders(bytes, badMeta));
        badMeta = meta;
        badMeta["registers"] = 4097;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, checkpointHeaders(bytes, badMeta));
        auto truncated = bytes;
        truncated.pop_back();
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, checkpointHeaders(truncated, meta));

        CheckpointInspection capture;
        capture.shader = {1, 2, 3, 4};
        capture.assembly = "original assembly\n";
        capture.report = {{"catalog", Json::array({{{"token", 0}, {"instruction", 7}},
                                                   {{"token", 1}, {"instruction", 11}},
                                                   {{"token", 2}, {"instruction", 8}}})}};
        completeCheckpointInspection(capture, meta, bytes);
        QCOMPARE(capture.report["hits_preview"][1]["call_stack"][0]["call_instruction"].get<unsigned>(), 7u);
        QTemporaryDir directory;
        exportCheckpoint(capture, std::filesystem::path(directory.path().toStdWString()));
        QFile file(directory.path() + "/registers.csv");
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto csv = file.readAll();
        QVERIFY(csv.contains("0,r0,x,True,0x7fc12345,2143363909,2143363909,nan\r\n"));
        QVERIFY(csv.contains("0,r0,y,True,0xff800000,4286578688,-8388608,-inf\r\n"));
        QVERIFY(csv.contains("0,r0,z,True,0x80000000,2147483648,-2147483648,-0.0\r\n"));
        QVERIFY(csv.contains("0,r0,w,False,,,,\r\n"));
        QCOMPARE(read((directory.path() + "/snapshots.bin").toStdString()), bytes);
    }
    void nativeSnapshots() {
        try {
            flora::testing::validateCheckpointGpu();
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
    void nestedAddressOrder() {
        const auto arrays = indexableDeclarations({{0x04000069, 7, 8, 4}, {0x04000069, 3, 4, 2}});
        const auto nested = indexableOperand(7, 0, 15, {}, 1, indexableOperand(3, 2, 3, {}, 0));
        const auto accesses = indexableAccesses(nested, arrays);
        QCOMPARE(accesses.size(), size_t(2));
        QCOMPARE(accesses[0].array, 3u);
        QCOMPARE(accesses[0].offset, 2u);
        QCOMPARE(accesses[1].array, 7u);
        std::vector<ArrayAccess> visited;
        const auto rewritten = rewriteIndices(nested, [&](const auto &access) {
            visited.push_back(access);
            return Words{0x10000a, uint32_t(100 + visited.size())};
        });
        QCOMPARE(visited.size(), size_t(2));
        QVERIFY(visited[1].relative == Words({0x0420300a, 3, 0x10000a, 101}));
        QVERIFY(rewritten == Words({0x0420301a, 7, 0x10000a, 102}));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, indexableAccesses(indexableOperand(3, 4), arrays));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, indexableDestination(indexableOperand(3, 1), arrays));
        auto depth = Words{0x10000a, 0};
        for (unsigned i = 0; i < 34; ++i) {
            Words next{0x420300a, 7};
            next.insert(next.end(), depth.begin(), depth.end());
            depth = next;
        }
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, indexableAccesses(depth, arrays));
    }
    void callGraph() {
        Rows code{{0x03000004, 0x10a000, 4},
                  {0x03000004, 0x10a000, 9},
                  {0x0100003e},
                  {0x0300002c, 0x10a000, 4},
                  {0x03000004, 0x10a000, 9},
                  {0x0100003e},
                  {0x0300002c, 0x10a000, 9},
                  {0x0100003e}};
        const auto graph = subroutines(code, 0);
        QCOMPARE(graph.depth, 2u);
        QVERIFY(!graph.owners.at(0));
        QCOMPARE(*graph.owners.at(4), 4u);
        code.insert(code.end() - 1, {0x03000004, 0x10a000, 4});
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, subroutines(code, 0));
        // A long static chain must be bounded by storage, not the host call stack.
        Rows deep{{0x03000004, 0x10a000, 0}, {0x0100003e}};
        for (unsigned i = 0; i < 4096; ++i) {
            deep.push_back({0x0300002c, 0x10a000, i});
            if (i != 4095)
                deep.push_back({0x03000004, 0x10a000, i + 1});
            deep.push_back({0x0100003e});
        }
        QCOMPARE(subroutines(deep, 0).depth, 4096u);
    }
    void selectorFiles() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        auto path = std::filesystem::path(directory.path().toStdWString()) / L"input-selector.json";
        const Json selector{
            {"format", "FloraGPA native input selector 1"}, {"name", "纹理"}, {"bits", 0xffffffffu}};
        writeSelector(path, selector);
        QVERIFY(readSelector(path) == selector);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, writeSelector(path, std::string(1024 * 1024, 'x')));
        QVERIFY(readSelector(path) == selector); // Failed serialization must preserve the previous file.
        QFile f(QString::fromStdWString(path.wstring()));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(QByteArray(1024 * 1024 + 1, ' '));
        f.close();
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, readSelector(path));
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(QByteArray(150, '[') + QByteArray(150, ']'));
        f.close();
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, readSelector(path));
    }
    void selectorRecordBounds() {
        const Json meta = Json::parse(R"({"record_stride":64,"registers":1,"matched_invocations":1,
        "register_slots":[{"name":"v[0][0]"}],
        "input_selector":{"inputs":[{"name":"v[0][0]","component":0,"bits":2143363909}]}})");
        std::vector<uint8_t> bytes(64);
        auto put = [&](size_t offset, uint32_t value) { std::memcpy(bytes.data() + offset, &value, 4); };
        put(32, 0x7fc12345);
        put(48, 1);
        verifySelectorRecords(bytes, meta);
        put(32, 0x7fc00000); // NaN payloads remain distinct raw inputs.
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, verifySelectorRecords(bytes, meta));
        put(32, 0x7fc12345);
        put(48, 0);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, verifySelectorRecords(bytes, meta));
        put(48, 1);
        bytes.pop_back();
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, verifySelectorRecords(bytes, meta));
        Json bad = meta;
        bad["record_stride"] = 0;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, verifySelectorRecords({}, bad));
    }
};
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (argc == 3 && std::string(argv[1]) == "--probe") {
        try {
            return probe(argv[2]);
        } catch (const std::exception &error) {
            qCritical("%s", error.what());
            return 1;
        }
    }
    CheckpointTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "CheckpointTests.moc"
