#pragma once
#include "DebugExpression.h"
#include <map>
#include <memory>
#include <set>

namespace flora {
// Recorded PS/VS/CS invocation semantics. These are independent of the native
// GS/HS/DS checkpoint format and never infer absent source/callstack metadata.
class ReplayDebugModel {
  public:
    using Json = nlohmann::json;
    using Point = std::pair<int64_t, int64_t>;
    explicit ReplayDebugModel(Json result);
    const Json &result() const { return *result_; }
    const Json &files() const { return files_; }
    const Json &variables() const { return state_.variables; }
    int64_t position() const { return state_.position; }
    size_t size() const { return result_->at("steps").size(); }
    const Json &move(size_t position);
    Json sourceValues() const;
    Json registerRows(const std::string &mode = "float") const;
    Json location(int64_t position) const;
    Json stack(int64_t position) const;
    Json instructionInfo(uint64_t instruction) const;
    size_t nextSource(int64_t position, int direction) const;
    std::pair<size_t, std::string> functionStep(int64_t position, bool out);
    void validateLine(Point point) const;
    void toggle(Point point);
    void setCondition(Point point, const std::string &text);
    void setHitCount(Point point, const std::string &mode, int64_t count);
    bool hit(int64_t position, Point point) const;
    bool lineEntry(int64_t position, Point point) const;
    size_t encounterCount(int64_t position, Point point);
    bool breakpointEntry(int64_t position);
    size_t seek(int64_t position);
    Json breakpoints() const;
    void addWatch(const std::string &text);
    void removeWatch(size_t index);
    Json watchResults() const;
    ExpressionValue evaluate(size_t position, const std::string &expression);
    Json configuration() const;
    void importConfiguration(const Json &config);
    Json exportReport() const;
    static Json identity(const Json &result);

  private:
    struct State {
        int64_t position = -1;
        Json variables = Json::object();
    };
    using Index = std::map<std::string, const Json *>;
    std::shared_ptr<const Json> result_;
    Json files_;
    State state_, conditionState_;
    std::map<uint64_t, Json> instructions_, locations_;
    std::map<int, Index> static_;
    std::set<Point> points_;
    std::map<Point, DebugExpression> conditions_;
    std::map<Point, Json> counts_;
    std::map<Point, std::vector<size_t>> entries_;
    std::vector<DebugExpression> watches_;
    void move(State &state, size_t position) const;
    Json resolve(const State &state) const;
    bool valid(int64_t position) const;
};
} // namespace flora
