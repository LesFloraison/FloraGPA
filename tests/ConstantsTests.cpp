#include "application/Constants.h"
#include "application/ShaderInspector.h"
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QtTest>
#include <cmath>
#include <d3dcompiler.h>
using namespace flora;
using Json = nlohmann::json;
namespace {
Json type(unsigned cls = 0, unsigned base = 3, unsigned rows = 1, unsigned cols = 1, unsigned elements = 0) {
    return {{"class_id", cls}, {"base_type", base},    {"rows", rows},
            {"columns", cols}, {"elements", elements}, {"members", 0}};
}
Json variable(Json t, unsigned size, unsigned offset = 0) {
    return {{"name", "v"}, {"type", t}, {"offset", offset}, {"size", size}};
}
void write(const QString &path, Bytes bytes) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly) ||
        f.write(reinterpret_cast<const char *>(bytes.data()), bytes.size()) != qint64(bytes.size()))
        throw std::runtime_error("Cannot write constant evidence");
}
} // namespace
class ConstantsTests final : public QObject {
    Q_OBJECT
  private slots:
    void matricesAndPadding() {
        std::vector<uint8_t> bytes(128, 0xa5);
        auto row = constantFields(variable(type(2, 3, 2, 3), 28), bytes).at(0);
        QCOMPARE(row["component_offsets"], Json({0, 4, 8, 16, 20, 24}));
        auto patches = constantPatches(row, Json({{1, 2, 3}, {4, 5, 6}}));
        QCOMPARE(patches.size(), size_t(2));
        QCOMPARE(patches[0].offset, uint64_t(0));
        QCOMPARE(patches[0].bytes.size(), size_t(12));
        QCOMPARE(patches[1].offset, uint64_t(16));
        for (auto &p : patches)
            std::copy(p.bytes.begin(), p.bytes.end(), bytes.begin() + p.offset);
        QCOMPARE(bytes[12], uint8_t(0xa5));
        QCOMPARE(bytes[28], uint8_t(0xa5));
        auto reread = constantFields(variable(type(2, 3, 2, 3), 28), bytes)[0];
        QCOMPARE(reread["value"], Json({{1, 2, 3}, {4, 5, 6}}));
        QVERIFY(constantPatches(reread, reread["value"]).empty());
        auto column = constantFields(variable(type(3, 3, 2, 3), 40), bytes)[0];
        QCOMPARE(column["component_offsets"], Json({0, 16, 32, 4, 20, 36}));
        auto cp = constantPatches(column, Json({{7, 8, 9}, {10, 11, 12}}));
        QCOMPARE(cp.size(), size_t(3));
        for (unsigned i = 0; i < 3; ++i) {
            QCOMPARE(cp[i].offset, uint64_t(i * 16));
            QCOMPARE(cp[i].bytes.size(), size_t(8));
        }
        auto doubleMatrix = constantFields(variable(type(2, 39, 2, 3), 56), bytes)[0];
        QCOMPARE(doubleMatrix["component_offsets"], Json({0, 8, 16, 32, 40, 48}));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, constantPatches(row, Json({1, 2, 3})));
    }
    void arraysStructuresAndRanges() {
        Json structure = type(5);
        structure["members"] = 2;
        structure["member_types"] =
            Json::array({{{"name", "a"}, {"offset", 0}, {"type", type(1, 3, 1, 3)}},
                         {{"name", "b"}, {"offset", 16}, {"type", type(0, 19, 1, 1, 2)}}});
        structure["elements"] = 2;
        std::vector<uint8_t> data(256);
        auto fields = constantFields(variable(structure, 84, 4), data, {2, 6});
        QCOMPARE(fields.size(), size_t(6));
        QCOMPARE(fields[0]["path"], Json("v[0].a"));
        QCOMPARE(fields[0]["buffer_offset"], Json(36));
        QCOMPARE(fields[2]["buffer_offset"], Json(68));
        QCOMPARE(fields[5]["path"], Json("v[1].b[1]"));
        QCOMPARE(fields[5]["buffer_offset"], Json(116));
        QCOMPARE(fields[5]["status"], Json("ready"));
        auto truncated = constantFields(variable(structure, 84, 4), data, {2, 5});
        QCOMPARE(truncated[5]["status"], Json("outside_bound_range"));
        QCOMPARE(truncated[3]["status"], Json("ready"));
        auto zero = constantFields(variable(type(), 4), data, {2, 0});
        QCOMPARE(zero[0]["status"], Json("outside_bound_range"));
        auto physical =
            constantFields(variable(type(1, 3, 1, 4), 16), Bytes(data).first(40), {2, std::nullopt});
        QCOMPARE(physical[0]["status"], Json("outside_bound_range"));
        auto bad = structure;
        bad["member_types"][1]["offset"] = 4;
        QCOMPARE(constantFields(variable(bad, 84), data)[0]["status"], Json("unsupported_layout"));
        QCOMPARE(constantFields(variable(type(0, 3, 1, 1, 5000), 65536), data)[0]["status"],
                 Json("unsupported_layout"));
        QCOMPARE(constantFields(variable(type(2, 3, 2, 3), 16), data)[0]["status"],
                 Json("unsupported_layout"));
    }
    void bitPreservationAndValidation() {
        std::vector<uint32_t> words{0x7fc01234, 0x80000000, 0x7f800000, 0xff800000};
        Bytes data(reinterpret_cast<const uint8_t *>(words.data()), 16);
        auto f = constantFields(variable(type(1, 3, 1, 4), 16), data)[0];
        auto qtText = QJsonDocument::fromJson(QByteArray::fromStdString(f.dump())).toJson();
        auto roundTrip = Json::parse(qtText.toStdString());
        auto recovered = constantValue(roundTrip);
        QVERIFY(std::signbit(recovered[1].get<double>()));
        QVERIFY2(constantPatches(roundTrip, recovered).empty(), qtText.constData());
        QCOMPARE(f["value"][0], Json("nan"));
        QVERIFY(constantPatches(f, f["value"]).empty());
        auto value = f["value"];
        value[0] = "bits:0x7fc05678";
        auto patch = constantPatches(f, value);
        QCOMPARE(patch.size(), size_t(1));
        QCOMPARE(patch[0].bytes, std::vector<uint8_t>({0x78, 0x56, 0xc0, 0x7f}));
        auto boolean = constantFields(variable(type(0, 1), 4), data)[0];
        QVERIFY(constantPatches(boolean, true).empty());
        QCOMPARE(constantPatches(boolean, false)[0].bytes, std::vector<uint8_t>(4));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, constantPatches(boolean, 1));
        auto integer = constantFields(variable(type(0, 2), 4), data)[0];
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, constantPatches(integer, 2147483648ULL));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, constantPatches(integer, -2147483649LL));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, constantPatches(integer, 1.0));
        auto u = constantFields(variable(type(0, 19), 4), data)[0];
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, constantPatches(u, -1));
        QCOMPARE(constantPatches(u, 4294967295ULL)[0].bytes, std::vector<uint8_t>(4, 255));
        auto scalar = constantFields(variable(type(), 4), data)[0];
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, constantPatches(scalar, 1e100));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, constantPatches(scalar, "bits:123"));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, constantPatches(scalar, "bits:zz000000"));
        auto d = constantFields(variable(type(0, 39), 8), data)[0];
        QCOMPARE(constantPatches(d, "bits:8000000000000000")[0].bytes,
                 std::vector<uint8_t>({0, 0, 0, 0, 0, 0, 0, 128}));
    }
    void compiledReflectionAndEvidence() {
        const char *source = R"(
struct Inner { float3 tint; bool visible; row_major float2x3 basis; };
struct Outer { Inner items[2]; uint2 ids; };
cbuffer Parameters:register(b2) {
    Outer scene[2]; column_major float3x2 camera; double3 weights[2];
    int counter; uint flags; float exceptional[3];
};
float4 main():SV_Target { return float4(scene[0].items[1].tint + camera[0][0] + (float)weights[1].x + counter + flags + exceptional[1], 1); }
)";
        Com<ID3DBlob> binary, errors;
        auto hr = D3DCompile(source, std::strlen(source), "constants.hlsl", nullptr, nullptr, "main",
                             "ps_5_0", D3DCOMPILE_SKIP_OPTIMIZATION, 0, &binary, &errors);
        QVERIFY2(SUCCEEDED(hr),
                 errors ? static_cast<const char *>(errors->GetBufferPointer()) : "Compile failed");
        Bytes code(static_cast<const uint8_t *>(binary->GetBufferPointer()), binary->GetBufferSize());
        auto metadata = inspectShader(code);
        auto cb = metadata["constant_buffers"][0];
        std::vector<uint8_t> raw(cb["size"].get<size_t>() + 64, 0xa5);
        const uint32_t words[] = {0x7fc01234, 0x80000000, 0x7f800000, 0xff800000,
                                  0x12345678, 0x3f800000, 0,          0xffffffff};
        for (size_t i = 0; i + 4 <= raw.size(); i += 4)
            std::memcpy(raw.data() + i, &words[(i / 4) % 8], 4);
        Json evidence = Json::array();
        for (auto &v : cb["variables"]) {
            Json var{{"name", v["name"]},
                     {"offset", v["offset"]},
                     {"size", v["size"]},
                     {"type", v["type_layout"]}};
            auto fields = constantFields(var, raw, {2, std::nullopt});
            for (auto &f : fields) {
                QCOMPARE(f["status"], Json("ready"));
                QVERIFY(constantPatches(f, f["value"]).empty());
            }
            Json edits = Json::array();
            for (auto &f : fields) {
                unsigned cls = f["class_id"], cols = f["columns"], rows = f["rows"], base = f["base_type"];
                Json value = base == 1 ? Json(false) : Json(13);
                if (cls == 1)
                    value = Json(std::vector<Json>(cols, value));
                else if (cls >= 2)
                    value = Json(std::vector<Json>(rows, Json(std::vector<Json>(cols, value))));
                Json patches = Json::array();
                for (auto &p : constantPatches(f, value))
                    patches.push_back(
                        {{"offset", p.offset},
                         {"hex", QByteArray(reinterpret_cast<const char *>(p.bytes.data()), p.bytes.size())
                                     .toHex()
                                     .toStdString()}});
                edits.push_back({{"field", f["path"]}, {"value", value}, {"patches", patches}});
            }
            evidence.push_back({{"variable", var}, {"fields", fields}, {"edits", edits}});
        }
        QCOMPARE(evidence[0]["fields"].size(), size_t(14));
        auto directory = qEnvironmentVariable("FLORA_CONSTANT_EVIDENCE_DIR");
        if (!directory.isEmpty()) {
            QVERIFY(QDir().mkpath(directory));
            write(directory + "/shader.dxbc", code);
            write(directory + "/buffer.bin", raw);
            auto text = evidence.dump(2);
            write(directory + "/constants.json",
                  {reinterpret_cast<const uint8_t *>(text.data()), text.size()});
        }
    }
};
QTEST_GUILESS_MAIN(ConstantsTests)
#include "ConstantsTests.moc"
