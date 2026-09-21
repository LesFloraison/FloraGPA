#include "ShaderProject.h"
#include "ShaderDebugData.h"
#include "UnicodeCaseFoldData.h"
#include "replay/Replay.h"
#include <QString>
#include <d3dcompiler.h>
#include <set>

namespace flora {
namespace {
using Json = nlohmann::json;
using Text = std::u32string;
constexpr size_t sourceLimit = 16 * 1024 * 1024;
constexpr auto projectFormat = "FloraGPA shader project 1";
[[noreturn]] void fail(const std::string &message) { throw std::runtime_error(message); }
Text text(const std::string &value) {
    const auto decoded = shader_debug::decodeSourceText(
        {reinterpret_cast<const uint8_t *>(value.data()), value.size()}, false);
    if (!decoded.valid)
        fail("Invalid UTF-8 project text");
    return QString::fromStdString(value).toStdU32String();
}
std::string utf8(const Text &value) { return QString::fromStdU32String(value).toStdString(); }
bool separator(char32_t c) { return c == U'\\' || c == U'/'; }
bool space(char32_t c) {
    return (c >= 9 && c <= 13) || (c >= 0x1c && c <= 0x20) || c == 0x85 || c == 0xa0 || c == 0x1680 ||
           (c >= 0x2000 && c <= 0x200a) || c == 0x2028 || c == 0x2029 || c == 0x202f || c == 0x205f ||
           c == 0x3000;
}
bool initial(char32_t c) { return (c >= U'A' && c <= U'Z') || (c >= U'a' && c <= U'z') || c == U'_'; }
bool identifierPart(char32_t c) { return initial(c) || (c >= U'0' && c <= U'9'); }
bool identifier(const Json &value) {
    if (!value.is_string())
        return false;
    const auto s = value.get<std::string>();
    return !s.empty() && initial(static_cast<unsigned char>(s[0])) &&
           std::all_of(s.begin(), s.end(), [](unsigned char c) { return identifierPart(c); });
}
Text fold(const Text &value) {
    Text out;
    using namespace shader_debug;
    for (auto cp : value) {
        auto it = std::lower_bound(std::begin(caseFoldEntries), std::end(caseFoldEntries), cp,
                                   [](const auto &e, char32_t c) { return e.codepoint < c; });
        if (it != std::end(caseFoldEntries) && it->codepoint == cp)
            out.append(caseFoldData + it->offset, it->size);
        else
            out.push_back(cp);
    }
    return out;
}
struct Path {
    Text drive, root, tail;
};
Path splitRoot(const Text &p) {
    if (!p.empty() && separator(p[0])) {
        if (p.size() > 1 && separator(p[1])) {
            Text normalized = p;
            std::replace(normalized.begin(), normalized.end(), U'/', U'\\');
            size_t start = fold(normalized.substr(0, 8)) == U"\\\\?\\unc\\" ? 8 : 2;
            auto first = normalized.find(U'\\', start);
            auto second = first == Text::npos ? Text::npos : normalized.find(U'\\', first + 1);
            if (second == Text::npos)
                return {p, {}, {}};
            return {p.substr(0, second), p.substr(second, 1), p.substr(second + 1)};
        }
        return {{}, p.substr(0, 1), p.substr(1)};
    }
    // CPython's Windows splitroot accelerator indexes UTF-16 code units.
    if (p.size() > 1 && p[0] <= 0xffff && p[1] == U':') {
        if (p.size() > 2 && separator(p[2]))
            return {p.substr(0, 2), p.substr(2, 1), p.substr(3)};
        return {p.substr(0, 2), {}, p.substr(2)};
    }
    return {{}, {}, p};
}
bool absolute(const Text &p) {
    // Python 3.13 ntpath: a single leading slash is rooted, not absolute.
    return (p.size() >= 3 && p[1] == U':' && separator(p[2])) ||
           (p.size() >= 2 && separator(p[0]) && separator(p[1]));
}
Text normalize(Text p) {
    std::replace(p.begin(), p.end(), U'/', U'\\');
    auto [drive, root, tail] = splitRoot(p);
    std::vector<Text> parts;
    size_t pos = 0;
    while (pos <= tail.size()) {
        auto end = tail.find(U'\\', pos);
        if (end == Text::npos)
            end = tail.size();
        auto part = tail.substr(pos, end - pos);
        if (!part.empty() && part != U".") {
            if (part == U".." && !parts.empty() && parts.back() != U"..")
                parts.pop_back();
            else if (part != U".." || root.empty())
                parts.push_back(std::move(part));
        }
        pos = end + 1;
    }
    Text out = drive + root;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i)
            out += U'\\';
        out += parts[i];
    }
    return out.empty() ? U"." : out;
}
Text directory(const Text &p) {
    const auto [drive, root, tail] = splitRoot(p);
    auto end = tail.find_last_of(U"\\/");
    Text head = end == Text::npos ? Text{} : tail.substr(0, end + 1);
    while (!head.empty() && separator(head.back()))
        head.pop_back();
    return drive + root + head;
}
Text join(const Text &base, const Text &name) {
    auto a = splitRoot(base), b = splitRoot(name);
    if (!b.root.empty()) {
        if (!b.drive.empty() || a.drive.empty())
            a.drive = b.drive;
        a.root = b.root;
        a.tail = b.tail;
    } else {
        if (!b.drive.empty() && b.drive != a.drive) {
            if (QString::fromStdU32String(b.drive).toLower() != QString::fromStdU32String(a.drive).toLower())
                return b.drive + b.root + b.tail;
            a.drive = b.drive;
        }
        if (!a.tail.empty() && !separator(a.tail.back()))
            a.tail += U'\\';
        a.tail += b.tail;
    }
    if (!a.tail.empty() && a.root.empty() && !a.drive.empty() && a.drive.back() != U':' &&
        !separator(a.drive.back()))
        return a.drive + U'\\' + a.tail;
    return a.drive + a.root + a.tail;
}
bool truth(const Json &v) {
    if (v.is_null())
        return false;
    if (v.is_boolean())
        return v.get<bool>();
    if (v.is_number())
        return v.get<double>() != 0;
    return !v.empty() && (!v.is_string() || !v.get<std::string>().empty());
}
Json capturedFlags(const std::string &raw) {
    auto s = text(raw);
    while (!s.empty() && space(s.back()))
        s.pop_back();
    auto begin = std::find_if_not(s.begin(), s.end(), space);
    s.erase(s.begin(), begin);
    bool negative = false;
    if (!s.empty() && (s[0] == U'+' || s[0] == U'-')) {
        negative = s[0] == U'-';
        s.erase(0, 1);
    }
    // Python int(..., 0) accepts Unicode decimal digits and digit separators.
    for (auto &c : s) {
        const auto digit = QChar::digitValue(c);
        if (digit >= 0 && QChar::category(c) == QChar::Number_DecimalDigit)
            c = U'0' + digit;
    }
    unsigned base = 10;
    bool prefix = false;
    if (s.size() > 1 && s[0] == U'0') {
        const auto c = s[1];
        if (c == U'x' || c == U'X')
            base = 16;
        else if (c == U'o' || c == U'O')
            base = 8;
        else if (c == U'b' || c == U'B')
            base = 2;
        prefix = base != 10;
    }
    const bool leadingZero = !prefix && !s.empty() && s[0] == U'0';
    if (prefix) {
        s.erase(0, 2);
        if (!s.empty() && s[0] == U'_')
            s.erase(0, 1);
    }
    if (s.empty())
        fail("Invalid captured compiler flags");
    uint64_t value = 0;
    bool wasDigit = false;
    for (auto c : s) {
        if (c == U'_') {
            if (!wasDigit)
                fail("Invalid captured compiler flags");
            wasDigit = false;
            continue;
        }
        unsigned digit = c >= U'0' && c <= U'9'   ? c - U'0'
                         : c >= U'a' && c <= U'f' ? c - U'a' + 10
                         : c >= U'A' && c <= U'F' ? c - U'A' + 10
                                                  : 255;
        if (digit >= base || (leadingZero && digit) || value > (UINT64_MAX - digit) / base)
            fail("Invalid captured compiler flags");
        value = value * base + digit;
        wasDigit = true;
    }
    if (!wasDigit)
        fail("Invalid captured compiler flags");
    if (!negative)
        return value;
    if (value > uint64_t(INT64_MAX) + 1)
        fail("Invalid captured compiler flags");
    return value == uint64_t(INT64_MAX) + 1 ? INT64_MIN : -int64_t(value);
}
void exactKeys(const Json &v, const std::set<std::string> &keys, const char *message) {
    if (!v.is_object() || v.size() != keys.size())
        fail(message);
    for (const auto &key : keys)
        if (!v.contains(key))
            fail(message);
}
Bytes bytes(const std::string &s) { return {reinterpret_cast<const uint8_t *>(s.data()), s.size()}; }
auto projectCompiler() {
    // Match the reference's System32 compiler even when Qt deploys an older DLL.
    struct Compiler {
        HMODULE module{};
        decltype(&D3DCompile) compile{};
        Compiler() {
            std::wstring path(32768, L'\0');
            const auto count = GetSystemDirectoryW(path.data(), UINT(path.size()));
            if (!count || count >= path.size())
                fail("Cannot locate system D3D compiler");
            path.resize(count);
            path += L"\\d3dcompiler_47.dll";
            module = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (!module)
                fail("System D3DCompiler 47 is required for shader projects");
            compile = reinterpret_cast<decltype(compile)>(GetProcAddress(module, "D3DCompile"));
            if (!compile) {
                FreeLibrary(module);
                module = nullptr;
                fail("System D3DCompile is unavailable");
            }
        }
        ~Compiler() {
            if (module)
                FreeLibrary(module);
        }
    };
    static const Compiler compiler;
    return compiler.compile;
}
class Includes final : public ID3DInclude {
    const Json &project_;
    std::map<std::string, const Json *> files_;
    std::map<const void *, std::vector<char>> active_;
    std::map<const void *, std::string> parents_;
    size_t openedBytes_{};

  public:
    Json log = Json::array();
    std::vector<std::string> failures;
    Includes(const Json &p, const void *root) : project_(p) {
        for (const auto &f : p.at("files"))
            files_[shaderProjectPathKey(f.at("name"))] = &f;
        parents_[root] = p.at("root").get<std::string>();
    }
    HRESULT STDMETHODCALLTYPE Open(D3D_INCLUDE_TYPE kind, LPCSTR name, LPCVOID parent, LPCVOID *output,
                                   UINT *length) override {
        *output = nullptr;
        *length = 0;
        try {
            if (kind != D3D_INCLUDE_LOCAL && kind != D3D_INCLUDE_SYSTEM)
                fail("Unknown include type");
            if (parent && !parents_.contains(parent))
                fail("Unknown include parent");
            const auto requested = text(name ? name : "");
            const auto key = shaderProjectPathKey(utf8(requested));
            const auto parentName = parent ? parents_.at(parent) : project_.at("root").get<std::string>();
            std::vector<std::string> candidates;
            if (absolute(requested))
                candidates.push_back(key);
            else {
                std::vector<Text> dirs;
                if (kind == D3D_INCLUDE_LOCAL)
                    dirs.push_back(directory(text(parentName)));
                for (const auto &d : project_.at("include_dirs"))
                    dirs.push_back(text(d.get<std::string>()));
                dirs.push_back(directory(text(project_.at("root").get<std::string>())));
                for (const auto &d : dirs)
                    candidates.push_back(shaderProjectPathKey(utf8(join(d, requested))));
            }
            const Json *selected = nullptr;
            for (const auto &candidate : candidates) {
                if (files_.contains(candidate)) {
                    selected = files_.at(candidate);
                    break;
                }
            }
            if (!selected)
                fail("Include not present in project: " + utf8(requested) + " (from " + parentName + ")");
            const auto raw = selected->at("text").get<std::string>();
            openedBytes_ += raw.size();
            if (log.size() >= 4096 || active_.size() >= 256 || openedBytes_ > sourceLimit * 8)
                fail("Include expansion exceeds compilation bounds");
            std::vector<char> buffer(raw.begin(), raw.end());
            buffer.push_back(0);
            const auto pointer = buffer.data();
            active_.emplace(pointer, std::move(buffer));
            parents_[pointer] = selected->at("name").get<std::string>();
            log.push_back({{"request", utf8(requested)},
                           {"parent", parentName},
                           {"kind", kind == D3D_INCLUDE_LOCAL ? "local" : "system"},
                           {"file", selected->at("name")},
                           {"sha256", sha256(bytes(raw))}});
            *output = pointer;
            *length = UINT(raw.size());
            return S_OK;
        } catch (const std::exception &e) {
            failures.push_back(e.what());
            return E_FAIL;
        }
    }
    HRESULT STDMETHODCALLTYPE Close(LPCVOID pointer) override {
        if (!active_.erase(pointer)) {
            failures.push_back("Compiler closed an unknown include");
            return E_FAIL;
        }
        parents_.erase(pointer);
        return S_OK;
    }
};
} // namespace

std::string shaderProjectPathKey(const Json &name) {
    if (!name.is_string())
        fail("Invalid virtual source path");
    const auto p = text(name.get<std::string>());
    if (p.empty() || p.size() > 32767 || p.find_first_of(Text{U'\0', U'\r', U'\n'}) != Text::npos)
        fail("Invalid virtual source path");
    const auto split = splitRoot(p);
    if (!split.drive.empty() && split.root.empty() && !absolute(p))
        fail("Drive-relative source paths are ambiguous");
    return utf8(fold(normalize(p)));
}
Json validateShaderProject(Json p) {
    if (!p.is_object() || p.value("format", Json{}) != projectFormat)
        fail("Unsupported shader project");
    const std::set<std::string> allowed{"format",  "files",        "root",
                                        "entry",   "profile",      "flags",
                                        "defines", "include_dirs", "unresolved_defines"};
    for (auto it = p.begin(); it != p.end(); ++it)
        if (!allowed.contains(it.key()))
            fail("Unknown shader project settings");
    const auto files = p.value("files", Json{});
    if (!files.is_array() || files.empty() || files.size() > 1024)
        fail("Shader project requires 1..1024 files");
    std::map<std::string, std::string> names;
    size_t total = 0;
    for (const auto &f : files) {
        exactKeys(f, {"name", "text"}, "Invalid shader project file");
        auto key = shaderProjectPathKey(f.at("name"));
        if (!names.emplace(key, f.at("name").get<std::string>()).second)
            fail("Duplicate virtual source path: " + f.at("name").get<std::string>());
        if (!f.at("text").is_string())
            fail("Shader source must be text without NUL");
        auto raw = f.at("text").get<std::string>();
        if (text(raw).find(U'\0') != Text::npos)
            fail("Shader source must be text without NUL");
        total += raw.size();
    }
    if (total > sourceLimit)
        fail("Shader project exceeds 16 MiB source limit");
    const auto root = shaderProjectPathKey(p.value("root", Json{}));
    if (!names.contains(root))
        fail("Entry source file is not in the project");
    p["root"] = names.at(root);
    if (!identifier(p.value("entry", Json{})))
        fail("Invalid HLSL entry point");
    const std::set<std::string> stages{"vs", "hs", "ds", "gs", "ps", "cs"}, versions{"4_0", "4_1", "5_0"};
    const auto profile = p.value("profile", Json{});
    if (!profile.is_string())
        fail("Unsupported HLSL profile");
    const auto s = profile.get<std::string>();
    if (s.size() != 6 || s[2] != '_' || !stages.contains(s.substr(0, 2)) || !versions.contains(s.substr(3)))
        fail("Unsupported HLSL profile");
    const auto flags = p.value("flags", Json(2048));
    if (!flags.is_number_integer() ||
        (flags.is_number_unsigned() ? flags.get<uint64_t>() > UINT32_MAX
                                    : flags.get<int64_t>() < 0 || flags.get<int64_t>() > UINT32_MAX))
        fail("Compiler flags must be uint32");
    p["flags"] = flags;
    if (!p.contains("defines"))
        p["defines"] = Json::array();
    if (!p["defines"].is_array() || p["defines"].size() > 1024)
        fail("Invalid macro list");
    std::set<std::string> seen;
    for (const auto &m : p["defines"]) {
        exactKeys(m, {"name", "value"}, "Invalid or duplicate macro name");
        if (!identifier(m.at("name")) || !seen.insert(m.at("name").get<std::string>()).second)
            fail("Invalid or duplicate macro name");
        if (!m.at("value").is_string())
            fail("Invalid macro value");
        auto value = text(m.at("value").get<std::string>());
        if (value.size() > 65536 || value.find(U'\0') != Text::npos)
            fail("Invalid macro value");
    }
    if (!p.contains("include_dirs"))
        p["include_dirs"] = Json::array();
    if (!p["include_dirs"].is_array() || p["include_dirs"].size() > 256)
        fail("Invalid include directory list");
    for (const auto &d : p["include_dirs"])
        shaderProjectPathKey(d);
    if (truth(p.value("unresolved_defines", Json{})))
        fail("Captured macro definitions need explicit values in defines before compiling");
    return p;
}
std::string shaderProjectDigest(const Json &p) {
    const auto serialized = p.dump(-1, ' ', true);
    return sha256(bytes(serialized));
}
Json parseShaderProjectDefines(const std::string &raw) {
    const auto s = text(raw);
    Json out = Json::array();
    size_t pos = 0;
    auto macro = [&](size_t at) {
        return at + 2 < s.size() && s[at] == U'/' && s[at + 1] == U'D' && initial(s[at + 2]);
    };
    while (pos < s.size()) {
        while (pos < s.size() && space(s[pos]))
            ++pos;
        if (pos == s.size())
            break;
        if (!macro(pos))
            fail("Unrecognized captured macro syntax");
        pos += 2;
        auto start = pos++;
        while (pos < s.size() && identifierPart(s[pos]))
            ++pos;
        auto name = s.substr(start, pos - start);
        Text value;
        if (pos < s.size() && s[pos] == U'=') {
            start = ++pos;
            char32_t quote = 0;
            bool escaped = false;
            while (pos < s.size()) {
                const auto c = s[pos];
                if (escaped)
                    escaped = false;
                else if (c == U'\\' && quote)
                    escaped = true;
                else if (quote) {
                    if (c == quote)
                        quote = 0;
                } else if (c == U'\'' || c == U'"')
                    quote = c;
                else if (space(c)) {
                    auto next = pos;
                    while (next < s.size() && space(s[next]))
                        ++next;
                    if (macro(next))
                        break;
                }
                ++pos;
            }
            if (quote)
                fail("Unterminated captured macro quote");
            auto end = pos;
            while (start < end && space(s[start]))
                ++start;
            while (end > start && space(s[end - 1]))
                --end;
            value = s.substr(start, end - start);
        } else if (pos < s.size() && !space(s[pos]))
            fail("Unrecognized captured macro suffix");
        out.push_back({{"name", utf8(name)}, {"value", utf8(value)}});
    }
    return out;
}
Json shaderProjectFromSources(const Json &report, const std::string &profile) {
    Json files = Json::array();
    for (const auto &f : report.at("files")) {
        if (!truth(f.at("text_valid")))
            fail("Source encoding must be resolved before creating an editable project");
        files.push_back(
            {{"name", truth(f.at("name")) ? f.at("name") : Json("unnamed.hlsl")}, {"text", f.at("text")}});
    }
    if (files.empty())
        fail("No embedded sources; load or reconstruct HLSL first");
    const auto &env = report.at("environment");
    const auto flags = capturedFlags(env.value("hlslFlags", std::string("0x800")));
    const auto raw = env.value("hlslDefines", std::string{});
    Json macros = Json::array();
    std::string unresolved;
    try {
        macros = parseShaderProjectDefines(raw);
    } catch (const std::exception &) {
        unresolved = raw;
    }
    return {{"format", projectFormat},
            {"files", files},
            {"root", files.size() == 1 ? files[0]["name"] : Json("")},
            {"entry", env.value("hlslEntry", "main")},
            {"profile", profile},
            {"flags", flags},
            {"defines", macros},
            {"include_dirs", Json::array({"."})},
            {"unresolved_defines", unresolved}};
}
ShaderProjectCompilation compileShaderProject(const Json &project) {
    const auto p = validateShaderProject(project);
    const auto rootName = p.at("root").get<std::string>();
    std::string root;
    for (const auto &f : p.at("files"))
        if (f.at("name") == rootName)
            root = f.at("text").get<std::string>();
    Includes includes(p, root.data());
    std::vector<std::pair<std::string, std::string>> definitions;
    for (const auto &m : p.at("defines"))
        definitions.emplace_back(m.at("name").get<std::string>(), m.at("value").get<std::string>());
    std::vector<D3D_SHADER_MACRO> macros;
    for (const auto &[name, value] : definitions)
        macros.push_back({name.c_str(), value.c_str()});
    macros.push_back({nullptr, nullptr});
    const auto flags = p.at("flags").get<uint32_t>() & ~uint32_t(0x100);
    Com<ID3DBlob> code, errors;
    const auto hr = projectCompiler()(root.data(), root.size(), rootName.c_str(), macros.data(), &includes,
                                      p.at("entry").get<std::string>().c_str(),
                                      p.at("profile").get<std::string>().c_str(), flags, 0, &code, &errors);
    QByteArray diagnostic;
    if (errors)
        diagnostic = QByteArray(static_cast<const char *>(errors->GetBufferPointer()),
                                qsizetype(errors->GetBufferSize()));
    while (diagnostic.endsWith('\0'))
        diagnostic.chop(1);
    auto diagnostics = QString::fromUtf8(diagnostic).toStdString();
    if (FAILED(hr) || !includes.failures.empty()) {
        auto message = QString("HLSL project compilation failed (0x%1):\n")
                           .arg(uint32_t(hr), 8, 16, QChar('0'))
                           .toStdString() +
                       diagnostics + "\n";
        for (const auto &failure : includes.failures)
            message += failure + "\n";
        fail(message);
    }
    ShaderProjectCompilation result;
    if (code) {
        const auto begin = static_cast<const uint8_t *>(code->GetBufferPointer());
        result.bytecode.assign(begin, begin + code->GetBufferSize());
    }
    result.report = {{"diagnostics", diagnostics},
                     {"project_sha256", shaderProjectDigest(p)},
                     {"entry", p.at("entry")},
                     {"profile", p.at("profile")},
                     {"flags", flags},
                     {"defines", p.at("defines")},
                     {"includes", includes.log},
                     {"filesystem_includes", false}};
    if (flags != p.at("flags").get<uint32_t>()) {
        result.report["requested_flags"] = p.at("flags");
        result.report["removed_legacy_flags"] = 0x100;
    }
    return result;
}
Json verifyShaderProject(Bytes bytecode, const Json &project) {
    auto result = compileShaderProject(project);
    auto left = shader_debug::chunks(bytecode), right = shader_debug::chunks(result.bytecode);
    for (const auto tag : {"SPDB", "SDBG"}) {
        left.erase(tag);
        right.erase(tag);
    }
    bool equal = left.size() == right.size();
    for (const auto &[tag, value] : left)
        equal = equal && right.contains(tag) && std::ranges::equal(value, right.at(tag));
    if (!equal)
        fail("Saved shader project does not reproduce the current non-debug DXBC chunks");
    result.report["source_kind"] = "saved_shader_project";
    result.report["semantic_equivalence"] =
        std::ranges::equal(bytecode, result.bytecode) ? "bytecode_identical" : "non_debug_chunks_identical";
    return result.report;
}
} // namespace flora
