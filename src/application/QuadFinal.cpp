#include "QuadFinal.h"
#include "DxbcInspection.h"
#include "HlslCompilation.h"
#include "PostTransform.h"
#include "core/StreamOutput.h"
#include <algorithm>
#include <regex>
#include <tuple>
namespace flora {
using Json = nlohmann::json;
class QuadFinalState {
    Replay &r;
    Json geometryReport;
    std::vector<Json> fields;
    Json outputs;
    uint32_t stream = 0, factor = 0, stride = 0;
    uint64_t planned = 0, vertices = 0;
    Com<ID3D11VertexShader> vs;
    Com<ID3D11GeometryShader> gs;
    Com<ID3D11InputLayout> layout;
    Com<ID3D11Buffer> buffer;
    Bytes code(Id id) const {
        const auto replacement = r.options_.shaders.find(id);
        return replacement == r.options_.shaders.end() ? r.frame_.shader(r.frame_.resource(id).data)
                                                       : Bytes(replacement->second);
    }

  public:
    QuadFinalState(Replay &replay, const Event &event, const State &state) : r(replay) {
        const auto originalGs = state.stages[3].shader;
        if (r.options_.warp && !originalGs && state.stages[1].shader && state.stages[2].shader &&
            streamOutputTopologies(code(state.stages[1].shader))[0] == 2)
            throw std::runtime_error(
                "WARP direct tessellated isolines cannot preserve native rasterization in Quad diagnostics; "
                "select the hardware device. Final geometry export remains available.");
        if (originalGs)
            if (const auto declaration = shaderStreamOutput(r.frame_, originalGs))
                stream = readStreamOutputDeclaration(r.frame_, declaration).rasterizedStream;
        if (stream == UINT32_MAX)
            throw std::runtime_error("Final-geometry Quad inspection requires a rasterized GS stream");
        auto geometry = captureBoundPostTransform(r, event, state, stream);
        factor = geometry.report.at("vertices_per_primitive").get<uint32_t>();
        if (factor < 1 || factor > 3)
            throw std::runtime_error("Final-geometry Quad serialization requires points, lines or triangles");
        planned = geometry.report.at("primitives").get<uint64_t>();
        vertices = geometry.report.at("vertices").get<uint64_t>();
        stride = geometry.report.at("stride").get<uint32_t>();
        if (!stride || vertices > geometry.bytes.size() / stride ||
            vertices * stride != geometry.bytes.size() || vertices % factor || vertices / factor != planned ||
            geometry.bytes.size() > UINT32_MAX)
            throw std::runtime_error("Invalid final-geometry Quad vertex storage");
        for (const auto &field : geometry.report.at("attributes")) {
            const auto system = field.at("system_value").get<uint32_t>();
            if (system >= 1 && system <= 5)
                fields.push_back(field);
        }
        std::stable_sort(fields.begin(), fields.end(), [](const auto &a, const auto &b) {
            return (a.at("system_value") != 1) < (b.at("system_value") != 1);
        });
        if (std::none_of(fields.begin(), fields.end(), [](const auto &a) {
                return a.at("system_value") == 1 && a.at("component_type") == 3 &&
                       a.at("components") == Json::array({0, 1, 2, 3});
            }))
            throw std::runtime_error("Final-geometry Quad inspection requires complete float4 SV_Position");
        std::string input = "struct V{", bridge = "struct B{", output = "struct O{";
        std::string vsBody = "B main(V v){B o;", gsBody;
        std::vector<D3D11_INPUT_ELEMENT_DESC> elements;
        static constexpr DXGI_FORMAT formats[3][4] = {
            {DXGI_FORMAT_R32_UINT, DXGI_FORMAT_R32G32_UINT, DXGI_FORMAT_R32G32B32_UINT,
             DXGI_FORMAT_R32G32B32A32_UINT},
            {DXGI_FORMAT_R32_SINT, DXGI_FORMAT_R32G32_SINT, DXGI_FORMAT_R32G32B32_SINT,
             DXGI_FORMAT_R32G32B32A32_SINT},
            {DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32G32_FLOAT, DXGI_FORMAT_R32G32B32_FLOAT,
             DXGI_FORMAT_R32G32B32A32_FLOAT}};
        static constexpr const char *types[]{"uint", "int", "float"};
        for (size_t i = 0; i < fields.size(); ++i) {
            const auto &a = fields[i];
            const auto name = a.at("semantic").get<std::string>();
            const auto type = a.at("component_type").get<uint32_t>();
            const auto count = a.at("component_count").get<uint32_t>();
            const auto offset = a.at("offset").get<uint32_t>();
            Json components = Json::array();
            for (uint32_t c = 0; c < count && c < 4; ++c)
                components.push_back(c);
            if (!std::regex_match(name, std::regex("[A-Za-z_][A-Za-z0-9_]*")) || type < 1 || type > 3 ||
                count < 1 || count > 4 || a.at("components") != components || offset > stride ||
                count > (stride - offset) / 4)
                throw std::runtime_error("Unsupported rasterizer output semantic layout");
            const auto number = std::to_string(i), member = "a" + number;
            const auto declaration =
                std::string(types[type - 1]) + std::to_string(count) + " " + member + ":";
            input += declaration + "GEO" + number + ";";
            bridge += declaration + "TEXCOORD" + number + ";";
            output += declaration + name + std::to_string(a.at("index").get<uint32_t>()) + ";";
            if (!i)
                output += "uint id:SV_PrimitiveID;";
            vsBody += "o." + member + "=v." + member + ";";
            gsBody += "o." + member + "=v[n]." + member + ";";
            elements.push_back(
                {"GEO", UINT(i), formats[type - 1][count - 1], 0, offset, D3D11_INPUT_PER_VERTEX_DATA, 0});
        }
        input += "};";
        bridge += "};";
        output += "};";
        const auto vsCode = compileHlsl(input + bridge + vsBody + "return o;}", "vs_5_0").bytecode;
        static constexpr const char *primitives[]{"point", "line", "triangle"};
        static constexpr const char *streams[]{"PointStream", "LineStream", "TriangleStream"};
        const auto n = std::to_string(factor);
        const auto gsCode =
            compileHlsl(bridge + output + "[maxvertexcount(" + n + ")]void main(" + primitives[factor - 1] +
                            " B v[" + n + "],inout " + streams[factor - 1] +
                            "<O> dst){O o;o.id=0;[unroll]for(uint n=0;n<" + n + ";n++){" + gsBody +
                            "dst.Append(o);}}",
                        "gs_5_0")
                .bytecode;
        outputs = Json::array();
        for (const auto &[tag, bytes] : readDxbcParts(gsCode))
            if (tag == 0x4e47534f || tag == 0x3547534f) {
                if (!outputs.empty())
                    throw std::runtime_error("Conflicting Quad bridge output signatures");
                outputs = dxbc_detail::signature(bytes, tag == 0x3547534f);
            }
        for (const auto &[system, reg, mask] : {std::tuple(1u, 0u, 15u), std::tuple(7u, 1u, 1u)})
            if (std::none_of(outputs.begin(), outputs.end(), [&](const auto &a) {
                    return a.at("system_value") == system && a.at("register") == reg && a.at("mask") == mask;
                }))
                throw std::runtime_error(
                    "Generated geometry shader does not match the quad PS input registers");
        check(r.device_->CreateVertexShader(vsCode.data(), vsCode.size(), nullptr, &vs),
              "Create Quad bridge VS");
        check(r.device_->CreateGeometryShader(gsCode.data(), gsCode.size(), nullptr, &gs),
              "Create Quad bridge GS");
        check(r.device_->CreateInputLayout(elements.data(), UINT(elements.size()), vsCode.data(),
                                           vsCode.size(), &layout),
              "Create final geometry diagnostic input layout");
        if (!geometry.bytes.empty()) {
            D3D11_BUFFER_DESC desc{};
            desc.ByteWidth = UINT(geometry.bytes.size());
            desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
            const D3D11_SUBRESOURCE_DATA data{geometry.bytes.data(), 0, 0};
            check(r.device_->CreateBuffer(&desc, &data, &buffer), "Create Quad final vertex buffer");
        }
        geometryReport = std::move(geometry.report);
    }
    uint64_t submit() {
        if (!planned)
            return 0;
        auto c = r.context_.Get();
        r.unbindStreamOutput();
        c->HSSetShader(nullptr, nullptr, 0);
        c->DSSetShader(nullptr, nullptr, 0);
        c->VSSetShader(vs.Get(), nullptr, 0);
        c->GSSetShader(gs.Get(), nullptr, 0);
        c->IASetInputLayout(layout.Get());
        auto vertexBuffer = buffer.Get();
        const UINT offset = 0;
        c->IASetVertexBuffers(0, 1, &vertexBuffer, &stride, &offset);
        c->IASetIndexBuffer(nullptr, DXGI_FORMAT_R32_UINT, 0);
        c->IASetPrimitiveTopology(factor == 1   ? D3D11_PRIMITIVE_TOPOLOGY_POINTLIST
                                  : factor == 2 ? D3D11_PRIMITIVE_TOPOLOGY_LINELIST
                                                : D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        for (uint64_t first = 0; first < vertices; first += factor)
            c->Draw(factor, UINT(first));
        return planned;
    }
    Json metadata() const {
        Json rasterizer = Json::array();
        for (const auto &field : fields)
            rasterizer.push_back({{"semantic", field.at("semantic")},
                                  {"index", field.at("index")},
                                  {"component_type", field.at("component_type")},
                                  {"components", field.at("components")}});
        static constexpr const char *names[]{"points", "lines", "triangles"};
        return {{"strategy", std::string("serialized_final_shader_") + names[factor - 1]},
                {"planned_submissions", planned},
                {"vertices_per_primitive", factor},
                {"rasterized_stream", stream},
                {"geometry", geometryReport},
                {"rasterizer_outputs", rasterizer},
                {"primitive_id", "local zero for each isolated primitive"},
                {"limitations",
                 {"Final output is captured before serialization; diagnostic primitives bypass original "
                  "VS/HS/DS/GS.",
                  "Splitting and re-submission change scheduling; this is not a physical quad invocation "
                  "measurement.",
                  "Original SO targets are unbound during helper draws; pre-raster UAV writes use private "
                  "resource/counter snapshots."}}};
    }
    const Json &outputSignatures() const { return outputs; }
};
QuadFinal::QuadFinal(Replay &replay, const Event &event, const State &state)
    : state_(std::make_unique<QuadFinalState>(replay, event, state)) {}
QuadFinal::~QuadFinal() = default;
uint64_t QuadFinal::submit() { return state_->submit(); }
Json QuadFinal::metadata() const { return state_->metadata(); }
const Json &QuadFinal::outputSignatures() const { return state_->outputSignatures(); }
} // namespace flora
