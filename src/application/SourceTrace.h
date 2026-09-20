#pragma once
#include "DebugExpression.h"
#include <functional>

namespace flora {
class SourceTrace {
  public:
    using Json = nlohmann::json;
    using Point = std::pair<int64_t, int64_t>;
    using ValueLoader = std::function<Json(size_t)>;
    SourceTrace(Json result, Json rows, ValueLoader loader = {});
    Json location(size_t index) const;
    bool same(size_t a, size_t b) const;
    Json stack(size_t index);
    std::optional<std::vector<std::string>> path(size_t index);
    bool reentry(size_t index) const;
    size_t next(size_t index, int direction);
    size_t functionStep(size_t index, bool out = false);
    void toggle(int64_t file, int64_t line);
    void validateLine(int64_t file, int64_t line) const;
    void setRule(int64_t file, int64_t line, const std::string &text = "", const std::string &mode = "always",
                 int64_t count = 1);
    Json exportBreakpoints() const;
    void copyRules(const SourceTrace &source);
    DebugEnvironment environment(size_t index, const std::optional<std::string> &frame = {});
    bool matches(size_t index, Point point) const;
    size_t seek(size_t index);
    bool lineEntry(size_t index, Point point);
    size_t encounterCount(size_t index, Point point);
    bool breakpointEntry(size_t index);

  private:
    Json result_, rows_;
    ValueLoader loader_;
    std::optional<uint64_t> checkpoint_;
    std::map<uint64_t, Json> locations_;
    std::map<uint64_t, uint64_t> offsets_;
    std::set<Point> breakpoints_;
    std::map<Point, DebugExpression> conditions_;
    std::map<Point, Json> counts_;
    std::map<std::tuple<uint64_t, Point>, std::vector<size_t>> entries_;
    std::map<std::pair<std::optional<uint64_t>, uint32_t>, Json> stacks_;
    std::optional<std::pair<size_t, Json>> values_;
    std::optional<uint64_t> instruction(size_t index) const;
    std::optional<uint64_t> offset(size_t index) const;
};
} // namespace flora
