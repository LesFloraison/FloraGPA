// Build-time generator. The checked API description is data, not executable
// expressions from the application or capture file. No Python dependency.
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>
using Json = nlohmann::json;
namespace {
std::vector<std::string> ancestry(const Json& spec, std::string name) {
    std::vector<std::string> result;
    std::set<std::string> visited;
    while (name != "IUnknown") {
        if (!visited.insert(name).second) throw std::runtime_error("Inheritance cycle");
        result.push_back(name); name = spec.at("interfaces").at(name).at("parent");
    }
    return result;
}
std::string match(const std::vector<std::string>& chain) {
    std::string result;
    for (const auto& name : chain) { if (!result.empty()) result += " || "; result += "iid == __uuidof(" + name + ")"; }
    return result;
}
void method(std::ostream& out, const std::string& iface, const Json& m) {
    const std::string name = m.at("name"), result = m.at("result"), api = iface + "::" + name;
    const auto& parameters = m.at("parameters");
    std::string args, pre, post, iid;
    for (const auto& p : parameters) if (p.at("type") == "REFIID") iid = p.at("name");
    out << result << " STDMETHODCALLTYPE " << name << "(";
    bool first = true;
    for (const auto& p : parameters) {
        if (!first) out << ", "; first = false;
        out << p.at("declaration").get<std::string>();
        const std::string pname = p.at("name"), type = p.at("type"), count = p.at("count"), direction = p.at("direction");
        std::smatch com;
        const bool is_com = std::regex_search(type, com, std::regex("\\b(I(?:D3D|DXGI|Unknown)[A-Za-z0-9_]*)\\b"));
        std::string arg = pname;
        if (is_com && direction == "in" && name != "SetPrivateDataInterface") {
            if (count.empty()) arg = "unwrap(" + pname + ")";
            else {
                std::string keep = "";
                if (name == "OMSetRenderTargetsAndUnorderedAccessViews") keep = ", " + count + " == UINT(-1)";
                pre += "InputArray<" + com[1].str() + "> input_" + pname + "(" + pname + ", " + count + keep + ");\n";
                arg = "input_" + pname + ".get()";
            }
        } else if (is_com && direction == "out") {
            if (count.empty()) post += "wrap_object(" + pname + ", \"" + api + "\");\n";
            else {
                std::string limit = count;
                if (count.front() == '*') {
                    const std::string capacity = "capacity_" + pname;
                    pre += "const UINT " + capacity + " = " + count.substr(1) + " ? " + count + " : 0;\n";
                    limit = "(std::min)(" + capacity + ", (" + count.substr(1) + " ? " + count + " : 0))";
                }
                post += "if (" + pname + ") for (UINT i = 0; i < " + limit + "; ++i) wrap_object(&" + pname + "[i], \"" + api + "\");\n";
            }
        } else if (direction == "out" && std::regex_match(type, std::regex("void\\s*\\*\\s*\\*")) && !iid.empty()) {
            post += "wrap_object(" + iid + ", " + pname + ", \"" + api + "\");\n";
        }
        if (!args.empty()) args += ", "; args += arg;
    }
    out << ") override {\nconst DWORD before = GetLastError();\ntry {\n" << pre
        << "auto* target = owner_.native<" << iface << ">();\n"
        << "if (!target) throw std::runtime_error(\"Native interface disappeared\");\n"
        << "NativeCall internal;\nSetLastError(before);\n";
    if (result != "void") out << "auto result = ";
    out << "target->" << name << "(" << args << ");\nconst DWORD after = GetLastError();\n";
    if (!post.empty()) {
        if (result == "HRESULT") out << "if (SUCCEEDED(result)) {\n";
        out << post;
        if (result == "HRESULT") out << "}\n";
    }
    out << "SetLastError(after);\n";
    if (result != "void") out << "return result;\n";
    out << "} catch (...) { object_gap(\"" << api << "\", __uuidof(" << iface << "), \"COM forwarding failed\"); SetLastError(before);";
    if (result == "HRESULT") out << " return E_OUTOFMEMORY;";
    else if (result != "void") out << " return {};";
    out << " }\n}\n";
}
}
int main(int argc, char** argv) {
    try {
        if (argc != 3) throw std::runtime_error("Expected API description and output path");
        std::ifstream input(argv[1]); const Json spec = Json::parse(input);
        if (spec.at("schema") != 1) throw std::runtime_error("Unsupported API schema");
        std::ostringstream out;
        out << "// Generated from capture/api/interfaces.json. Do not edit.\n#include \"objects.hpp\"\nnamespace flora::capture {\nnamespace {\n";
        for (const auto& family : spec.at("families")) {
            const std::string name = family; auto chain = ancestry(spec, name);
            out << "class Proxy_" << name << " final : public " << name << ", public Proxy {\nObject& owner_;\npublic:\n"
                << "explicit Proxy_" << name << "(Object& owner) : owner_(owner) {}\n"
                << "void* primary() noexcept override { return static_cast<" << name << "*>(this); }\n"
                << "void* address(REFIID iid) noexcept override { return (" << match(chain) << ") ? primary() : nullptr; }\n"
                << "HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override { return owner_.QueryInterface(iid, out); }\n"
                << "ULONG STDMETHODCALLTYPE AddRef() override { return owner_.AddRef(); }\n"
                << "ULONG STDMETHODCALLTYPE Release() override { return owner_.Release(); }\n";
            for (const auto& iface : chain) for (const auto& m : spec.at("interfaces").at(iface).at("methods")) method(out, iface, m);
            out << "};\n";
        }
        out << "}\nstd::unique_ptr<Proxy> make_object_proxy(Object& owner, REFIID iid) {\n";
        for (const auto& family : spec.at("families")) {
            const std::string name = family; auto chain = ancestry(spec, name);
            auto anchor = chain.front();
            for (const auto& candidate : chain) {
                if (std::regex_replace(candidate, std::regex("[0-9]+$"), "") != std::regex_replace(anchor, std::regex("[0-9]+$"), "")) break;
                anchor = candidate;
            }
            out << "if ((" << match(chain) << ") && owner.native<" << anchor << ">()) return std::make_unique<Proxy_" << name << ">(owner);\n";
        }
        out << "return {};\n}\n}\n";
        std::ofstream output(argv[2], std::ios::binary | std::ios::trunc); output << out.str();
        if (!output) throw std::runtime_error("Cannot write generated proxies");
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
