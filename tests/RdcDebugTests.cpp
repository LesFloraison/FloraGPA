#include "rdc/DebugTrace.h"
#include "rdc/Invocation.h"
#include <QFile>
#include <QtTest>
#include <cmath>
#include <cstdlib>
#include <limits>

// These tests create API containers locally and never load RenderDoc. The
// production executable uses allocation bridges to the selected replay DLL.
extern "C" void *__cdecl RENDERDOC_AllocArrayMem(uint64_t size) { return std::malloc(size_t(size)); }
extern "C" void __cdecl RENDERDOC_FreeArrayMem(void *memory) { std::free(memory); }

using namespace flora;
class RdcDebugTests : public QObject {
    Q_OBJECT
  private slots:
    void vertexInputSnapshots() {
        using Json = nlohmann::json;
        ActionDescription draw; draw.flags = ActionFlags::Drawcall | ActionFlags::Instanced;
        draw.numIndices = 4; draw.numInstances = 6; draw.vertexOffset = 2; draw.instanceOffset = 7;
        D3D12Pipe::InputAssembly ia; ia.vertexBuffers.resize(2); ia.layouts.resize(3);
        for (unsigned i = 0; i < 2; ++i) {
            uint64_t id = i + 1; memcpy(&ia.vertexBuffers[i].resourceId, &id, 8);
            ia.vertexBuffers[i].byteOffset = 1ULL << 33; ia.vertexBuffers[i].byteSize = 1024;
            ia.vertexBuffers[i].byteStride = i ? 4 : 16;
        }
        for (unsigned i = 0; i < 3; ++i) {
            auto &l = ia.layouts[i]; l.semanticName = "TEXCOORD"; l.semanticIndex = i;
            l.inputSlot = i == 2 ? 1 : 0; l.byteOffset = i == 1 ? D3D12Pipe::Layout::TightlyPacked : 0;
            l.format.type = ResourceFormatType::Regular; l.format.compByteWidth = 4; l.format.compCount = i == 2 ? 1 : 2;
            l.perInstance = i == 2; l.instanceDataStepRate = i == 2 ? 3 : 0;
        }
        bool truncated{}; unsigned reads{};
        const auto read = [&](ResourceId, uint64_t offset, uint64_t length) {
            ++reads;
            if (offset != (1ULL << 33) + 48 && offset != (1ULL << 33) + 56 && offset != (1ULL << 33) + 32)
                throw std::runtime_error("Wrong selected input address");
            bytebuf bytes; bytes.resize(size_t(length - (truncated ? 1 : 0)));
            for (auto &b : bytes) b = 127;
            return bytes;
        };
        auto snapshot = rdcVertexInputSnapshot(draw, ia, 1, 5, read);
        QCOMPARE(snapshot.at("status"), Json("read")); QCOMPARE(reads, 3u);
        QCOMPARE(snapshot.at("attributes")[1].at("byte_offset"), Json("8589934648"));
        QCOMPARE(snapshot.at("attributes")[2].at("element"), Json("8"));
        truncated = true; QCOMPARE(rdcVertexInputSnapshot(draw, ia, 1, 5, read).at("status"), Json("partial"));
        truncated = false; ia.vertexBuffers[0].byteSize = 55;
        QCOMPARE(rdcVertexInputSnapshot(draw, ia, 1, 5, read).at("attributes")[0].at("status"), Json("unavailable"));
        ia.vertexBuffers[0].byteOffset = UINT64_MAX - 3; ia.vertexBuffers[0].byteSize = 1024;
        QCOMPARE(rdcVertexInputSnapshot(draw, ia, 1, 5, read).at("status"), Json("partial"));
        QCOMPARE(rdcVertexInputSnapshot(draw, ia, 4, 5, read).at("status"), Json("unavailable"));
    }
    void vertexOutputLayout() {
        using Json = nlohmann::json;
        ShaderReflection shader; shader.stage = ShaderStage::Vertex;
        SigParameter uv; uv.semanticName = "TEXCOORD"; uv.semanticIndex = 2;
        uv.compCount = 2; uv.varType = VarType::Float; uv.regIndex = 0;
        SigParameter position; position.semanticName = "SV_Position"; position.systemValue = ShaderBuiltin::Position;
        position.compCount = 4; position.regIndex = 1;
        SigParameter id; id.semanticName = "ID"; id.compCount = 1; id.varType = VarType::UInt; id.regIndex = 2;
        shader.outputSignature = {uv, position, id};
        auto result = rdcVertexOutputLayout(shader, 28);
        QCOMPARE(result.at("status"), Json("mapped"));
        const auto &attrs = result.at("attributes");
        QCOMPARE(attrs[0].at("signature_index"), Json(1)); QCOMPARE(attrs[0].at("byte_offset"), Json(0));
        QCOMPARE(attrs[1].at("signature_index"), Json(0)); QCOMPARE(attrs[1].at("byte_offset"), Json(16));
        QCOMPARE(attrs[2].at("byte_offset"), Json(24));
        QCOMPARE(attrs[2].at("format").at("compType"), Json(uint32_t(CompType::UInt)));
        QCOMPARE(rdcVertexOutputLayout(shader, 32).at("status"), Json("unavailable"));
        shader.outputSignature[2].varType = VarType::Double;
        QVERIFY(!rdcVertexOutputLayout(shader, 28).at("attributes")[2].at("supported").get<bool>());
        shader.outputSignature[2].stream = 1;
        QCOMPARE(rdcVertexOutputLayout(shader, 28).at("status"), Json("unavailable"));
        shader.stage = ShaderStage::Geometry;
        QCOMPARE(rdcVertexOutputLayout(shader, 28).at("status"), Json("unavailable"));
        shader.stage = ShaderStage::Vertex; shader.outputSignature = {uv, position, id};
        ActionDescription draw; draw.flags = ActionFlags::Drawcall; draw.numIndices = 2; draw.numInstances = 1;
        MeshFormat mesh; uint64_t resource = 1; memcpy(&mesh.vertexResourceId, &resource, 8);
        mesh.numIndices = 2; mesh.unproject = true; mesh.vertexByteStride = 28; mesh.vertexByteSize = 56;
        mesh.format.type = ResourceFormatType::Regular; mesh.format.compType = CompType::Float;
        mesh.format.compCount = 4; mesh.format.compByteWidth = 4;
        bool truncated{};
        auto read = [&](ResourceId, uint64_t offset, uint64_t length) {
            if (offset != 28 || length != 28) throw std::runtime_error("Wrong selected output vertex range");
            uint32_t words[]{1,2,3,4,5,6,UINT32_MAX}; bytebuf bytes; bytes.resize(truncated ? 27 : 28);
            memcpy(bytes.data(), words, bytes.size()); return bytes;
        };
        auto reference = rdcVertexOutputReference(draw, mesh, shader, 1, 0, read);
        QCOMPARE(reference.at("status"), Json("read"));
        QCOMPARE(reference.at("outputs")[1].at("value").at("uintValue"), Json::array({5,6}));
        QCOMPARE(reference.at("outputs")[2].at("value").at("uintValue"), Json::array({UINT32_MAX}));
        QCOMPARE(reference.at("outputs")[2].at("interpretation"), Json("uint"));
        QCOMPARE(reference.at("outputs")[2].at("buffer_byte_offset"), Json("52"));
        truncated = true;
        QCOMPARE(rdcVertexOutputReference(draw, mesh, shader, 1, 0, read).at("status"), Json("unavailable"));
    }
    void vertexPositionAddressing() {
        using Json = nlohmann::json;
        ActionDescription draw; draw.flags = ActionFlags::Drawcall | ActionFlags::Indexed | ActionFlags::Instanced;
        draw.numIndices = 10; draw.numInstances = 2;
        MeshFormat mesh; uint64_t vb = 11, ib = 12;
        memcpy(&mesh.vertexResourceId, &vb, 8); memcpy(&mesh.indexResourceId, &ib, 8);
        mesh.unproject = true; mesh.numIndices = 10;
        mesh.format.type = ResourceFormatType::Regular; mesh.format.compCount = 4;
        mesh.format.compByteWidth = 4; mesh.format.compType = CompType::Float;
        mesh.indexByteStride = 2; mesh.indexByteOffset = (1ULL << 33);
        mesh.indexByteSize = mesh.indexByteOffset + 20;
        mesh.vertexByteOffset = (1ULL << 34); mesh.vertexByteStride = 112;
        mesh.vertexByteSize = 2 * mesh.vertexByteOffset;
        mesh.baseVertex = -2; // Returned index 4 maps to row 2, not invocation ordinal 7.
        unsigned reads{}; bool truncated = false;
        const auto read = [&](ResourceId resource, uint64_t offset, uint64_t length) {
            ++reads; bytebuf bytes;
            if (resource == mesh.indexResourceId) {
                if (offset != (1ULL << 33) + 14 || length != 2) throw std::runtime_error("Wrong remapped index read");
                uint16_t raw = 4; bytes.resize(truncated ? 1 : 2); memcpy(bytes.data(), &raw, bytes.size());
            } else {
                if (resource != mesh.vertexResourceId || offset != (1ULL << 34) + 224 || length != 16)
                    throw std::runtime_error("Wrong position read");
                const uint32_t values[]{1068914556,1067567186,1031109634,1085587053};
                bytes.resize(16); memcpy(bytes.data(), values, 16);
            }
            return bytes;
        };
        auto result = rdcVertexPositionReference(draw, mesh, 7, 1, read);
        QCOMPARE(result.at("status"), Json("read")); QCOMPARE(reads, 2u);
        QCOMPARE(result.at("post_vertex"), Json("2"));
        QCOMPARE(result.at("position_byte_offset"), Json("17179869408"));
        QCOMPARE(result.at("value").at("uintValue")[3], Json(1085587053));
        auto bad = mesh; bad.vertexByteSize = mesh.vertexByteOffset + 239;
        QCOMPARE(rdcVertexPositionReference(draw, bad, 7, 1, read).at("status"), Json("unavailable"));
        bad = mesh; bad.vertexByteOffset = UINT64_MAX - 15; bad.vertexByteSize = UINT64_MAX;
        QCOMPARE(rdcVertexPositionReference(draw, bad, 7, 1, read).at("status"), Json("unavailable"));
        bad = mesh; bad.baseVertex = -5;
        QCOMPARE(rdcVertexPositionReference(draw, bad, 7, 1, read).at("status"), Json("unavailable"));
        bad = mesh; bad.baseVertex = 2; bad.vertexByteStride = UINT32_MAX;
        QCOMPARE(rdcVertexPositionReference(draw, bad, 7, 1, read).at("status"), Json("unavailable"));
        truncated = true;
        QCOMPARE(rdcVertexPositionReference(draw, mesh, 7, 1, read).at("status"), Json("unavailable"));
        const auto count = reads;
        for (int kind = 0; kind < 5; ++kind) {
            bad = mesh;
            if (kind == 0) bad.unproject = false;
            if (kind == 1) bad.numIndices = 9;
            if (kind == 2) bad.indexByteStride = 0;
            if (kind == 3) bad.indexByteOffset = UINT64_MAX - 3;
            if (kind == 4) bad.format.compType = CompType::UInt;
            QCOMPARE(rdcVertexPositionReference(draw, bad, 7, 1, read).at("status"), Json("unavailable"));
        }
        QCOMPARE(rdcVertexPositionReference(draw, mesh, 7, 2, read).at("status"), Json("unavailable"));
        QCOMPARE(reads, count);
        draw.flags = ActionFlags::Drawcall | ActionFlags::Instanced; mesh.indexByteStride = 0;
        mesh.indexResourceId = {}; mesh.baseVertex = 0;
        QCOMPARE(rdcVertexPositionReference(draw, mesh, 2, 1, read).at("status"), Json("read"));
    }
    void vertexInvocationBounds() {
        using Json = nlohmann::json;
        ActionDescription draw; draw.flags = ActionFlags::Drawcall | ActionFlags::Indexed;
        draw.numIndices = 6; draw.numInstances = 2; draw.indexOffset = 4; draw.baseVertex = -3;
        draw.instanceOffset = 7;
        D3D12Pipe::InputAssembly ia; uint64_t resource = 42;
        memcpy(&ia.indexBuffer.resourceId, &resource, 8);
        ia.indexBuffer.byteOffset = 1ULL << 33; ia.indexBuffer.byteSize = 32; ia.indexBuffer.byteStride = 2;
        ia.topology = Topology::TriangleList;
        unsigned reads{}; uint32_t raw = 9;
        const auto read = [&](ResourceId id, uint64_t offset, uint64_t length) {
            ++reads;
            if (id != ia.indexBuffer.resourceId || offset != ia.indexBuffer.byteOffset + 12 || length != 2)
                throw std::runtime_error("Unexpected vertex index read");
            bytebuf bytes; bytes.resize(2); memcpy(bytes.data(), &raw, 2); return bytes;
        };
        auto result = rdcVertexInvocation(draw, ia, 2, 1, read);
        QCOMPARE(result.at("actual_vertex_index"), Json(6));
        QCOMPARE(result.at("index_byte_offset"), Json("8589934604"));
        QCOMPARE(result.at("raw_index"), Json(9));
        QCOMPARE(result.at("first_instance"), Json(7));
        const auto invalid = [&](const ActionDescription &action, const D3D12Pipe::InputAssembly &state,
                                 uint32_t vertex, uint32_t instance) {
            const auto before = reads;
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, rdcVertexInvocation(action, state, vertex, instance, read));
            QCOMPARE(reads, before); // Invalid request must not touch the index resource.
        };
        invalid(draw, ia, 6, 0); invalid(draw, ia, 0, 2);
        auto changed = draw; changed.numInstances = 0; invalid(changed, ia, 0, 0);
        changed = draw; changed.indexOffset = UINT32_MAX; invalid(changed, ia, 2, 0);
        auto bad = ia; bad.indexBuffer.byteSize = 13; invalid(draw, bad, 2, 0);
        bad = ia; bad.indexBuffer.byteOffset = UINT64_MAX - 12; invalid(draw, bad, 2, 0);
        bad = ia; bad.indexBuffer.byteStride = 1; invalid(draw, bad, 2, 0);
        bad = ia; bad.indexBuffer.resourceId = ResourceId::Null(); invalid(draw, bad, 2, 0);
        raw = 2;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, rdcVertexInvocation(draw, ia, 2, 0, read));
        raw = 0xffff; ia.indexStripCutValue = 0xffff;
        QCOMPARE(rdcVertexInvocation(draw, ia, 2, 0, read).at("actual_vertex_index"), Json(65532));
        ia.topology = Topology::TriangleStrip;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, rdcVertexInvocation(draw, ia, 2, 0, read));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, rdcVertexInvocation(draw, ia, 2, 0,
            [](ResourceId, uint64_t, uint64_t) { return bytebuf(); }));
        ia.indexBuffer.byteStride = 4; ia.indexStripCutValue = 0; draw.baseVertex = 1;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, rdcVertexInvocation(draw, ia, 2, 0,
            [](ResourceId, uint64_t, uint64_t) { bytebuf bytes; bytes.resize(4); memset(bytes.data(), 255, 4); return bytes; }));
        draw.flags = ActionFlags::Drawcall; draw.vertexOffset = 10;
        result = rdcVertexInvocation(draw, ia, 2, 1, {});
        QCOMPARE(result.at("actual_vertex_index"), Json(12));
        QVERIFY(!result.contains("raw_index"));
        draw.vertexOffset = UINT32_MAX;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, rdcVertexInvocation(draw, ia, 2, 0, {}));
        draw.flags = ActionFlags::Dispatch; invalid(draw, ia, 0, 0);
    }
    void computeInvocationBounds() {
        using Json = nlohmann::json;
        ActionDescription action;
        action.eventId = 90443; action.flags = ActionFlags::Dispatch | ActionFlags::Indirect;
        action.dispatchDimension = {17, 3, 2}; action.dispatchThreadsDimension = {8, 4, 2};
        ShaderReflection shader; shader.dispatchThreadsDimension = {8, 4, 2};
        auto result = rdcComputeInvocation(action, shader, {16, 2, 1}, {7, 3, 1});
        QCOMPARE(result.at("indirect"), Json(true));
        QCOMPARE(result.at("group_count"), Json::array({17,3,2}));
        QCOMPARE(result.at("dispatch_thread"), Json::array({135,11,3}));
        action.dispatchThreadsDimension = {0, 0, 0};
        result = rdcComputeInvocation(action, shader, {16, 2, 1}, {7, 3, 1});
        QCOMPARE(result.at("has_group_size_override"), Json(false));
        QCOMPARE(result.at("shader_group_size"), Json::array({8, 4, 2}));
        QCOMPARE(result.at("dispatch_thread"), Json::array({135, 11, 3}));
        for (size_t axis = 0; axis < 3; ++axis) {
            rdcfixedarray<uint32_t,3> group{0,0,0}, thread{0,0,0};
            group[axis] = action.dispatchDimension[axis];
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, rdcComputeInvocation(action, shader, group, thread));
            group[axis] = 0; thread[axis] = shader.dispatchThreadsDimension[axis];
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, rdcComputeInvocation(action, shader, group, thread));
            auto empty = action; empty.dispatchDimension[axis] = 0;
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, rdcComputeInvocation(empty, shader, {0,0,0}, {0,0,0}));
            auto unknown = shader; unknown.dispatchThreadsDimension[axis] = 0;
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, rdcComputeInvocation(action, unknown, {0,0,0}, {0,0,0}));
        }
        action.dispatchDimension = {UINT32_MAX,1,1};
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
            rdcComputeInvocation(action, shader, {UINT32_MAX-1,0,0}, {0,0,0}));
        action.flags = ActionFlags::Drawcall | ActionFlags::Indexed | ActionFlags::Indirect;
        action.numIndices = 6; action.numInstances = 2; action.indexOffset = 4;
        action.baseVertex = -3; action.instanceOffset = 5;
        result = rdcExecutedAction(action);
        QCOMPARE(result.at("base_vertex"), Json(-3));
        QCOMPARE(result.at("first_index"), Json(4));
        QCOMPARE(result.at("instance_count"), Json(2));
        QVERIFY(!result.contains("first_vertex"));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
            rdcComputeInvocation(action, shader, {0,0,0}, {0,0,0}));
    }
    void pixelOutputComparison() {
        using Json = nlohmann::json;
        ShaderReflection reflection;
        SigParameter signature;
        signature.systemValue = ShaderBuiltin::ColorOutput;
        signature.compCount = 4; signature.regChannelMask = 15;
        reflection.outputSignature.push_back(signature);
        ShaderDebugTrace trace;
        SourceVariableMapping mapping;
        mapping.name = "SV_Target0"; mapping.type = VarType::Float;
        mapping.rows = 1; mapping.columns = 4; mapping.signatureIndex = 0;
        mapping.offset = 0; mapping.undefinedValue = false;
        for (uint32_t i = 0; i < 4; ++i) {
            DebugVariableReference variable;
            variable.name = "_OUT.SV_Target0"; variable.type = DebugVariableType::Variable;
            variable.component = i; mapping.variables.push_back(variable);
        }
        trace.sourceVars.push_back(mapping);
        ShaderVariable output;
        output.name = "_OUT"; output.type = VarType::Struct;
        output.members.push_back(ShaderVariable("SV_Target0", 1.f, 2.f, 3.f, 0.f));
        auto steps = Json::array({{{"changes", Json::array({{{"before", rdcVariable({})},
                                                            {"after", rdcVariable(output)}}})}}});
        Json reference{{"status", "read"}, {"interpretation", "float"},
                       {"value", {{"uintValue", Json::array({1065353216u,1073741824u,1077936128u,0u})}}}};
        auto traceJson = rdcDebugTrace(trace);
        auto compare = [&] { return rdcComparePixelOutput(reflection, traceJson, steps, reference); };
        QCOMPARE(compare().at("status"), Json("exact_output_match"));
        // A one-bit difference must not be hidden behind a default tolerance.
        reference["value"]["uintValue"][1] = 1073741825u;
        QCOMPARE(compare().at("status"), Json("output_mismatch"));
        QCOMPARE(compare().at("mismatched_components"), Json::array({1}));
        reference["status"] = "unverified_replay_changed";
        QCOMPARE(compare().at("status"), Json("unavailable"));
        reference["status"] = "read";
        // Matching input signature indices must not become output mappings.
        auto input = traceJson["sourceVars"][0];
        for (auto &v : input["variables"]) v["type"] = uint32_t(DebugVariableType::Input);
        traceJson["sourceVars"].push_back(input);
        QCOMPARE(compare().at("status"), Json("output_mismatch"));
        traceJson["sourceVars"].push_back(traceJson["sourceVars"][0]);
        QCOMPARE(compare().at("status"), Json("unavailable"));
        traceJson["sourceVars"].erase(2);
        steps.push_back({{"changes", Json::array({{{"before", rdcVariable(output)},
                                                   {"after", rdcVariable({})}}})}});
        QCOMPARE(compare().at("status"), Json("unavailable"));
        steps.erase(1);
        reflection.outputSignature[0].regChannelMask = 10;
        QCOMPARE(compare().at("status"), Json("unavailable"));
        reflection.stage = ShaderStage::Vertex;
        reflection.outputSignature[0].systemValue = ShaderBuiltin::Position;
        reflection.outputSignature[0].regChannelMask = 15;
        reflection.outputSignature[0].regIndex = 3; // Position need not be register zero.
        auto position = [&] { return rdcCompareVertexPosition(reflection, traceJson, steps, reference); };
        QCOMPARE(position().at("status"), Json("output_mismatch"));
        reference["value"]["uintValue"][1] = 1073741824u;
        QCOMPARE(position().at("status"), Json("exact_output_match"));
        reflection.outputSignature[0].compCount = 3;
        QCOMPARE(position().at("status"), Json("unavailable"));
        reflection.outputSignature[0].compCount = 4;
        Json allReference{{"status", "read"}, {"outputs", Json::array({reference})}};
        allReference["outputs"][0].update({{"signature_index", 0}, {"semantic", "SV_Position"}, {"semantic_index", 0}});
        auto all = [&] { return rdcCompareVertexOutputs(reflection, traceJson, steps, allReference); };
        QCOMPARE(all().at("status"), Json("exact_output_match"));
        QCOMPARE(all().at("matched"), Json(1));
        allReference["outputs"][0]["value"]["uintValue"][0] = 1065353217;
        QCOMPARE(all().at("status"), Json("output_mismatch"));
        QCOMPARE(all().at("mismatched"), Json(1));
        allReference["outputs"][0]["status"] = "unavailable";
        QCOMPARE(all().at("status"), Json("partial_comparison"));
        QCOMPARE(all().at("unavailable"), Json(1));
        allReference["outputs"][0]["signature_index"] = 2;
        QCOMPARE(all().at("status"), Json("unavailable"));
        auto integers = reflection;
        integers.outputSignature[0].varType = VarType::UInt;
        integers.outputSignature[0].systemValue = ShaderBuiltin::Undefined;
        auto integerTrace = traceJson, integerSteps = steps;
        integerTrace["sourceVars"][0]["type"] = uint32_t(VarType::UInt);
        auto &integerOutput = integerSteps[0]["changes"][0]["after"]["members"][0];
        integerOutput["type"] = uint32_t(VarType::UInt);
        const auto words = Json::array({UINT32_MAX, 16777217u, 0x80000000u, 0u});
        for (size_t i = 0; i < 4; ++i) integerOutput["value"]["u32v"][i] = words[i];
        auto integerReference = allReference;
        integerReference["outputs"][0].update({{"signature_index", 0}, {"status", "read"},
            {"interpretation", "uint"}, {"value", {{"uintValue", words}}}});
        QCOMPARE(rdcCompareVertexOutputs(integers, integerTrace, integerSteps, integerReference).at("status"), Json("exact_output_match"));
        integers.outputSignature[0].varType = VarType::SInt;
        integerTrace["sourceVars"][0]["type"] = uint32_t(VarType::SInt);
        integerOutput["type"] = uint32_t(VarType::SInt);
        integerReference["outputs"][0]["interpretation"] = "sint";
        QCOMPARE(rdcCompareVertexOutputs(integers, integerTrace, integerSteps, integerReference).at("status"), Json("exact_output_match"));
        reflection.stage = ShaderStage::Geometry;
        QCOMPARE(position().at("status"), Json("unavailable"));
    }
    void hellbladePixelOutputEvidence() {
        const auto path = qEnvironmentVariable("FLORA_DX12_PIXEL_DEBUG_REPORT");
        if (path.isEmpty()) QSKIP("Set the saved Hellblade2 MainPS report for this CPU evidence check");
        QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly));
        using Json = nlohmann::json;
        const auto report = Json::parse(file.readAll().toStdString());
        QCOMPARE(report.at("execution"), Json("37003"));
        ShaderReflection reflection;
        SigParameter signature;
        signature.systemValue = ShaderBuiltin::ColorOutput;
        signature.compCount = 4; signature.regChannelMask = 15;
        reflection.outputSignature.push_back(signature);
        const auto comparison = rdcComparePixelOutput(reflection, report.at("trace"), report.at("steps"),
                                                      report.at("gpu_invocation_reference"));
        QCOMPARE(comparison.at("status"), Json("output_mismatch"));
        QCOMPARE(comparison.at("mismatched_components"), Json::array({0,1,2}));
        QCOMPARE(comparison.at("trace_bits"), Json::array({1026478034u,1028981137u,1034447827u,0u}));
        QCOMPARE(comparison.at("gpu_bits"), Json::array({1026307110u,1028810213u,1034362365u,0u}));
    }
    void typedValues() {
        ShaderVariable v;
        v.name = "wide";
        v.type = VarType::ULong;
        v.rows = 1;
        v.columns = 2;
        v.value.u64v[0] = UINT64_MAX;
        v.value.u64v[1] = (uint64_t(1) << 63) + 1;
        v.value.u64v[15] = UINT64_MAX - 1;
        const auto j = rdcVariable(v);
        QCOMPARE(j["value"]["u64v"][0].get<uint64_t>(), UINT64_MAX);
        QCOMPARE(j["value"]["u64v"][1].get<uint64_t>(), (uint64_t(1) << 63) + 1);
        QCOMPARE(j["value"]["s64v"][0].get<int64_t>(), int64_t(-1));
        QCOMPARE(j["value"]["s64v"][15].get<int64_t>(), int64_t(-2));
        QCOMPARE(j["value"]["s8v"][0].get<int>(), -1);
        QCOMPARE(j["value"]["u8v"][0].get<unsigned>(), 255u);
        QCOMPARE(j["value"]["s16v"][0].get<int>(), -1);
        QCOMPARE(j["value"]["u16v"][0].get<unsigned>(), 65535u);
        QCOMPARE(j["value"]["u32v"][0].get<uint32_t>(), UINT32_MAX);
        QCOMPARE(j["value"]["s32v"][0].get<int32_t>(), int32_t(-1));
        QCOMPARE(j["value"]["f64v"][0].get<std::string>(), std::string("nan"));
        for (const auto &field : j["value"].items())
            QCOMPARE(field.value().size(), size_t(16));
        QCOMPARE(nlohmann::json::parse(j.dump()), j);
    }
    void nonfiniteAndHalf() {
        ShaderVariable v;
        v.value.f32v[0] = std::numeric_limits<float>::infinity();
        v.value.f32v[1] = -std::numeric_limits<float>::infinity();
        v.value.f32v[2] = std::numeric_limits<float>::quiet_NaN();
        v.value.f32v[3] = -0.f;
        v.value.f32v[4] = std::numeric_limits<float>::denorm_min();
        auto j = rdcVariable(v)["value"]["f32v"];
        QCOMPARE(j[0].get<std::string>(), std::string("inf"));
        QCOMPARE(j[1].get<std::string>(), std::string("-inf"));
        QCOMPARE(j[2].get<std::string>(), std::string("nan"));
        QVERIFY(std::signbit(j[3].get<double>()));
        QCOMPARE(j[4].get<double>(), double(std::numeric_limits<float>::denorm_min()));
        const uint16_t bits[] = {0x0000, 0x8000, 0x0001, 0x03ff, 0x0400, 0x3c00,
                                 0xbc00, 0x7bff, 0x7c00, 0xfc00, 0x7e00, 0x7c01};
        for (size_t i = 0; i < std::size(bits); ++i)
            v.value.u16v[i] = bits[i];
        j = rdcVariable(v)["value"]["f16v"];
        QVERIFY(!std::signbit(j[0].get<double>()));
        QVERIFY(std::signbit(j[1].get<double>()));
        QCOMPARE(j[2].get<double>(), 0.000000059604644775390625);
        QCOMPARE(j[3].get<double>(), 0.000060975551605224609375);
        QCOMPARE(j[4].get<double>(), 0.00006103515625);
        QCOMPARE(j[5].get<double>(), 1.);
        QCOMPARE(j[6].get<double>(), -1.);
        QCOMPARE(j[7].get<double>(), 65504.);
        QCOMPARE(j[8].get<std::string>(), std::string("inf"));
        QCOMPARE(j[9].get<std::string>(), std::string("-inf"));
        QCOMPARE(j[10].get<std::string>(), std::string("nan"));
        QCOMPARE(j[11].get<std::string>(), std::string("nan"));
    }
    void nestedVariablesAndChanges() {
        ShaderVariable root;
        root.name = "constants";
        root.type = VarType::Struct;
        root.members.push_back(ShaderVariable("matrix", 1.f, 2.f, 3.f, 4.f));
        root.members[0].rows = 4;
        root.members[0].flags = ShaderVariableFlags::RowMajorMatrix;
        root.members[0].value.f32v[15] = 123.f;
        auto j = rdcVariable(root);
        QCOMPARE(j["members"][0]["value"]["f32v"][15].get<double>(), 123.);
        QCOMPARE(j["members"][0]["rows"].get<int>(), 4);
        ShaderDebugState s;
        s.callstack.push_back("main");
        s.callstack.push_back("nested");
        s.nextInstruction = 24;
        s.stepIndex = 100;
        ShaderVariableChange birth;
        birth.after = root;
        ShaderVariableChange death;
        death.before = root;
        s.changes.push_back(birth);
        s.changes.push_back(death);
        auto state = rdcDebugState(s);
        QCOMPARE(state["changes"][0]["after"], j);
        QCOMPARE(state["changes"][1]["before"], j);
        QCOMPARE(state["changes"][0]["before"]["name"].get<std::string>(), std::string());
        QCOMPARE(state["changes"][1]["after"]["name"].get<std::string>(), std::string());
        QCOMPARE(state["callstack"], nlohmann::json({"main", "nested"}));
        QCOMPARE(state["stepIndex"].get<int>(), 100);
        QCOMPARE(state["nextInstruction"].get<int>(), 24);
    }
    void sourceMaps() {
        ShaderDebugTrace trace;
        trace.stage = ShaderStage::Compute;
        InstructionSourceInfo instruction;
        instruction.instruction = 42;
        instruction.lineInfo.fileIndex = 2;
        instruction.lineInfo.lineStart = 4;
        instruction.lineInfo.lineEnd = 5;
        SourceVariableMapping mapping;
        mapping.name = "value[2].z";
        mapping.type = VarType::Double;
        mapping.offset = 80;
        mapping.undefinedValue = true;
        mapping.variables.push_back(DebugVariableReference(DebugVariableType::Variable, "r9", 2));
        instruction.sourceVars.push_back(mapping);
        trace.instInfo.push_back(instruction);
        trace.sourceVars.push_back(mapping);
        SourceVariableMapping constant;
        constant.name = "constants.member";
        constant.type = VarType::Float;
        constant.rows = constant.columns = 1;
        constant.offset = 64;
        constant.variables.push_back(DebugVariableReference(DebugVariableType::Constant, "cb0[4]"));
        trace.sourceVars.push_back(constant);
        const auto j = rdcDebugTrace(trace);
        QVERIFY(j["sourceVars"][0]["offset"].is_null());
        QCOMPARE(j["sourceVars"][1]["offset"].get<int>(), 64);
        QCOMPARE(j["stage"].get<unsigned>(), unsigned(ShaderStage::Compute));
        QCOMPARE(j["instInfo"][0]["sourceVars"][0]["offset"].get<int>(), 80);
        QCOMPARE(j["instInfo"][0]["sourceVars"][0]["variables"][0]["component"].get<int>(), 2);
        QVERIFY(j["instInfo"][0]["sourceVars"][0]["undefinedValue"].get<bool>());
        QVERIFY(!j.contains("debugger"));
    }
};
QTEST_GUILESS_MAIN(RdcDebugTests)
#include "RdcDebugTests.moc"
