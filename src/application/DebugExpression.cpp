#include "DebugExpression.h"
#include "SourceStack.h"
#include <QRegularExpression>
#include <QString>
#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <locale.h>

namespace flora {
namespace {
using Json = nlohmann::json;
using Scalar = ExpressionScalar;
using Value = ExpressionValue;
bool truth(const Scalar &v) {
    return std::visit([](auto x) { return x != 0; }, v);
}
double real(const Scalar &v) {
    return std::visit([](auto x) { return double(x); }, v);
}
int64_t integer(const Scalar &v) {
    return std::visit([](auto x) { return int64_t(x); }, v);
}
bool integerKind(const std::string &kind) { return kind == "int" || kind == "uint"; }
const std::set<std::string> kinds{"bool", "int", "uint", "float", "double"};
Scalar convert(const Scalar &v, const std::string &kind) {
    if (kind == "bool")
        return truth(v);
    if (integerKind(kind)) {
        uint32_t bits;
        if (const auto f = std::get_if<double>(&v)) {
            if (!std::isfinite(*f) || *f < -2147483648.0 || *f >= 4294967296.0)
                throw ExpressionError("Float-to-integer conversion is out of range");
            bits = uint32_t(int64_t(*f));
        } else
            bits = std::visit([](auto x) { return uint32_t(x); }, v);
        return kind == "int" ? int64_t(std::bit_cast<int32_t>(bits)) : int64_t(bits);
    }
    const auto x = real(v);
    if (kind == "double")
        return x;
    return double(float(x));
}
Value value(const std::string &kind, const std::vector<Scalar> &items) {
    Value result{kind, {}};
    for (const auto &v : items)
        result.values.push_back(convert(v, kind));
    return result;
}
Scalar fromJson(const Json &j) {
    if (j.is_boolean())
        return j.get<bool>();
    if (j.is_number_unsigned())
        return j.get<uint64_t>();
    if (j.is_number_integer())
        return j.get<int64_t>();
    if (j.is_number_float())
        return j.get<double>();
    throw ExpressionError("Source value is not numeric");
}
std::string root(const std::string &path) { return path.substr(0, path.find_first_of(".[")); }
bool identifier(const std::string &s) {
    static const QRegularExpression valid(QStringLiteral("^[A-Za-z_][A-Za-z_0-9]*$"));
    return valid.match(QString::fromStdString(s)).hasMatch();
}
bool function(const std::string &s) {
    if (s == "all" || s == "any" || s == "abs" || s == "min" || s == "max" || kinds.contains(s))
        return true;
    return !s.empty() && s.back() >= '2' && s.back() <= '4' && kinds.contains(s.substr(0, s.size() - 1));
}
const std::map<std::string, int> precedence{{"||", 1}, {"&&", 2}, {"|", 3},  {"^", 4},  {"&", 5},  {"==", 6},
                                            {"!=", 6}, {"<", 7},  {"<=", 7}, {">", 7},  {">=", 7}, {"<<", 8},
                                            {">>", 8}, {"+", 9},  {"-", 9},  {"*", 10}, {"/", 10}, {"%", 10}};
Value binary(const std::string &op, const Value &left, const Value &right) {
    std::string kind;
    for (const auto &k : {"double", "float", "uint", "int", "bool"})
        if (left.kind == k || right.kind == k) {
            kind = k;
            break;
        }
    if (kind == "bool")
        kind = "int";
    const auto count = std::max(left.values.size(), right.values.size());
    if ((left.values.size() != 1 && left.values.size() != count) ||
        (right.values.size() != 1 && right.values.size() != count))
        throw ExpressionError("Vector dimensions do not match");
    const bool shift = op == "<<" || op == ">>";
    if (shift || op == "&" || op == "|" || op == "^") {
        if (!integerKind(left.kind) || !integerKind(right.kind))
            throw ExpressionError("Bitwise operations require integers");
        if (shift)
            kind = left.kind;
    }
    const bool compare = op == "==" || op == "!=" || op == "<" || op == "<=" || op == ">" || op == ">=";
    std::vector<Scalar> items;
    for (size_t i = 0; i < count; ++i) {
        const auto x = convert(left.values[left.values.size() == 1 ? 0 : i], kind);
        const auto rawY = right.values[right.values.size() == 1 ? 0 : i];
        const auto y = shift ? rawY : convert(rawY, kind);
        if ((op == "/" || op == "%") && !truth(y))
            throw ExpressionError("Expression divisor is zero");
        if (shift && (integer(y) < 0 || integer(y) >= 32))
            throw ExpressionError("Shift count must be between 0 and 31");
        if (kind == "float" || kind == "double") {
            const double a = real(x), b = real(y);
            Scalar z;
            if (op == "+")
                z = a + b;
            else if (op == "-")
                z = a - b;
            else if (op == "*")
                z = a * b;
            else if (op == "/")
                z = a / b;
            else if (op == "%") {
                if (std::isinf(a) && !std::isnan(b))
                    throw ExpressionError("Floating remainder is outside the numeric domain");
                z = std::fmod(a, b);
            } else if (op == "==")
                z = a == b;
            else if (op == "!=")
                z = a != b;
            else if (op == "<")
                z = a < b;
            else if (op == "<=")
                z = a <= b;
            else if (op == ">")
                z = a > b;
            else if (op == ">=")
                z = a >= b;
            else if (op == "min")
                z = b < a ? b : a;
            else if (op == "max")
                z = b > a ? b : a;
            else
                throw ExpressionError("Unsupported expression operator");
            items.push_back(z);
        } else {
            const auto a = integer(x), b = integer(y);
            Scalar z;
            if (op == "+")
                z = a + b;
            else if (op == "-")
                z = a - b;
            else if (op == "*")
                z = uint64_t(a) * uint64_t(b);
            else if (op == "/")
                z = a / b;
            else if (op == "%")
                z = a % b;
            else if (op == "&")
                z = a & b;
            else if (op == "|")
                z = a | b;
            else if (op == "^")
                z = a ^ b;
            else if (op == "<<")
                z = uint64_t(a) << b;
            else if (op == ">>")
                z = a >> b;
            else if (op == "==")
                z = a == b;
            else if (op == "!=")
                z = a != b;
            else if (op == "<")
                z = a < b;
            else if (op == "<=")
                z = a <= b;
            else if (op == ">")
                z = a > b;
            else if (op == ">=")
                z = a >= b;
            else if (op == "min")
                z = std::min(a, b);
            else if (op == "max")
                z = std::max(a, b);
            else
                throw ExpressionError("Unsupported expression operator");
            items.push_back(z);
        }
    }
    return value(compare ? "bool" : kind, items);
}
} // namespace
namespace expression_detail {
struct Node {
    std::string op, text;
    Value literal;
    std::vector<std::shared_ptr<const Node>> children;
};
using NodePtr = std::shared_ptr<const Node>;
class Parser {
    std::vector<std::string> tokens_;
    size_t position_ = 0;
    std::string peek() const { return position_ < tokens_.size() ? tokens_[position_] : ""; }
    std::string take() {
        auto t = peek();
        ++position_;
        return t;
    }
    void expect(const std::string &token) {
        if (take() != token)
            throw ExpressionError("Expected " + token);
    }
    NodePtr make(std::string op, std::string text, std::vector<NodePtr> children = {}) {
        return std::make_shared<Node>(Node{op, text, {}, children});
    }
    Value literal(std::string token) {
        // Python decimal literals accept Unicode decimal digits, but identifiers are ASCII.
        std::string normalized;
        for (auto cp : QString::fromStdString(token).toUcs4()) {
            const auto digit = QChar::digitValue(cp);
            if (QChar::isDigit(cp) && digit >= 0)
                normalized += char('0' + digit);
            else if (cp < 128)
                normalized += char(cp);
            else
                throw ExpressionError("Unsupported numeric literal");
        }
        token = normalized;
        std::transform(token.begin(), token.end(), token.begin(),
                       [](unsigned char c) { return char(std::tolower(c)); });
        bool unsign = false;
        uint64_t number = 0;
        if (token.starts_with("0x")) {
            unsign = token.ends_with('u');
            if (unsign)
                token.pop_back();
            auto result = std::from_chars(token.data() + 2, token.data() + token.size(), number, 16);
            if (result.ec != std::errc{} || result.ptr != token.data() + token.size())
                throw ExpressionError("Integer literal exceeds uint32");
        } else if (token.find_first_of(".e") != std::string::npos || token.ends_with('f')) {
            if (token.ends_with('u'))
                throw ExpressionError("Floating literal cannot use an unsigned suffix");
            if (token.ends_with('f'))
                token.pop_back();
            struct Locale {
                _locale_t value = _create_locale(LC_NUMERIC, "C");
                ~Locale() { _free_locale(value); }
            };
            static const Locale locale;
            char *end = nullptr;
            const auto f = _strtod_l(token.c_str(), &end, locale.value);
            if (!end || end != token.c_str() + token.size())
                throw ExpressionError("Invalid floating literal");
            return value("float", {f});
        } else {
            unsign = token.ends_with('u');
            if (unsign)
                token.pop_back();
            auto result = std::from_chars(token.data(), token.data() + token.size(), number);
            if (result.ec != std::errc{} || result.ptr != token.data() + token.size())
                throw ExpressionError("Integer literal exceeds uint32");
        }
        if (number > UINT32_MAX)
            throw ExpressionError("Integer literal exceeds uint32");
        return value(unsign || number > INT32_MAX ? "uint" : "int", {number});
    }
    NodePtr parse(int minimum = 0, int depth = 0) {
        if (depth > 40)
            throw ExpressionError("Expression nesting exceeds 40 levels");
        auto token = take();
        NodePtr node;
        if (token == "+" || token == "-" || token == "!" || token == "~")
            node = make("unary", token, {parse(11, depth + 1)});
        else if (token == "(") {
            node = parse(0, depth + 1);
            expect(")");
        } else if (!token.empty() &&
                   (token[0] == '.' || QChar::isDigit(QString::fromStdString(token).toUcs4().front())))
            node = std::make_shared<Node>(Node{"literal", "", literal(token), {}});
        else if (identifier(token)) {
            if (token == "true" || token == "false")
                node = std::make_shared<Node>(Node{"literal", "", Value{"bool", {token == "true"}}, {}});
            else if (peek() == "(") {
                if (!function(token))
                    throw ExpressionError("Unsupported expression function: " + token);
                take();
                std::vector<NodePtr> args;
                if (peek() != ")")
                    for (;;) {
                        args.push_back(parse(0, depth + 1));
                        if (peek() != ",")
                            break;
                        take();
                    }
                expect(")");
                node = make("call", token, args);
            } else
                node = make("name", token);
        } else
            throw ExpressionError("Expected a variable, literal or parenthesized expression");
        for (;;) {
            token = peek();
            if (token == ".") {
                take();
                auto field = take();
                if (!identifier(field))
                    throw ExpressionError("Expected member name");
                node = make("member", field, {node});
            } else if (token == "[") {
                take();
                auto index = parse(0, depth + 1);
                expect("]");
                node = make("index", "", {node, index});
            } else if (precedence.contains(token) && precedence.at(token) >= minimum) {
                take();
                node = make("binary", token, {node, parse(precedence.at(token) + 1, depth + 1)});
            } else if (token == "?" && minimum == 0) {
                take();
                auto yes = parse(0, depth + 1);
                expect(":");
                node = make("select", "", {node, yes, parse(0, depth + 1)});
            } else
                break;
        }
        return node;
    }

  public:
    explicit Parser(const QString &text) {
        static const QRegularExpression pattern(
            QStringLiteral(
                R"([\s\x{001c}-\x{001f}]*(?:(0[xX][0-9a-fA-F]+[uU]?|(?:\d+\.\d*|\.\d+|\d+)(?:[eE][+-]?\d+)?[uUfF]?)|([A-Za-z_][A-Za-z_0-9]*)|(&&|\|\||==|!=|<=|>=|<<|>>|\+\+|--|[+*/%<>&|^!~?:().,\[\]-])))"),
            QRegularExpression::UseUnicodePropertiesOption);
        qsizetype pos = 0;
        while (pos < text.size()) {
            const auto match = pattern.match(text, pos, QRegularExpression::NormalMatch,
                                             QRegularExpression::AnchorAtOffsetMatchOption);
            if (!match.hasMatch())
                throw ExpressionError("Unsupported expression character");
            tokens_.push_back((!match.captured(1).isEmpty()   ? match.captured(1)
                               : !match.captured(2).isEmpty() ? match.captured(2)
                                                              : match.captured(3))
                                  .toStdString());
            pos = match.capturedEnd();
            if (tokens_.size() > 256)
                throw ExpressionError("Expression exceeds 256 tokens");
        }
        tokens_.push_back("");
    }
    NodePtr result() {
        auto result = parse();
        if (!peek().empty())
            throw ExpressionError("Extra or unsupported expression token: " + peek());
        return result;
    }
};
} // namespace expression_detail
const Scalar &ExpressionValue::scalar() const {
    if (values.size() != 1)
        throw ExpressionError("A scalar is required; reduce vector conditions with all or any");
    return values.front();
}
bool ExpressionValue::truth() const { return flora::truth(scalar()); }
std::string ExpressionValue::text() const {
    std::string result;
    for (const auto &v : values) {
        if (!result.empty())
            result += ", ";
        if (auto b = std::get_if<bool>(&v))
            result += *b ? "true" : "false";
        else if (auto f = std::get_if<double>(&v)) {
            if (std::isnan(*f))
                result += "nan";
            else if (std::isinf(*f))
                result += *f < 0 ? "-inf" : "inf";
            else {
                char buffer[64];
                const auto r = std::to_chars(buffer, buffer + sizeof buffer, *f, std::chars_format::general,
                                             kind == "double" ? 17 : 9);
                if (r.ec != std::errc{})
                    throw ExpressionError("Cannot format expression value");
                result.append(buffer, r.ptr);
            }
        } else if (auto i = std::get_if<int64_t>(&v))
            result += std::to_string(*i);
        else
            result += std::to_string(std::get<uint64_t>(v));
    }
    return result;
}
DebugExpression::DebugExpression(const std::string &text) {
    auto cps = QString::fromStdString(text).toUcs4();
    if (cps.empty() || cps.size() > 2048)
        throw ExpressionError("Expression length must be 1 to 2048 characters");
    auto whitespace = [](char32_t cp) { return QChar::isSpace(cp) || (cp >= 0x1c && cp <= 0x1f); };
    size_t first = 0, last = cps.size();
    while (first < last && whitespace(cps[first]))
        ++first;
    while (last > first && whitespace(cps[last - 1]))
        --last;
    if (first == last)
        throw ExpressionError("Expression length must be 1 to 2048 characters");
    const std::u32string trimmedPoints(cps.begin() + first, cps.begin() + last);
    const auto trimmed = QString::fromUcs4(trimmedPoints.data(), qsizetype(trimmedPoints.size()));
    text_ = trimmed.toStdString();
    tree_ = expression_detail::Parser(trimmed).result();
}
Value DebugExpression::evaluate(const DebugEnvironment &environment) const {
    return environment.expression(*this);
}
DebugEnvironment::DebugEnvironment(Json records) : records_(std::move(records)) {
    for (const auto &r : records_)
        if (r.at("scope") == "local")
            localRoots_.insert(root(r.value("source_container", r.at("name").get<std::string>())));
}
std::optional<Value> DebugEnvironment::exact(const std::string &path) const {
    const auto r = root(path);
    if (ambiguousRoots_.contains(r))
        throw ExpressionError("Multiple visible SDBG declarations: " + r);
    const std::string scope = localRoots_.contains(r) ? "local" : "global";
    const Json *found = nullptr;
    for (const auto &row : records_)
        if (row.at("scope") == scope && row.at("name") == path) {
            if (found)
                throw ExpressionError("Ambiguous variable mapping: " + path);
            found = &row;
        }
    if (!found)
        return {};
    if (found->at("status") != "available")
        throw ExpressionError("Unavailable expression value: " + path + " (" +
                              found->at("status").get<std::string>() + ")");
    auto kind = found->at("type").get<std::string>();
    const std::map<std::string, std::string> promote{{"half", "float"}, {"int8", "int"},    {"int16", "int"},
                                                     {"uint8", "uint"}, {"uint16", "uint"}, {"enum", "uint"}};
    if (promote.contains(kind))
        kind = promote.at(kind);
    if (!kinds.contains(kind))
        throw ExpressionError("Unsupported expression type: " + kind);
    const auto &items = found->at("values");
    if (items.empty() || items.size() > 4 ||
        items.size() != size_t(std::max<int64_t>(1, found->at("columns").get<int64_t>())))
        throw ExpressionError("Incomplete source dimensions or values: " + path);
    std::vector<Scalar> values;
    for (const auto &v : items)
        values.push_back(fromJson(v));
    return value(kind, values);
}
std::optional<Value> DebugEnvironment::lookup(const std::string &path) const {
    if (auto v = exact(path))
        return v;
    std::string base = path;
    std::optional<std::vector<size_t>> indices;
    if (!groups_.contains(path)) {
        static const QRegularExpression swizzle(QStringLiteral(R"(^(.+)\.([xyzw]{1,4}|[rgba]{1,4})$)"));
        static const QRegularExpression indexed(QStringLiteral(R"(^(.+)\[(\d+)\]$)"));
        const auto member = swizzle.match(QString::fromStdString(path)),
                   index = indexed.match(QString::fromStdString(path));
        if (member.hasMatch() && groups_.contains(member.captured(1).toStdString())) {
            base = member.captured(1).toStdString();
            indices.emplace();
            const auto components = member.captured(2).toStdString();
            const std::string alphabet =
                components.find_first_not_of("xyzw") == std::string::npos ? "xyzw" : "rgba";
            for (const auto c : components)
                indices->push_back(alphabet.find(c));
        } else if (index.hasMatch() && groups_.contains(index.captured(1).toStdString())) {
            base = index.captured(1).toStdString();
            bool ok = false;
            const auto i = index.captured(2).toULongLong(&ok);
            if (!ok)
                throw ExpressionError("Vector component is out of range: " + path);
            indices = std::vector<size_t>{size_t(i)};
        } else
            return {};
    }
    const auto &groups = groups_.at(base);
    if (groups.size() != 1)
        throw ExpressionError("Ambiguous variable mapping: " + base);
    const auto &names = groups.front();
    if (!indices) {
        indices.emplace();
        for (size_t i = 0; i < names.size(); ++i)
            indices->push_back(i);
    }
    Value result;
    for (const auto i : *indices) {
        if (i >= names.size())
            throw ExpressionError("Vector component is out of range: " + path);
        auto v = exact(names[i]);
        if (!v || (!result.kind.empty() && result.kind != v->kind))
            throw ExpressionError("Incomplete vector components: " + path);
        result.kind = v->kind;
        result.values.push_back(v->scalar());
    }
    if (result.values.empty())
        throw ExpressionError("Incomplete vector components: " + path);
    return result;
}
std::optional<std::string> DebugEnvironment::path(const expression_detail::Node &n) const {
    if (n.op == "name")
        return n.text;
    if (n.op == "member") {
        const auto p = path(*n.children[0]);
        return p ? std::optional(*p + "." + n.text) : std::nullopt;
    }
    if (n.op == "index") {
        const auto p = path(*n.children[0]);
        return p ? std::optional(*p + "[" + std::to_string(index(*n.children[1])) + "]") : std::nullopt;
    }
    return {};
}
uint32_t DebugEnvironment::index(const expression_detail::Node &node) const {
    const auto result = evaluate(node);
    if (!integerKind(result.kind))
        throw ExpressionError("Index must be an integer scalar");
    const auto i = integer(result.scalar());
    if (i < 0 || i > INT32_MAX)
        throw ExpressionError("Index is out of range");
    return uint32_t(i);
}
Value DebugEnvironment::evaluate(const expression_detail::Node &n) const {
    const auto &op = n.op;
    if (op == "literal")
        return n.literal;
    if (op == "name" || op == "member" || op == "index") {
        const auto p = path(n);
        if (p)
            if (auto v = lookup(*p))
                return *v;
        if (op == "name")
            throw ExpressionError("No variable in the current scope: " + n.text);
        const auto base = evaluate(*n.children[0]);
        if (op == "index") {
            const auto i = index(*n.children[1]);
            if (i >= base.values.size())
                throw ExpressionError("Vector index is out of range");
            return {base.kind, {base.values[i]}};
        }
        const auto &field = n.text;
        const std::string alphabet = field.find_first_not_of("xyzw") == std::string::npos   ? "xyzw"
                                     : field.find_first_not_of("rgba") == std::string::npos ? "rgba"
                                                                                            : "";
        if (alphabet.empty() || field.empty() || field.size() > 4)
            throw ExpressionError("No mapped member or valid swizzle: " + field);
        Value result{base.kind, {}};
        for (const auto c : field) {
            const auto i = alphabet.find(c);
            if (i >= base.values.size())
                throw ExpressionError("Swizzle exceeds vector dimensions");
            result.values.push_back(base.values[i]);
        }
        return result;
    }
    if (op == "select")
        return evaluate(*n.children[evaluate(*n.children[0]).truth() ? 1 : 2]);
    if (op == "unary") {
        const auto x = evaluate(*n.children[0]);
        std::vector<Scalar> result;
        if (n.text == "!") {
            for (const auto &v : x.values)
                result.push_back(!truth(v));
            return value("bool", result);
        }
        if (n.text == "~") {
            if (!integerKind(x.kind))
                throw ExpressionError("Bitwise complement requires integers");
            for (const auto &v : x.values)
                result.push_back(~integer(v));
            return value(x.kind, result);
        }
        const auto kind = x.kind == "bool" ? "int" : x.kind;
        for (const auto &v : x.values)
            result.push_back(n.text == "+"                           ? v
                             : (kind == "float" || kind == "double") ? Scalar(-real(v))
                                                                     : Scalar(-integer(v)));
        return value(kind, result);
    }
    if (op == "binary") {
        const auto left = evaluate(*n.children[0]);
        if (n.text == "&&")
            return {"bool", {left.truth() && evaluate(*n.children[1]).truth()}};
        if (n.text == "||")
            return {"bool", {left.truth() || evaluate(*n.children[1]).truth()}};
        return binary(n.text, left, evaluate(*n.children[1]));
    }
    if (op == "call") {
        std::vector<Value> args;
        for (const auto &c : n.children)
            args.push_back(evaluate(*c));
        const auto &name = n.text;
        if (name == "all" || name == "any" || name == "abs") {
            if (args.size() != 1)
                throw ExpressionError(name + " requires one argument");
            const auto &x = args.front();
            if (name == "all")
                return {"bool", {std::all_of(x.values.begin(), x.values.end(), truth)}};
            if (name == "any")
                return {"bool", {std::any_of(x.values.begin(), x.values.end(), truth)}};
            std::vector<Scalar> values;
            for (const auto &v : x.values)
                values.push_back((x.kind == "float" || x.kind == "double") ? Scalar(std::abs(real(v)))
                                                                           : Scalar(std::abs(integer(v))));
            return value(x.kind == "bool" ? "int" : x.kind, values);
        }
        if (name == "min" || name == "max") {
            if (args.size() != 2)
                throw ExpressionError(name + " requires two arguments");
            return binary(name, args[0], args[1]);
        }
        const bool vector = name.back() >= '2' && name.back() <= '4';
        const auto count = vector ? size_t(name.back() - '0') : 1;
        const auto kind = vector ? name.substr(0, name.size() - 1) : name;
        std::vector<Scalar> items;
        for (const auto &arg : args)
            items.insert(items.end(), arg.values.begin(), arg.values.end());
        if (items.size() == 1)
            items.resize(count, items.front());
        if (items.size() != count)
            throw ExpressionError("Constructor component count does not match: " + name);
        return value(kind, items);
    }
    throw ExpressionError("Unknown expression node");
}
Value DebugEnvironment::expression(const DebugExpression &expression) const {
    try {
        return evaluate(*expression.tree_);
    } catch (const ExpressionError &) {
        throw;
    } catch (const std::exception &e) {
        throw ExpressionError(std::string("Cannot evaluate expression: ") + e.what());
    }
}
DebugEnvironment DebugEnvironment::initialize(const Json &variables, const Json &values,
                                              const std::set<std::string> &selected) {
    Json records = Json::array();
    for (const auto &row : values) {
        if (!selected.contains(row.at("variable_id").get<std::string>()))
            continue;
        const auto kind = row.at("type").get<std::string>();
        Json items = Json::array();
        if (row.at("status") == "available") {
            const auto bits = row.at("bits").get<uint64_t>();
            if (kind == "float")
                items.push_back(std::bit_cast<float>(uint32_t(bits)));
            else if (kind == "double")
                items.push_back(std::bit_cast<double>(bits));
            else if (kind == "int")
                items.push_back(std::bit_cast<int32_t>(uint32_t(bits)));
            else if (kind == "int64")
                items.push_back(std::bit_cast<int64_t>(bits));
            else if (kind == "bool")
                items.push_back(bits != 0);
            else
                items.push_back(bits);
        }
        records.push_back({{"name", row.at("name")},
                           {"scope", "local"},
                           {"type", kind},
                           {"status", row.at("status")},
                           {"values", items},
                           {"columns", 1}});
    }
    DebugEnvironment result(records);
    for (const auto &id : selected) {
        const auto &variable = variables.at(id);
        const auto name = variable.at("name").get<std::string>();
        for (const auto &group : variable.value("type", Json::object()).value("vectors", Json::array())) {
            std::vector<std::string> members;
            for (const auto &m : group.at("members"))
                members.push_back(name + m.get<std::string>());
            result.groups_[name + group.at("path").get<std::string>()].push_back(std::move(members));
        }
    }
    return result;
}
DebugEnvironment DebugEnvironment::native(const Json &symbols, const Json &allValues, uint64_t offset,
                                          const std::string &frame) {
    std::map<std::string, Json> scopes;
    Json variables = Json::object();
    for (const auto &s : symbols.at("scopes"))
        scopes[s.at("id").get<std::string>()] = s;
    for (const auto &v : symbols.at("variables"))
        variables[v.at("id").get<std::string>()] = v;
    const auto values = sourceFrameLocals(symbols, allValues, frame);
    struct Candidate {
        std::string name;
        size_t depth;
        bool active;
    };
    std::map<std::string, Candidate> candidates;
    std::map<std::string, size_t> ranks;
    for (const auto &v : values) {
        const auto id = v.at("variable_id").get<std::string>();
        if (candidates.contains(id))
            continue;
        auto scope = v.at("scope_id").get<std::string>();
        size_t depth = 0;
        bool active = true;
        std::set<std::string> seen;
        while (scope != frame) {
            if (!seen.insert(scope).second || seen.size() > 64)
                throw ExpressionError("Recursive source variable scope");
            const auto &block = scopes.at(scope);
            if (!(block.at("code_start").get<uint64_t>() <= offset &&
                  offset < block.at("code_end").get<uint64_t>())) {
                active = false;
                break;
            }
            ++depth;
            scope = block.at("parent").get<std::string>();
        }
        const auto name = variables.at(id).at("name").get<std::string>();
        candidates[id] = {name, depth, active};
        if (active)
            ranks[name] = std::max(ranks[name], depth);
    }
    std::set<std::string> selected;
    for (const auto &[id, c] : candidates)
        if (c.active && ranks.at(c.name) == c.depth)
            selected.insert(id);
    return initialize(variables, values, selected);
}
DebugEnvironment DebugEnvironment::sdbg(const Json &symbols, const Json &values, uint64_t offset) {
    if (symbols.value("status", "") != "available")
        throw ExpressionError("SDBG variable information is unavailable");
    const auto key = std::to_string(offset);
    const auto map = symbols.value("instruction_map", Json::object());
    if (!map.contains(key))
        throw ExpressionError("Current instruction has no SDBG active scope");
    const auto visible = map.at(key).at("visible_variables");
    Json variables = Json::object();
    std::set<std::string> selected;
    std::map<std::string, size_t> counts;
    for (const auto &v : symbols.at("variables"))
        variables[v.at("id").get<std::string>()] = v;
    for (const auto &[id, v] : variables.items())
        if (std::find(visible.begin(), visible.end(), v.at("sdbg_id")) != visible.end() &&
            !v.at("return_value").get<bool>()) {
            selected.insert(id);
            ++counts[v.at("name").get<std::string>()];
        }
    auto result = initialize(variables, values, selected);
    for (const auto &[name, count] : counts)
        if (count > 1)
            result.ambiguousRoots_.insert(name);
    return result;
}
} // namespace flora
