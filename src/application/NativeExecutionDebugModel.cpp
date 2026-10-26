#include "NativeExecutionDebugModel.h"
#include "ShaderSourceLines.h"
#include "SourceStack.h"
#include "SourceVariables.h"
#include "SystemDisassembly.h"
#include <QScopedValueRollback>
#include <QString>
#include <algorithm>
#include <bit>

namespace flora {
namespace {
using Json = nlohmann::json;
void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}
std::string registerName(const dxbc_exec::Register &r) {
    const auto &i = r.indices;
    if (i.size() == 1 && (r.kind == 0 || r.kind == 1 || r.kind == 2))
        return std::string(r.kind == 0 ? "r" : r.kind == 1 ? "v" : "o") + std::to_string(i[0]);
    if (r.kind == 3 && i.size() == 2)
        return "x" + std::to_string(i[0]) + "[" + std::to_string(i[1]) + "]";
    if (r.kind == 8 && i.size() == 2)
        return "cb" + std::to_string(i[0]) + "[" + std::to_string(i[1]) + "]";
    if (i.empty()) {
        if (r.kind == 32)
            return "vThreadID";
        if (r.kind == 33)
            return "vThreadGroupID";
        if (r.kind == 34)
            return "vThreadIDInGroup";
        if (r.kind == 36)
            return "vThreadIDInGroupFlattened";
    }
    std::string result = "operand" + std::to_string(r.kind);
    for (auto index : i)
        result += "[" + std::to_string(index) + "]";
    return result;
}
Json registerRecord(const dxbc_exec::Register &r, const dxbc_exec::Value &v) {
    Json valid = Json::array();
    for (uint32_t i = 0; i < 4; ++i)
        valid.push_back(bool(v.valid & (1u << i)));
    return {{"name", registerName(r)}, {"dxbc_operand_kind", r.kind}, {"indices", r.indices},
            {"bits", v.bits},          {"valid_components", v.valid}, {"written", valid}};
}
} // namespace

NativeExecutionDebugModel::NativeExecutionDebugModel(const QString &path, const std::string &digest,
                                                     Bytes shader, const dxbc_exec::TraceReadLimits &limits,
                                                     const CancelCheck &cancelled)
    : trace_(path, digest, limits, cancelled) {
    const auto header = trace_.header();
    const auto shaderHash = sha256(shader);
    require(header.at("shader_sha256") == shaderHash,
            "Native debug shader does not match the execution trace");
    const dxbc_exec::Program program(shader);
    require(program.stage() == 5 && header.at("group_size") == Json(program.threadGroupSize()) &&
                program.instructions().size() == trace_.instructionCount(),
            "Native debug program identity mismatch");
    lane_ = header.at("selected_lane");
    auto source = shaderSourceLines(shader);
    require(trace_.source() == source, "Native debug source metadata does not match the shader");
    const auto locations = shaderLinesByOffset(source);
    auto symbols = sourceVariables(shader);
    auto stack = sourceStack(shader, symbols, source);
    Json catalog = Json::array(), arrays = Json::array();
    for (size_t i = 0; i < program.instructions().size(); ++i) {
        checkCancellation(cancelled);
        const auto &instruction = program.instructions()[i];
        const auto recorded = trace_.instruction(i);
        const auto location =
            locations.value(std::to_string(uint64_t(instruction.wordOffset) * 4), Json(nullptr));
        require(recorded.at("words") == Json(instruction.words) &&
                    recorded.at("word_offset") == instruction.wordOffset &&
                    recorded.at("dxbc_opcode") == instruction.opcode &&
                    recorded.at("declaration") == instruction.declaration &&
                    recorded.at("source") == location,
                "Native debug instruction or source mapping mismatch");
        catalog.push_back({{"instruction", i},
                           {"token", i},
                           {"opcode", instruction.opcode},
                           {"word_offset", instruction.wordOffset},
                           {"checkpoint_allowed", !instruction.declaration},
                           {"source_location", location}});
        if (instruction.opcode == 105) {
            require(instruction.words.size() == 4, "Invalid native debug indexable temporary declaration");
            arrays.push_back({{"array", instruction.words[1]},
                              {"elements", instruction.words[2]},
                              {"components", instruction.words[3]}});
        }
    }
    registerMetadata_ = {{"shader_stage", "cs"}, {"indexable_temporaries", arrays}};
    for (uint64_t i = 0; i < trace_.selectedSize(); ++i) {
        checkCancellation(cancelled);
        const auto global = trace_.selectedBoundary(i);
        const auto step = trace_.step(global);
        const auto &beforeStack = step.at("return_instruction_stack");
        const auto instruction = step.at("instruction").get<size_t>();
        rows_.push_back({{"record", i},
                         {"hit", i},
                         {"invocation", lane_},
                         {"global_step", global},
                         {"instruction", instruction},
                         {"token", instruction},
                         {"opcode", catalog.at(instruction).at("opcode")},
                         {"call_depth", beforeStack.size()},
                         {"return_instruction_stack_before", beforeStack},
                         {"state_boundary", "before_instruction"}});
    }
    require(!rows_.empty(), "Native debug invocation has no execution steps");
    result_ = {{"format", "flora.native-execution-debug-model"},
               {"version", 1},
               {"action", "debug-thread"},
               {"shader_sha256", shaderHash},
               {"trace_sha256", digest},
               {"trace", true},
               {"execution_context", header},
               {"catalog", catalog},
               {"source_lines", source},
               {"source_variables", symbols},
               {"source_stack", stack},
               {"disassembly", systemDisassembly(shader)},
               {"state_boundary", "before_selected_instruction_or_terminal_group"}};
    if (symbols.value("format", "") == "SDBG assignments" && symbols.value("status", "") == "available") {
        sdbg_ = std::make_unique<SdbgTraceValues>(result_, rows_, [this, &cancelled](size_t i) {
            checkCancellation(cancelled);
            return registersAt(i);
        });
    }
    settings_ = std::make_unique<NativeDebugSettings>(
        NativeDebugSettings{SourceTrace(result_, rows_, [this](size_t i) { return valuesAt(i); }), {}, {}});
    settings_->source.setCheckpoint([this] { checkCancellation(operationCancelled_); });
    move(0, cancelled);
}
NativeExecutionDebugModel::~NativeExecutionDebugModel() = default;
void NativeExecutionDebugModel::move(size_t position, const CancelCheck &cancelled) {
    require(position <= size(), "Native debug position is outside the invocation");
    trace_.moveTo(trace_.selectedBoundary(position), cancelled);
    position_ = position;
}
Json NativeExecutionDebugModel::registersAt(size_t position) {
    require(position <= size(), "Native debug position is outside the invocation");
    trace_.moveTo(trace_.selectedBoundary(position), operationCancelled_);
    Json result = Json::array();
    for (const auto &[key, v] : trace_.registers()) {
        checkCancellation(operationCancelled_);
        if (key.first == lane_)
            result.push_back(registerRecord(key.second, v));
    }
    for (const auto &[key, v] : trace_.constants()) {
        checkCancellation(operationCancelled_);
        result.push_back(registerRecord({8, {key.first, key.second}}, v));
    }
    return result;
}
Json NativeExecutionDebugModel::registerValues() { return registersAt(position_); }
Json NativeExecutionDebugModel::registerRows(const std::string &interpretation) {
    require(interpretation == "float" || interpretation == "int" || interpretation == "uint" ||
                interpretation == "hex",
            "Unsupported native register interpretation");
    Json result = Json::array();
    for (const auto &row : registerValues()) {
        checkCancellation(operationCancelled_);
        std::string text;
        for (size_t i = 0; i < 4; ++i) {
            if (i)
                text += ", ";
            if (!row.at("written")[i].get<bool>()) {
                text += "unknown";
                continue;
            }
            const auto bits = row.at("bits")[i].get<uint32_t>();
            if (interpretation == "hex")
                text += "0x" + QString::number(bits, 16).rightJustified(8, QChar('0')).toStdString();
            else if (interpretation == "uint")
                text += std::to_string(bits);
            else if (interpretation == "int")
                text += std::to_string(std::bit_cast<int32_t>(bits));
            else
                text += source_detail::scalarText("float", bits);
        }
        result.push_back({row.at("name"), text, interpretation});
    }
    return result;
}
Json NativeExecutionDebugModel::valuesAt(size_t position) {
    require(position < size(), "Terminal native debug state has no source instruction scope");
    if (sdbg_)
        return sdbg_->at(position);
    const auto &row = rows_.at(position);
    const auto offset =
        result_.at("catalog").at(row.at("instruction").get<size_t>()).at("word_offset").get<uint64_t>() * 4;
    return resolveSourceVariables(result_.at("source_variables"), registersAt(position), registerMetadata_,
                                  row, offset);
}
Json NativeExecutionDebugModel::sourceValues() {
    return position_ == size() ? Json::array() : valuesAt(position_);
}
Json NativeExecutionDebugModel::location() const {
    return position_ == size() ? Json(nullptr) : settings_->source.location(position_);
}
Json NativeExecutionDebugModel::stack() {
    return position_ == size() ? Json{{"status", "terminal"}, {"frames", Json::array()}}
                               : settings_->source.stack(position_);
}
Json NativeExecutionDebugModel::instructionState() const {
    if (position_ == size())
        return {
            {"state_boundary", "terminal_group"}, {"global_step", trace_.size()}, {"instruction", nullptr}};
    auto result = rows_.at(position_);
    result["execution"] = trace_.step(result.at("global_step").get<uint64_t>());
    return result;
}
void NativeExecutionDebugModel::toggleInstruction(uint64_t instruction) {
    require(instruction < result_.at("catalog").size() &&
                result_.at("catalog")[instruction].at("checkpoint_allowed") == true,
            "Instruction breakpoint has no executable original mapping");
    if (settings_->instructions.contains(instruction))
        settings_->instructions.erase(instruction);
    else {
        require(settings_->instructions.size() < 4096, "At most 4096 instruction breakpoints are supported");
        settings_->instructions.insert(instruction);
    }
}
size_t NativeExecutionDebugModel::seek(size_t position, const CancelCheck &cancelled) {
    require(position < size(), "No instruction after terminal native debug state");
    for (size_t i = position + 1; i < size(); ++i) {
        checkCancellation(cancelled);
        if (settings_->instructions.contains(rows_[i].at("instruction").get<uint64_t>()) ||
            settings_->source.breakpointEntry(i))
            return i;
    }
    throw std::runtime_error("No breakpoint hit in the remaining native invocation");
}
void NativeExecutionDebugModel::addWatch(const std::string &expression) {
    DebugExpression parsed(expression);
    require(settings_->watches.size() < 64, "At most 64 watch expressions are supported");
    require(std::none_of(settings_->watches.begin(), settings_->watches.end(),
                         [&](const auto &item) { return item.text() == parsed.text(); }),
            "Duplicate watch expression");
    settings_->watches.push_back(std::move(parsed));
}
void NativeExecutionDebugModel::removeWatch(size_t index) {
    require(index < settings_->watches.size(), "Unknown watch expression");
    settings_->watches.erase(settings_->watches.begin() + index);
}
ExpressionValue NativeExecutionDebugModel::evaluate(size_t position, const std::string &expression) {
    require(position < size(), "Terminal native debug state has no source instruction scope");
    return settings_->source.environment(position).expression(DebugExpression(expression));
}
Json NativeExecutionDebugModel::watchResults() {
    Json result = Json::array();
    for (const auto &watch : settings_->watches) {
        checkCancellation(operationCancelled_);
        Json row{{"expression", watch.text()}};
        try {
            const auto value = evaluate(position_, watch.text());
            row.update({{"status", "available"}, {"type", value.kind}, {"value", value.text()}});
        } catch (const OperationCancelled &) {
            throw;
        } catch (const std::exception &e) {
            row.update({{"status", "unavailable"}, {"error", e.what()}});
        }
        result.push_back(std::move(row));
    }
    return result;
}
Json NativeExecutionDebugModel::configuration() const { return exportNativeDebugConfig(result_, *settings_); }
void NativeExecutionDebugModel::importConfiguration(const Json &configuration) {
    auto replacement =
        prepareNativeDebugConfig(result_, rows_, [this](size_t i) { return valuesAt(i); }, configuration);
    settings_ = std::make_unique<NativeDebugSettings>(std::move(replacement));
    settings_->source.setCheckpoint([this] { checkCancellation(operationCancelled_); });
}
Json NativeExecutionDebugModel::inspect(NativeDebugCommand command, size_t position,
                                        const Json &configuration, const std::string &interpretation,
                                        bool includeReport, const CancelCheck &cancelled) {
    const auto original = position_;
    const QScopedValueRollback guard(operationCancelled_, cancelled);
    try {
        checkCancellation(cancelled);
        require(command >= NativeDebugCommand::Position && command <= NativeDebugCommand::Continue,
                "Unknown native debug command");
        if (configuration != this->configuration())
            importConfiguration(configuration);
        size_t target = position;
        if (command == NativeDebugCommand::PreviousSource || command == NativeDebugCommand::NextSource)
            target = source().next(position, command == NativeDebugCommand::PreviousSource ? -1 : 1);
        else if (command == NativeDebugCommand::Over || command == NativeDebugCommand::Out)
            target = source().functionStep(position, command == NativeDebugCommand::Out);
        else if (command == NativeDebugCommand::Continue)
            target = seek(position, cancelled);
        move(target, cancelled);
        Json result{{"position", position_},
                    {"interpretation", interpretation},
                    {"state", instructionState()},
                    {"registers", registerValues()},
                    {"register_rows", registerRows(interpretation)},
                    {"source_values", sourceValues()},
                    {"location", location()},
                    {"source_stack", stack()},
                    {"watches", watchResults()}};
        if (includeReport)
            result["report"] = exportReport();
        checkCancellation(cancelled);
        return result;
    } catch (...) {
        // Do not perform an unbounded rewind after cancellation. All readers
        // explicitly restore this displayed boundary before returning values.
        position_ = original;
        throw;
    }
}
Json NativeExecutionDebugModel::exportReport() {
    return {{"format", "flora.native-execution-debug-report"},
            {"version", 1},
            {"model", result_},
            {"position", position_},
            {"state", instructionState()},
            {"registers", registerValues()},
            {"source_values", sourceValues()},
            {"source_stack", stack()},
            {"configuration", configuration()},
            {"watches", watchResults()}};
}
} // namespace flora
