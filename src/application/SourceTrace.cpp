#include "SourceTrace.h"
#include "SourceStack.h"
#include <QList>
#include <QString>

namespace flora {
SourceTrace::SourceTrace(Json result, Json rows, ValueLoader loader)
    : result_(std::move(result)), rows_(std::move(rows)), loader_(std::move(loader)) {
    const auto checkpoint = result_.value("checkpoint_instruction", Json(nullptr));
    if (!checkpoint.is_null() && checkpoint.contains("instruction") &&
        !checkpoint.at("instruction").is_null())
        checkpoint_ = checkpoint.at("instruction").get<uint64_t>();
    for (const auto &r : result_.at("catalog")) {
        if (r.contains("source_location") && !r.at("source_location").is_null() &&
            !r.at("source_location").empty() && r.at("checkpoint_allowed") == true &&
            !r.at("instruction").is_null())
            locations_[r.at("instruction").get<uint64_t>()] = r.at("source_location");
        if (!r.at("instruction").is_null())
            offsets_[r.at("instruction").get<uint64_t>()] = r.at("word_offset").get<uint64_t>() * 4;
    }
}
std::optional<uint64_t> SourceTrace::instruction(size_t i) const {
    const auto &row = rows_.at(i);
    if (!row.contains("instruction"))
        return checkpoint_;
    if (row.at("instruction").is_null())
        return {};
    return row.at("instruction").get<uint64_t>();
}
std::optional<uint64_t> SourceTrace::offset(size_t i) const {
    const auto token = instruction(i);
    if (!token || !offsets_.contains(*token))
        return {};
    return offsets_.at(*token);
}
SourceTrace::Json SourceTrace::location(size_t i) const {
    const auto token = instruction(i);
    return token && locations_.contains(*token) ? locations_.at(*token) : Json(nullptr);
}
bool SourceTrace::same(size_t a, size_t b) const {
    const auto &left = rows_.at(a), &right = rows_.at(b);
    return left.at("invocation") == right.at("invocation") &&
           left.value("hs_phase", Json(nullptr)) == right.value("hs_phase", Json(nullptr));
}
SourceTrace::Json SourceTrace::stack(size_t i) {
    const auto key = std::pair{offset(i), rows_.at(i).value("call_depth", 0u)};
    if (!stacks_.contains(key))
        stacks_[key] =
            key.first ? sourceStackAt(result_.value("source_stack", Json::object()), *key.first, key.second)
                      : Json{{"status", "unmapped"}, {"frames", Json::array()}};
    return stacks_.at(key);
}
std::optional<std::vector<std::string>> SourceTrace::path(size_t i) {
    const auto result = stack(i);
    if (result.at("status") != "available")
        return {};
    std::vector<std::string> frames;
    for (const auto &f : result.at("frames"))
        frames.push_back(f.at("id").get<std::string>());
    return frames;
}
bool SourceTrace::reentry(size_t i) const {
    rows_.at(i);
    if (!i || !same(i - 1, i))
        return true;
    const auto &before = rows_.at(i - 1), &after = rows_.at(i);
    return after.at("instruction") < before.at("instruction") ||
           after.value("call_depth", 0u) != before.value("call_depth", 0u);
}
size_t SourceTrace::next(size_t index, int direction) {
    if (direction != -1 && direction != 1)
        throw std::runtime_error("Invalid source step direction");
    const auto start = location(index);
    for (int64_t target = int64_t(index) + direction; target >= 0 && uint64_t(target) < rows_.size();
         target += direction) {
        const auto i = size_t(target);
        if (!same(index, i))
            break;
        const auto here = location(i);
        const auto a = path(index), b = path(i);
        const bool changed = a && b && a != b;
        if (!here.is_null() && (here != start || changed || reentry(direction == 1 ? i : i + 1)))
            return i;
    }
    throw std::runtime_error("No more mapped source locations in this invocation's visible trace");
}
size_t SourceTrace::functionStep(size_t index, bool out) {
    const auto current = path(index);
    if (!current || current->empty())
        throw std::runtime_error("No source function stack at the current location");
    if (out && current->size() == 1)
        throw std::runtime_error("Cannot step out of the source entry function");
    const auto start = location(index);
    for (size_t i = index + 1; i < rows_.size(); ++i) {
        if (!same(index, i))
            break;
        if (breakpointEntry(i))
            return i;
        const auto future = path(i);
        if (!future || future->empty())
            throw std::runtime_error("Uncertain subsequent source function ranges");
        const bool equal = future == current,
                   descendant = future->size() > current->size() &&
                                std::equal(current->begin(), current->end(), future->begin());
        if (descendant)
            continue;
        const auto loc = location(i);
        if (loc.is_null())
            continue;
        if (out) {
            if (std::find(future->begin(), future->end(), current->back()) == future->end())
                return i;
        } else if (!equal || loc != start || reentry(i))
            return i;
    }
    throw std::runtime_error("No source function step target in this invocation's visible trace");
}
void SourceTrace::validateLine(int64_t file, int64_t line) const {
    for (const auto &[id, r] : locations_)
        if (r.at("file") == file && r.at("line_start").get<int64_t>() <= line &&
            line <= r.at("line_end").get<int64_t>())
            return;
    throw std::runtime_error("Source line has no executable original instruction mapping");
}
void SourceTrace::toggle(int64_t file, int64_t line) {
    validateLine(file, line);
    const Point point{file, line};
    if (breakpoints_.contains(point)) {
        breakpoints_.erase(point);
        conditions_.erase(point);
        counts_.erase(point);
    } else {
        if (breakpoints_.size() >= 256)
            throw std::runtime_error("At most 256 source breakpoints are supported");
        breakpoints_.insert(point);
    }
}
void SourceTrace::setRule(int64_t file, int64_t line, const std::string &text, const std::string &mode,
                          int64_t count) {
    validateLine(file, line);
    std::optional<DebugExpression> expression;
    bool content = false;
    for (auto cp : QString::fromStdString(text).toUcs4())
        if (!QChar::isSpace(cp) && !(cp >= 0x1c && cp <= 0x1f)) {
            content = true;
            break;
        }
    if (content)
        expression.emplace(text);
    if ((mode != "always" && mode != "equal" && mode != "at_least" && mode != "multiple") || count < 1 ||
        count > UINT32_MAX)
        throw std::runtime_error("Invalid breakpoint hit count rule");
    const Point point{file, line};
    if (!breakpoints_.contains(point)) {
        if (breakpoints_.size() >= 256)
            throw std::runtime_error("At most 256 source breakpoints are supported");
        breakpoints_.insert(point);
    }
    if (expression)
        conditions_.insert_or_assign(point, *expression);
    else
        conditions_.erase(point);
    if (mode == "always")
        counts_.erase(point);
    else
        counts_[point] = {{"mode", mode}, {"count", count}};
}
SourceTrace::Json SourceTrace::exportBreakpoints() const {
    Json result = Json::array();
    for (const auto &p : breakpoints_) {
        Json row{{"file", p.first}, {"line", p.second}};
        if (conditions_.contains(p))
            row["condition"] = conditions_.at(p).text();
        if (counts_.contains(p))
            row["hit_count"] = counts_.at(p);
        result.push_back(std::move(row));
    }
    return result;
}
void SourceTrace::copyRules(const SourceTrace &source) {
    for (const auto &r : source.exportBreakpoints()) {
        const auto rule = r.value("hit_count", Json::object());
        setRule(r.at("file"), r.at("line"), r.value("condition", ""), rule.value("mode", "always"),
                rule.value("count", int64_t(1)));
    }
}
DebugEnvironment SourceTrace::environment(size_t i, const std::optional<std::string> &frame) {
    const auto symbols = result_.value("source_variables", Json::object());
    const auto &row = rows_.at(i);
    if (symbols.value("format", "") == "SDBG assignments") {
        if (!result_.value("trace", false))
            throw ExpressionError("SDBG watches require a full invocation trace");
        if (frame)
            throw ExpressionError("SDBG ancestor source frames are not reconstructed");
        if (symbols.value("shader_sha256", Json(nullptr)) != result_.value("shader_sha256", Json(nullptr)))
            throw ExpressionError("SDBG variable shader identity mismatch");
        if (!loader_)
            throw ExpressionError("No native variable reader for this result");
        if (!values_ || values_->first != i)
            values_ = std::pair{i, loader_(i)};
        const auto at = offset(i);
        if (!at || row.value("call_depth", 0u))
            throw ExpressionError("Current SDBG scope is unverified");
        return DebugEnvironment::sdbg(symbols, values_->second, *at);
    }
    const auto active = path(i);
    if (!active || active->empty())
        throw ExpressionError("No verified source scope for this record");
    const auto selected = frame ? *frame : active->back();
    if (std::find(active->begin(), active->end(), selected) == active->end())
        throw ExpressionError("Selected source frame is not active");
    if (!loader_)
        throw ExpressionError("No native variable reader for this result");
    if (!values_ || values_->first != i)
        values_ = std::pair{i, loader_(i)};
    return DebugEnvironment::native(result_.at("source_variables"), values_->second, *offset(i), selected);
}
bool SourceTrace::matches(size_t i, Point p) const {
    const auto loc = location(i);
    return !loc.is_null() && loc.at("file") == p.first && loc.at("line_start").get<int64_t>() <= p.second &&
           p.second <= loc.at("line_end").get<int64_t>();
}
size_t SourceTrace::seek(size_t index) {
    if (breakpoints_.empty())
        throw std::runtime_error("Set a source breakpoint first");
    for (size_t i = index + 1; i < rows_.size(); ++i) {
        if (!same(index, i))
            break;
        if (breakpointEntry(i))
            return i;
    }
    throw std::runtime_error("No source breakpoint hit in this invocation's visible trace");
}
bool SourceTrace::lineEntry(size_t i, Point point) {
    if (!matches(i, point))
        return false;
    return reentry(i) || !matches(i - 1, point) || path(i) != path(i - 1);
}
size_t SourceTrace::encounterCount(size_t index, Point point) {
    if (!result_.value("trace", false))
        throw ExpressionError("Hit counts require a full invocation trace");
    const auto invocation = rows_.at(index).at("invocation").get<uint64_t>();
    const auto key = std::tuple{invocation, point};
    if (!entries_.contains(key)) {
        std::vector<size_t> indices;
        for (size_t i = 0; i < rows_.size(); ++i)
            if (rows_.at(i).at("invocation") == invocation)
                indices.push_back(i);
        if (indices.empty() || rows_.at(indices.front()).at("hit") != 0)
            throw ExpressionError("The visible preview lacks this invocation's beginning");
        auto &entries = entries_[key];
        for (const auto i : indices)
            if (lineEntry(i, point))
                entries.push_back(i);
    }
    const auto &entries = entries_.at(key);
    return size_t(std::upper_bound(entries.begin(), entries.end(), index) - entries.begin());
}
bool SourceTrace::breakpointEntry(size_t index) {
    std::vector<Point> points;
    for (const auto &point : breakpoints_) {
        if (!lineEntry(index, point))
            continue;
        if (counts_.contains(point)) {
            const auto &rule = counts_.at(point);
            const auto n = encounterCount(index, point), count = rule.at("count").get<uint64_t>();
            const auto mode = rule.at("mode").get<std::string>();
            if (!(mode == "equal" ? n == count : mode == "at_least" ? n >= count : n % count == 0))
                continue;
        }
        points.push_back(point);
    }
    if (std::any_of(points.begin(), points.end(), [&](const auto &p) { return !conditions_.contains(p); }))
        return true;
    if (!points.empty()) {
        const auto env = environment(index);
        for (const auto &p : points)
            try {
                if (env.expression(conditions_.at(p)).truth())
                    return true;
            } catch (const ExpressionError &e) {
                throw ExpressionError("Cannot evaluate breakpoint " + std::to_string(p.first) + ":" +
                                      std::to_string(p.second) + " at record " +
                                      rows_.at(index).at("record").dump() + ": " + e.what());
            }
    }
    return false;
}
} // namespace flora
