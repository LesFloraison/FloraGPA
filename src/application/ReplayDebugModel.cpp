#include "ReplayDebugModel.h"
#include "NativeDebugConfig.h"
#include "core/Frame.h"
#include <QRegularExpression>
#include <QString>
#include <algorithm>
#include <cmath>
#include <functional>

namespace flora {
namespace {
using Json = nlohmann::json;
using Index = std::map<std::string, const Json *>;
const std::map<int, std::pair<const char *, const char *>> types{
    {0, {"float", "f32v"}},  {1, {"double", "f64v"}}, {2, {"half", "f16v"}},   {3, {"int", "s32v"}},
    {4, {"uint", "u32v"}},   {5, {"int16", "s16v"}},  {6, {"uint16", "u16v"}}, {7, {"int64", "s64v"}},
    {8, {"uint64", "u64v"}}, {9, {"int8", "s8v"}},    {10, {"uint8", "u8v"}},  {11, {"bool", "u32v"}},
    {12, {"enum", "u32v"}}};
const std::map<int, const char *> opaque{{14, "GPU pointer"},
                                         {15, "constant block"},
                                         {16, "read-only resource"},
                                         {17, "read-write resource"},
                                         {18, "sampler"}};
int64_t integer(const Json &v) {
    if (!v.is_number_integer() || (v.is_number_unsigned() && v.get<uint64_t>() > INT64_MAX))
        throw std::runtime_error("Expected a supported integer");
    return v.get<int64_t>();
}
uint64_t positive(const Json &v) {
    const auto n = integer(v);
    if (n < 0)
        throw std::runtime_error("Expected a non-negative integer");
    return uint64_t(n);
}
bool fields(const Json &v, const std::set<std::string> &keys) {
    if (!v.is_object() || v.size() != keys.size())
        return false;
    return std::all_of(keys.begin(), keys.end(), [&](const auto &k) { return v.contains(k); });
}
void indexVariable(Index &result, const Json &v, const std::string &prefix = {}, unsigned depth = 0) {
    if (depth > 64)
        throw std::runtime_error("Debug variable nesting exceeds 64");
    if (!v.is_object())
        return;
    const auto name = v.value("name", std::string{});
    const auto path = prefix + (prefix.empty() || name.starts_with('[') ? "" : ".") + name;
    if (!result.emplace(path, &v).second)
        result[path] = nullptr;
    if (const auto children = v.find("members"); children != v.end())
        for (const auto &child : *children)
            indexVariable(result, child, path, depth + 1);
}
const Json *lookup(const std::map<int, Index> &indices, const Json &ref) {
    const auto type = ref.value("type", Json(nullptr)), name = ref.value("name", Json(nullptr));
    if (!type.is_number_integer() || !name.is_string())
        return nullptr;
    const auto cat = indices.find(type.get<int>());
    if (cat == indices.end())
        return nullptr;
    const auto item = cat->second.find(name.get<std::string>());
    return item == cat->second.end() ? nullptr : item->second;
}
bool members(const Json *v) { return v && !v->value("members", Json::array()).empty(); }
size_t lines(const std::string &text) {
    if (text.empty())
        return 0;
    const auto split = QString::fromStdString(text).split(QRegularExpression(
        "\\r\\n|[\\n\\r\\x{000b}\\x{000c}\\x{001c}-\\x{001e}\\x{0085}\\x{2028}\\x{2029}]"));
    return size_t(split.size() - (split.back().isEmpty() ? 1 : 0));
}
std::string display(const Json &value, const std::string &mode) {
    if (value.is_string())
        return value.get<std::string>();
    if (mode == "hex" && value.is_number_integer())
        return "0x" + QString::number(value.get<uint64_t>(), 16).toStdString();
    if (value.is_number_float()) {
        const auto number = value.get<double>();
        if (number == 0 && std::signbit(number))
            return "-0";
        return QString::number(number, 'g', 7).toStdString();
    }
    return value.dump();
}
void registerRows(Json &out, const Json &v, const std::string &mode, const std::string &prefix,
                  unsigned depth) {
    if (depth > 64)
        throw std::runtime_error("Debug variable nesting exceeds 64");
    const auto name = prefix + v.value("name", std::string{});
    const auto children = v.value("members", Json::array());
    if (!children.empty()) {
        for (const auto &child : children)
            registerRows(out, child, mode,
                         name + (child.value("name", std::string{}).starts_with('[') ? "" : "."), depth + 1);
        return;
    }
    const auto count =
        (std::max)(int64_t(1), integer(v.value("rows", Json(1))) * integer(v.value("columns", Json(1))));
    const auto key = mode == "float" ? "f32v" : mode == "int" ? "s32v" : "u32v";
    const auto values = v.value("value", Json::object()).value(key, Json::array());
    std::string rendered;
    for (size_t i = 0; i < (std::min)(values.size(), size_t(count)); ++i) {
        if (i)
            rendered += ", ";
        rendered += display(values[i], mode);
    }
    out.push_back({name, rendered,
                   v.value("type", Json("")).is_string() ? v.value("type", Json("")).get<std::string>()
                                                         : v.at("type").dump()});
}
std::string digest(const std::string &s) {
    return sha256(Bytes(reinterpret_cast<const uint8_t *>(s.data()), s.size()));
}
} // namespace

ReplayDebugModel::ReplayDebugModel(Json result) : result_(std::make_shared<const Json>(std::move(result))) {
    if (!result_->at("steps").is_array() || !result_->at("trace").is_object())
        throw std::runtime_error("Invalid recorded shader trace");
    files_ = result_->value("source_debug", Json::object()).value("files", Json::array());
    const auto &trace = result_->at("trace");
    for (const auto &v : trace.value("inputs", Json::array()))
        state_.variables[v.at("name").get<std::string>()] = v;
    conditionState_ = state_;
    for (const auto &[kind, field] : std::map<int, const char *>{{1, "inputs"},
                                                                 {2, "constantBlocks"},
                                                                 {3, "samplers"},
                                                                 {4, "readOnlyResources"},
                                                                 {5, "readWriteResources"}})
        if (trace.contains(field))
            for (const auto &v : trace.at(field))
                indexVariable(static_[kind], v);
    for (const auto &info : trace.value("instInfo", Json::array())) {
        const auto key = positive(info.at("instruction"));
        if (!instructions_.emplace(key, info).second)
            throw std::runtime_error("Duplicate debug instruction metadata");
        const auto line = info.value("lineInfo", Json::object());
        const auto f = line.value("fileIndex", Json(-1)), a = line.value("lineStart", Json(0)),
                   b = line.value("lineEnd", Json(0));
        if (!f.is_number_integer() || !a.is_number_integer() || !b.is_number_integer())
            continue;
        const auto file = integer(f), first = integer(a), last = integer(b);
        if (file < 0 || uint64_t(file) >= files_.size() || first < 1 || first > last ||
            uint64_t(last) > lines(files_[size_t(file)].value("contents", std::string{})))
            continue;
        locations_[key] = {file, first, last, line.value("colStart", Json(0)), line.value("colEnd", Json(0))};
    }
}
void ReplayDebugModel::move(State &state, size_t position) const {
    const auto &steps = result_->at("steps");
    if (position >= steps.size())
        throw std::runtime_error("Debug step is outside the recorded trace");
    while (state.position < int64_t(position)) {
        for (const auto &change : steps.at(size_t(++state.position)).at("changes")) {
            const auto &before = change.at("before"), &after = change.at("after");
            const auto old = before.at("name").get<std::string>(), next = after.at("name").get<std::string>();
            if (!old.empty() && old != next)
                state.variables.erase(old);
            if (!next.empty())
                state.variables[next] = after;
        }
    }
    while (state.position > int64_t(position)) {
        const auto &changes = steps.at(size_t(state.position)).at("changes");
        for (auto it = changes.rbegin(); it != changes.rend(); ++it) {
            const auto &before = it->at("before"), &after = it->at("after");
            const auto old = before.at("name").get<std::string>(), next = after.at("name").get<std::string>();
            if (!next.empty())
                state.variables.erase(next);
            if (!old.empty())
                state.variables[old] = before;
        }
        --state.position;
    }
}
const Json &ReplayDebugModel::move(size_t position) {
    move(state_, position);
    return result_->at("steps").at(position);
}
Json ReplayDebugModel::instructionInfo(uint64_t instruction) const {
    auto it = instructions_.upper_bound(instruction);
    return it == instructions_.begin() ? Json(nullptr) : std::prev(it)->second;
}
Json ReplayDebugModel::resolve(const State &state) const {
    Json records = Json::array();
    if (state.position < 0)
        return records;
    auto indices = static_;
    for (const auto &v : state.variables)
        indexVariable(indices[6], v);
    std::function<void(const Json &, const std::string &, unsigned)> add;
    add = [&](const Json &mapping, const std::string &scope, unsigned depth) {
        if (depth > 64 || records.size() > 1000000)
            throw std::runtime_error("Source variable expansion exceeds bounds");
        const auto declared = mapping.value("type", 255);
        const auto typeName = types.contains(declared)    ? types.at(declared).first
                              : opaque.contains(declared) ? opaque.at(declared)
                              : declared == 13            ? "struct"
                                                          : "unknown";
        Json record{{"name", mapping.value("name", std::string{})},
                    {"scope", scope},
                    {"type", typeName},
                    {"rows", mapping.value("rows", 0)},
                    {"columns", mapping.value("columns", 0)},
                    {"offset", mapping.value("offset", Json(0))},
                    {"status", "available"},
                    {"values", Json::array()},
                    {"references", mapping.value("variables", Json::array())},
                    {"issues", Json::array()}};
        const auto &refs = record["references"];
        if (mapping.value("undefinedValue", false)) {
            record["status"] = "undefined";
            records.push_back(record);
            return;
        }
        if (refs.empty()) {
            record["status"] = "unavailable";
            record["issues"].push_back("No component mapping");
            records.push_back(record);
            return;
        }
        const auto *container = lookup(indices, refs[0]);
        if (members(container) && std::all_of(refs.begin(), refs.end(), [&](const auto &r) {
                return lookup(indices, r) == container;
            })) {
            std::function<void(const Json &, const std::string &, unsigned)> storage;
            storage = [&](const Json &v, const std::string &path, unsigned level) {
                if (level > 64)
                    throw std::runtime_error("Debug variable nesting exceeds 64");
                if (members(&v)) {
                    for (const auto &member : v.at("members")) {
                        const auto suffix = member.at("name").get<std::string>();
                        storage(member, path + (suffix.starts_with('[') ? "" : ".") + suffix, level + 1);
                    }
                } else {
                    auto kind = v.value("type", 255);
                    if (kind == 255)
                        kind = declared;
                    const auto count = uint64_t((std::max)(1, v.value("rows", 0))) *
                                       uint64_t((std::max)(1, v.value("columns", 0)));
                    if (count > 1000000)
                        throw std::runtime_error("Source variable shape exceeds bounds");
                    Json child{{"name", record["name"].get<std::string>() + " → " + path},
                               {"type", kind},
                               {"rows", v.value("rows", 0)},
                               {"columns", v.value("columns", 0)},
                               {"variables", Json::array()}};
                    for (uint64_t i = 0; i < count; ++i)
                        child["variables"].push_back(
                            {{"type", refs[0].at("type")}, {"name", path}, {"component", i}});
                    const auto before = records.size();
                    add(child, scope, depth + 1);
                    for (size_t i = before; i < records.size(); ++i) {
                        records[i]["source_container"] = record["name"];
                        if (records[i]["status"] == "available")
                            records[i]["status"] = "backing_storage";
                    }
                }
            };
            storage(*container, refs[0].at("name"), 0);
            return;
        }
        for (const auto &ref : refs) {
            const auto *v = lookup(indices, ref);
            const auto kind = declared != 255 ? declared : v ? v->value("type", 255) : 255;
            auto issue = [&](const std::string &s) {
                record["values"].push_back(nullptr);
                record["issues"].push_back(s);
            };
            if (!v) {
                issue("Missing or ambiguous variable: " + ref.value("name", std::string("None")));
                continue;
            }
            if (members(v)) {
                issue("Mixed container references cannot be flattened");
                continue;
            }
            const auto refType = ref.value("type", 0);
            if (opaque.contains(kind) || refType == 3 || refType == 4 || refType == 5) {
                record["status"] = "reference";
                continue;
            }
            const auto component = ref.value("component", Json(nullptr));
            const auto values =
                types.contains(kind)
                    ? v->value("value", Json::object()).value(types.at(kind).second, Json::array())
                    : Json::array();
            if (!types.contains(kind) || !component.is_number_integer() || integer(component) < 0 ||
                !values.is_array() || uint64_t(integer(component)) >= values.size()) {
                issue("Unavailable type or component: " + ref.dump());
                continue;
            }
            const auto &value = values[size_t(integer(component))];
            record["values"].push_back(kind == 11 ? Json(value.is_boolean() ? value.get<bool>() : value != 0)
                                                  : value);
        }
        if (!record["issues"].empty()) {
            record["status"] = std::any_of(record["values"].begin(), record["values"].end(),
                                           [](const auto &v) { return !v.is_null(); })
                                   ? "partial"
                                   : "unavailable";
        } else if (record["status"] == "available" &&
                   refs.size() < size_t((std::max)(1, record["columns"].get<int>()))) {
            record["status"] = "partial";
            record["issues"].push_back(
                "Mapping provides fewer components than declared columns; omitted components remain unknown");
        } else if (record["status"] == "available" &&
                   refs.size() > uint64_t((std::max)(1, record["rows"].get<int>())) *
                                     uint64_t((std::max)(1, record["columns"].get<int>()))) {
            record["status"] = "mapping_mismatch";
            record["issues"].push_back("More mapped components than the declared shape; showing all supplied "
                                       "components without inventing array layout");
        }
        records.push_back(std::move(record));
    };
    for (const auto &mapping : result_->at("trace").value("sourceVars", Json::array()))
        add(mapping, "global", 0);
    const auto info =
        instructionInfo(positive(result_->at("steps").at(size_t(state.position)).at("nextInstruction")));
    if (!info.is_null())
        for (const auto &mapping : info.value("sourceVars", Json::array()))
            add(mapping, "local", 0);
    return records;
}
Json ReplayDebugModel::sourceValues() const { return resolve(state_); }
Json ReplayDebugModel::registerRows(const std::string &mode) const {
    if (mode != "float" && mode != "int" && mode != "uint" && mode != "hex")
        throw std::runtime_error("Unknown register interpretation");
    Json rows = Json::array();
    for (const auto &v : state_.variables)
        ::flora::registerRows(rows, v, mode, "", 0);
    for (const auto &v : result_->at("trace").value("constantBlocks", Json::array()))
        ::flora::registerRows(rows, v, mode, "", 0);
    return rows;
}
bool ReplayDebugModel::valid(int64_t position) const { return position >= 0 && uint64_t(position) < size(); }
Json ReplayDebugModel::location(int64_t position) const {
    if (!valid(position))
        return nullptr;
    const auto info = instructionInfo(positive(result_->at("steps")[size_t(position)].at("nextInstruction")));
    if (info.is_null())
        return nullptr;
    const auto found = locations_.find(positive(info.at("instruction")));
    return found == locations_.end() ? Json(nullptr) : found->second;
}
Json ReplayDebugModel::stack(int64_t position) const {
    if (!valid(position))
        return Json::array();
    const auto value = result_->at("steps")[size_t(position)].value("callstack", Json::array());
    if (!value.is_array() || !std::all_of(value.begin(), value.end(), [](const auto &s) {
            return s.is_string() && !s.template get<std::string>().empty();
        }))
        return Json::array();
    return value;
}
size_t ReplayDebugModel::nextSource(int64_t position, int direction) const {
    if (direction != -1 && direction != 1)
        throw std::runtime_error("Invalid source step direction");
    const auto current = location(position), frames = stack(position);
    for (auto i = position + direction; i >= 0 && uint64_t(i) < size(); i += direction) {
        const auto next = location(i);
        if (!next.is_null() && (next != current || stack(i) != frames))
            return size_t(i);
    }
    throw std::runtime_error("No further mapped source location in this direction");
}
void ReplayDebugModel::validateLine(Point point) const {
    if (std::none_of(locations_.begin(), locations_.end(), [&](const auto &v) {
            return v.second[0] == point.first && v.second[1] <= point.second && point.second <= v.second[2];
        }))
        throw std::runtime_error("Source line has no executable instruction mapping");
}
void ReplayDebugModel::toggle(Point point) {
    validateLine(point);
    if (points_.erase(point)) {
        conditions_.erase(point);
        counts_.erase(point);
    } else {
        if (points_.size() >= 256)
            throw std::runtime_error("At most 256 source breakpoints are supported");
        points_.insert(point);
    }
}
void ReplayDebugModel::setCondition(Point point, const std::string &text) {
    std::optional<DebugExpression> expression;
    if (!QString::fromStdString(text).trimmed().isEmpty())
        expression.emplace(text);
    if (!points_.contains(point))
        toggle(point);
    if (expression)
        conditions_.insert_or_assign(point, std::move(*expression));
    else
        conditions_.erase(point);
}
void ReplayDebugModel::setHitCount(Point point, const std::string &mode, int64_t count) {
    if (mode != "always" && mode != "equal" && mode != "at_least" && mode != "multiple")
        throw std::runtime_error("Invalid hit count mode");
    if (count < 1 || uint64_t(count) > UINT32_MAX)
        throw std::runtime_error("Hit count must be 1–4294967295");
    if (!points_.contains(point))
        toggle(point);
    if (mode == "always")
        counts_.erase(point);
    else
        counts_[point] = {{"mode", mode}, {"count", count}};
}
bool ReplayDebugModel::hit(int64_t position, Point point) const {
    const auto l = location(position);
    return !l.is_null() && l[0] == point.first && l[1] <= point.second && point.second <= l[2];
}
bool ReplayDebugModel::lineEntry(int64_t position, Point point) const {
    if (!valid(position) || !hit(position, point))
        return false;
    const auto &steps = result_->at("steps");
    return position == 0 || !hit(position - 1, point) || stack(position) != stack(position - 1) ||
           steps[size_t(position)].at("nextInstruction") < steps[size_t(position - 1)].at("nextInstruction");
}
size_t ReplayDebugModel::encounterCount(int64_t position, Point point) {
    if (!entries_.contains(point)) {
        auto &entries = entries_[point];
        for (size_t i = 0; i < size(); ++i)
            if (lineEntry(int64_t(i), point))
                entries.push_back(i);
    }
    const auto &entries = entries_.at(point);
    return position < 0
               ? 0
               : size_t(std::upper_bound(entries.begin(), entries.end(), size_t(position)) - entries.begin());
}
bool ReplayDebugModel::breakpointEntry(int64_t position) {
    if (!valid(position))
        return false;
    std::vector<Point> matches;
    for (const auto &p : points_) {
        if (!lineEntry(position, p))
            continue;
        if (counts_.contains(p)) {
            const auto &rule = counts_.at(p);
            const auto ordinal = encounterCount(position, p), count = rule.at("count").get<size_t>();
            const auto mode = rule.at("mode").get<std::string>();
            if (!(mode == "equal"      ? ordinal == count
                  : mode == "at_least" ? ordinal >= count
                                       : ordinal % count == 0))
                continue;
        }
        if (!conditions_.contains(p))
            return true;
        matches.push_back(p);
    }
    if (matches.empty())
        return false;
    move(conditionState_, size_t(position));
    const DebugEnvironment environment(resolve(conditionState_));
    for (const auto &p : matches) {
        try {
            if (environment.expression(conditions_.at(p)).truth())
                return true;
        } catch (const ExpressionError &e) {
            throw ExpressionError("Breakpoint " + std::to_string(p.first) + ":" + std::to_string(p.second) +
                                  " at step " + std::to_string(position) + ": " + e.what());
        }
    }
    return false;
}
size_t ReplayDebugModel::seek(int64_t position) {
    if (points_.empty())
        throw std::runtime_error("Set a source breakpoint first");
    for (auto i = position + 1; i >= 0 && uint64_t(i) < size(); ++i)
        if (breakpointEntry(i))
            return size_t(i);
    throw std::runtime_error("No later breakpoint in this invocation");
}
std::pair<size_t, std::string> ReplayDebugModel::functionStep(int64_t position, bool out) {
    const auto current = stack(position), loc = location(position);
    if (current.empty())
        throw std::runtime_error("The current step has no recorded callstack");
    if (out && current.size() == 1)
        throw std::runtime_error("The outermost function has no caller");
    bool child = false;
    for (auto i = position + 1; i >= 0 && uint64_t(i) < size(); ++i) {
        if (breakpointEntry(i))
            return {size_t(i), "breakpoint"};
        const auto next = stack(i);
        if (next.empty())
            throw std::runtime_error("A later step has no recorded callstack");
        if (next.size() > current.size() && std::equal(current.begin(), current.end(), next.begin())) {
            child = true;
            continue;
        }
        const bool ancestor =
            next.size() < current.size() && std::equal(next.begin(), next.end(), current.begin());
        if (next != current && !ancestor)
            throw std::runtime_error("Recorded callstack changes discontinuously");
        const auto mapped = location(i);
        if (mapped.is_null())
            continue;
        if (ancestor)
            return {size_t(i), "return"};
        const auto &steps = result_->at("steps");
        const bool backwards =
            steps[size_t(i)].at("nextInstruction") < steps[size_t(i - 1)].at("nextInstruction");
        if (!out && (mapped != loc || child || backwards))
            return {size_t(i), "source"};
    }
    throw std::runtime_error("No later function step in this invocation");
}
Json ReplayDebugModel::breakpoints() const {
    Json result = Json::array();
    for (const auto &point : points_) {
        Json p{{"file", point.first}, {"line", point.second}};
        if (conditions_.contains(point))
            p["condition"] = conditions_.at(point).text();
        if (counts_.contains(point))
            p["hit_count"] = counts_.at(point);
        result.push_back(std::move(p));
    }
    return result;
}
void ReplayDebugModel::addWatch(const std::string &text) {
    DebugExpression expression(text);
    for (const auto &v : watches_)
        if (v.text() == expression.text())
            return;
    if (watches_.size() >= 64)
        throw std::runtime_error("At most 64 watch expressions are supported");
    watches_.push_back(std::move(expression));
}
void ReplayDebugModel::removeWatch(size_t index) {
    if (index >= watches_.size())
        throw std::runtime_error("Select a watch expression");
    watches_.erase(watches_.begin() + index);
}
Json ReplayDebugModel::watchResults() const {
    Json rows = Json::array();
    const DebugEnvironment environment(sourceValues());
    for (const auto &e : watches_) {
        Json r{{"expression", e.text()}};
        try {
            const auto v = environment.expression(e);
            Json values = Json::array();
            for (const auto &x : v.values)
                std::visit([&](const auto &n) { values.push_back(n); }, x);
            r.update({{"status", "available"}, {"type", v.kind}, {"values", values}});
        } catch (const ExpressionError &error) {
            r.update({{"status", "unavailable"}, {"error", error.what()}});
        }
        rows.push_back(std::move(r));
    }
    return rows;
}
ExpressionValue ReplayDebugModel::evaluate(size_t position, const std::string &expression) {
    move(conditionState_, position);
    return DebugEnvironment(resolve(conditionState_)).expression(DebugExpression(expression));
}
Json ReplayDebugModel::identity(const Json &result) {
    const auto action = result.value("action", Json(nullptr)),
               assembly = result.value("disassembly", Json(nullptr));
    if ((action != "debug-pixel" && action != "debug-vertex" && action != "debug-thread") ||
        !assembly.is_string() || assembly.get<std::string>().empty())
        throw std::runtime_error("Missing recorded shader debug identity");
    return {{"action", action},
            {"source_debug_sha256",
             digest(debugConfigCanonicalJson(result.value("source_debug", Json::object())))},
            {"disassembly_sha256", digest(assembly.get<std::string>())}};
}
Json ReplayDebugModel::configuration() const {
    Json watches = Json::array();
    for (const auto &e : watches_)
        watches.push_back(e.text());
    return {{"format", "FloraGPA debug config 1"},
            {"identity", identity(*result_)},
            {"breakpoints", breakpoints()},
            {"watches", watches}};
}
void ReplayDebugModel::importConfiguration(const Json &config) {
    if (!fields(config, {"format", "identity", "breakpoints", "watches"}) ||
        config.at("format") != "FloraGPA debug config 1")
        throw std::runtime_error("Unsupported recorded shader debug configuration");
    if (config.at("identity") != identity(*result_))
        throw std::runtime_error("Configuration does not match this shader source, assembly or stage");
    const auto &points = config.at("breakpoints"), &texts = config.at("watches");
    if (!points.is_array() || points.size() > 256 || !texts.is_array() || texts.size() > 64)
        throw std::runtime_error("Debugger configuration exceeds rule limits");
    ReplayDebugModel replacement(*result_);
    std::set<std::string> unique;
    for (const auto &text : texts) {
        if (!text.is_string())
            throw std::runtime_error("Watch expressions must be strings");
        DebugExpression expression(text.get<std::string>());
        if (!unique.insert(expression.text()).second)
            throw std::runtime_error("Duplicate watch expression");
        replacement.addWatch(expression.text());
    }
    for (const auto &p : points) {
        if (!p.is_object() || !p.contains("file") || !p.contains("line"))
            throw std::runtime_error("Invalid source breakpoint");
        for (const auto &v : p.items())
            if (v.key() != "file" && v.key() != "line" && v.key() != "condition" && v.key() != "hit_count")
                throw std::runtime_error("Invalid source breakpoint field");
        Point point{integer(p.at("file")), integer(p.at("line"))};
        if (replacement.points_.contains(point))
            throw std::runtime_error("Duplicate source breakpoint");
        const auto condition = p.value("condition", Json(""));
        if (!condition.is_string())
            throw std::runtime_error("Breakpoint condition must be text");
        replacement.setCondition(point, condition.get<std::string>());
        if (p.contains("hit_count")) {
            const auto &rule = p.at("hit_count");
            if (!fields(rule, {"mode", "count"}) || !rule.at("mode").is_string())
                throw std::runtime_error("Invalid hit count rule");
            replacement.setHitCount(point, rule.at("mode"), integer(rule.at("count")));
        }
    }
    points_ = std::move(replacement.points_);
    conditions_ = std::move(replacement.conditions_);
    counts_ = std::move(replacement.counts_);
    watches_ = std::move(replacement.watches_);
}
Json ReplayDebugModel::exportReport() const {
    auto out = *result_;
    out["source_breakpoints"] = breakpoints();
    out["source_variable_snapshot"] = {{"step", position()}, {"variables", sourceValues()}};
    out["callstack_snapshot"] = {{"step", position()}, {"functions", stack(position())}};
    out["watch_snapshot"] = {{"step", position()}, {"expressions", watchResults()}};
    out["debug_config"] = configuration();
    return out;
}
} // namespace flora
