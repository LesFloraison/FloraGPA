#include "application/DxbcExecutionTraceReader.h"
#include "application/NativeExecutionDebugModel.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QSaveFile>
#include <iostream>
using namespace flora;
using namespace flora::dxbc_exec;
using Json = nlohmann::json;
namespace {
std::string hashBytes(Bytes bytes) {
    return QCryptographicHash::hash(
               QByteArrayView(reinterpret_cast<const char *>(bytes.data()), bytes.size()),
               QCryptographicHash::Sha256)
        .toHex()
        .toStdString();
}
std::string hash(const Json &value) {
    const auto text = value.dump();
    return hashBytes(Bytes(reinterpret_cast<const uint8_t *>(text.data()), text.size()));
}
Json state(const ExecutionTraceReader &trace) {
    Json registers = Json::array(), storage = Json::array(), counters = Json::array();
    for (const auto &[key, v] : trace.registers())
        registers.push_back({{"lane", key.first},
                             {"dxbc_operand_kind", key.second.kind},
                             {"indices", key.second.indices},
                             {"bits", v.bits},
                             {"valid_components", v.valid}});
    for (const auto &[key, b] : trace.storage())
        storage.push_back({{"space", key.first == TraceSpace::Buffer   ? "buffer"
                                     : key.first == TraceSpace::Shared ? "shared"
                                                                       : "texture"},
                           {"resource", key.second},
                           {"bytes", b.bytes.size()},
                           {"sha256", hashBytes(b.bytes)},
                           {"validity_sha256", hashBytes(b.valid)}});
    for (const auto &[id, c] : trace.counters())
        counters.push_back({{"view", id},
                            {"kind", c.kind == CounterKind::Append ? "append" : "ordered"},
                            {"value", c.value ? Json(*c.value) : Json(nullptr)}});
    return {{"registers_sha256", hash(registers)},
            {"registers", registers.size()},
            {"storage", storage},
            {"counters", counters}};
}
void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}
} // namespace
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    try {
        const auto args = app.arguments();
        require(
            args.size() == 4 || (args.size() == 6 && args[4] == "--shader"),
            "Usage: FloraDxbcTraceReaderProbe <trace.jsonl> <sha256> <report.json> [--shader <shader.dxbc>]");
        QElapsedTimer timer;
        timer.start();
        ExecutionTraceReader trace(args[1], args[2].toStdString());
        const auto readMs = timer.elapsed();
        const auto initial = state(trace);
        Json selected = Json::array();
        for (uint64_t i = 0; i < trace.selectedSize(); ++i) {
            trace.moveTo(trace.selectedBoundary(i));
            const auto step = trace.step(trace.selectedBoundary(i));
            selected.push_back({{"position", trace.position()},
                                {"instruction", step.at("instruction")},
                                {"state_sha256", hash(state(trace))}});
        }
        trace.moveTo(trace.size());
        const auto final = state(trace);
        trace.moveTo(0);
        require(state(trace) == initial, "Execution trace did not restore its exact initial state");
        Json report{
            {"format", "flora.dxbc-trace-reader-probe"},
            {"version", 1},
            {"trace_sha256", args[2].toStdString()},
            {"header", trace.header()},
            {"completion", trace.completion()},
            {"initial", initial},
            {"final", final},
            {"selected_boundaries", selected},
            {"round_trip", true},
            {"read_validate_rewind_ms", readMs},
            {"total_ms", timer.elapsed()},
            {"scope",
             "journal_integrity_and_state_projection_not_instruction_semantics_or_invocation_eligibility"}};
        if (args.size() == 6) {
            QFile shaderFile(args[5]);
            require(shaderFile.open(QIODevice::ReadOnly) && shaderFile.size() <= 64 * 1024 * 1024,
                    "Cannot read a bounded native debug shader");
            const auto shader = shaderFile.readAll();
            NativeExecutionDebugModel model(
                args[1], args[2].toStdString(),
                Bytes(reinterpret_cast<const uint8_t *>(shader.constData()), shader.size()));
            const auto initialRegisters = model.registerValues();
            size_t mapped = 0, sourceValues = 0;
            for (size_t i = 0; i < model.size(); ++i) {
                model.move(i);
                mapped += !model.location().is_null();
                sourceValues += model.sourceValues().size();
            }
            model.move(model.size());
            const auto terminal = model.instructionState();
            model.move(0);
            require(model.registerValues() == initialRegisters,
                    "Native model did not restore initial registers");
            const auto config = model.configuration();
            model.importConfiguration(config);
            require(model.configuration() == config, "Native model configuration did not round-trip");
            report["debug_model"] = {{"shader_sha256", model.result().at("shader_sha256")},
                                     {"selected_steps", model.size()},
                                     {"mapped_steps", mapped},
                                     {"source_value_records", sourceValues},
                                     {"symbol_status", model.result().at("source_variables").at("status")},
                                     {"terminal", terminal},
                                     {"register_round_trip", true},
                                     {"configuration_round_trip", true}};
        }
        QSaveFile file(args[3]);
        file.setDirectWriteFallback(false);
        require(file.open(QIODevice::WriteOnly), "Cannot open reader probe report");
        const auto text = report.dump(2) + "\n";
        require(file.write(text.data(), qint64(text.size())) == qint64(text.size()) && file.commit(),
                "Cannot publish reader probe report");
        std::cout << Json{{"ok", true},
                          {"steps", trace.size()},
                          {"selected_steps", trace.selectedSize()},
                          {"total_ms", timer.elapsed()}}
                         .dump()
                  << '\n';
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
