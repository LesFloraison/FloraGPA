#pragma once
#include "DxbcExecutionTraceReader.h"
#include "NativeDebugConfig.h"
#include "SdbgVariables.h"

namespace flora {
// Instruction boundaries in a selected invocation, backed by complete group
// state. This model does not accept legacy debugger numeric types or enums.
class NativeExecutionDebugModel {
  public:
    using Json = nlohmann::json;
    NativeExecutionDebugModel(const QString &tracePath, const std::string &traceSha256, Bytes shader,
                              const dxbc_exec::TraceReadLimits & = {}, const CancelCheck & = {});
    ~NativeExecutionDebugModel();
    NativeExecutionDebugModel(const NativeExecutionDebugModel &) = delete;
    NativeExecutionDebugModel &operator=(const NativeExecutionDebugModel &) = delete;
    const Json &result() const { return result_; }
    const Json &rows() const { return rows_; }
    Json completion() const { return trace_.completion(); }
    const Json &files() const { return result_.at("source_lines").at("files"); }
    size_t size() const { return rows_.size(); }
    // size() denotes the terminal whole-group state. No source location is
    // invented for that boundary.
    size_t position() const { return position_; }
    void move(size_t position, const CancelCheck & = {});
    Json registerValues();
    Json registerRows(const std::string &interpretation = "float");
    Json sourceValues();
    // Queries another boundary without changing the displayed position.
    Json sourceValuesAt(size_t position) { return valuesAt(position); }
    Json location() const;
    Json stack();
    Json instructionState() const;
    SourceTrace &source() { return settings_->source; }
    const std::set<uint64_t> &instructionBreakpoints() const { return settings_->instructions; }
    void toggleInstruction(uint64_t instruction);
    size_t seek(size_t position, const CancelCheck & = {});
    void addWatch(const std::string &expression);
    void removeWatch(size_t index);
    ExpressionValue evaluate(size_t position, const std::string &expression);
    Json watchResults();
    Json configuration() const;
    void importConfiguration(const Json &);
    Json exportReport();

  private:
    dxbc_exec::ExecutionTraceReader trace_;
    Json result_, rows_ = Json::array(), registerMetadata_;
    std::unique_ptr<SdbgTraceValues> sdbg_;
    std::unique_ptr<NativeDebugSettings> settings_;
    uint32_t lane_ = 0;
    size_t position_ = 0;
    Json registersAt(size_t position);
    Json valuesAt(size_t position);
};
} // namespace flora
