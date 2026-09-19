#include "application/Constants.h"
#include "application/Experiment.h"
#include "core/BufferBindings.h"
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>
#include <d3dcompiler.h>
using namespace flora;
namespace {
template <class T> void append(std::vector<uint8_t> &out, const T &value) {
    const auto *p = reinterpret_cast<const uint8_t *>(&value);
    out.insert(out.end(), p, p + sizeof value);
}
template <class T> void put(std::vector<uint8_t> &out, size_t pos, T value) {
    std::memcpy(out.data() + pos, &value, sizeof value);
}
std::vector<uint8_t> word(uint32_t value) {
    std::vector<uint8_t> out;
    append(out, value);
    return out;
}
struct Capture {
    std::vector<uint8_t> bytes = std::vector<uint8_t>(0x128);
    std::vector<Entry> entries;
    void add(Id id, uint8_t category, uint16_t type, std::vector<uint8_t> payload) {
        entries.push_back({id, bytes.size(), uint32_t(payload.size()), 0, category, type});
        bytes.insert(bytes.end(), payload.begin(), payload.end());
    }
    void buffer(Id id, Id dataId, UINT bind, UINT misc, std::array<uint32_t, 4> values) {
        std::vector<uint8_t> resource(16);
        append(resource, D3D11_BUFFER_DESC{16, D3D11_USAGE_DEFAULT, bind, 0, misc, misc ? 4u : 0u});
        append(resource, dataId);
        add(id, 5, 0x83, resource);
        auto data = word(16);
        append(data, values);
        add(dataId, 9, 1, data);
    }
    void uav(Id id, Id buffer, UINT flags) {
        std::vector<uint8_t> raw(16);
        append(raw, buffer);
        D3D11_UNORDERED_ACCESS_VIEW_DESC desc{};
        desc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        desc.Buffer.NumElements = 4;
        desc.Buffer.Flags = flags;
        append(raw, desc);
        add(id, 5, 0x8f, raw);
    }
    void save(const QString &path) {
        put(bytes, 0, 0x41504749u);
        put(bytes, 4, 0x128u);
        put(bytes, 8, 3u);
        put(bytes, 12, uint32_t(entries.size()));
        put(bytes, 0xf4, uint64_t(bytes.size()));
        std::memcpy(bytes.data() + 0x44, "DX11", 4);
        for (auto &e : entries) {
            append(bytes, e.id);
            append(bytes, e.offset);
            append(bytes, e.size);
            append(bytes, e.flags);
            append(bytes, e.category);
            append(bytes, e.type);
        }
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly) ||
            file.write(reinterpret_cast<const char *>(bytes.data()), bytes.size()) != qint64(bytes.size()))
            throw std::runtime_error("Cannot write test capture");
    }
};
std::vector<uint8_t> snapshot(const State &s) {
    std::vector<uint8_t> out(128);
    append(out, s.ib);
    append(out, s.ibFormat);
    append(out, s.ibOffset);
    append(out, s.layout);
    append(out, s.topology);
    append(out, s.vb);
    append(out, s.strides);
    append(out, s.offsets);
    auto stage = [&](const Stage &x) {
        append(out, x.cb);
        append(out, x.samplers);
        append(out, x.shader);
        append(out, x.srv);
        append(out, x.classes);
        append(out, x.classCount);
    };
    for (int i = 0; i < 4; ++i)
        stage(s.stages[i]);
    append(out, s.so);
    append(out, s.soOffsets);
    append(out, s.soCounts);
    append(out, s.soCount);
    append(out, s.scissors);
    append(out, s.rasterizer);
    append(out, s.viewports);
    stage(s.stages[4]);
    append(out, s.blend);
    append(out, s.blendFactor);
    append(out, s.sampleMask);
    append(out, s.depthState);
    append(out, s.stencilRef);
    append(out, s.rtv);
    append(out, s.omStart);
    append(out, s.dsv);
    append(out, s.omCounts);
    append(out, s.rtCount);
    stage(s.stages[5]);
    append(out, s.csStart);
    append(out, s.csCount);
    append(out, s.csUav);
    append(out, s.csCounts);
    append(out, s.predicate);
    append(out, s.predicateValue);
    append(out, 0u);
    append(out, s.omExtended);
    append(out, s.omExtendedCounts);
    append(out, s.csExtended);
    append(out, s.csExtendedCounts);
    if (out.size() != 22320)
        throw std::runtime_error("Bad synthetic state schema");
    return out;
}
Capture computeCapture() {
    Capture capture;
    capture.add(1, 5, 0x127, std::vector<uint8_t>(24));
    capture.buffer(2, 3, D3D11_BIND_CONSTANT_BUFFER, 0, {2, 0, 0, 0});
    capture.buffer(4, 5, D3D11_BIND_SHADER_RESOURCE, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, {7, 8, 9, 10});
    std::vector<uint8_t> srv(16);
    append(srv, Id(4));
    D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
    desc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    desc.Buffer.NumElements = 4;
    append(srv, desc);
    capture.add(6, 5, 0x8c, srv);
    capture.buffer(7, 8, D3D11_BIND_UNORDERED_ACCESS, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, {10, 0, 0, 0});
    capture.uav(9, 7, 0);
    capture.buffer(10, 11, D3D11_BIND_UNORDERED_ACCESS, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, {});
    capture.uav(12, 10, D3D11_BUFFER_UAV_FLAG_APPEND);
    capture.buffer(13, 14, 0, 0, {});
    const char *hlsl =
        "cbuffer Constants:register(b0){uint4 c;} StructuredBuffer<uint> input:register(t0);"
        "RWStructuredBuffer<uint> output:register(u0); AppendStructuredBuffer<uint> appended:register(u1);"
        "[numthreads(1,1,1)] void main(){output[0]+=input[0]+c.x; appended.Append(input[0]);}";
    Com<ID3DBlob> binary, diagnostics;
    check(D3DCompile(hlsl, std::strlen(hlsl), nullptr, nullptr, nullptr, "main", "cs_5_0", 0, 0, &binary,
                     &diagnostics),
          "Compile test shader");
    std::vector<uint8_t> shader(56);
    put(shader, 48, Id(16));
    capture.add(15, 5, 0x93, shader);
    std::vector<uint8_t> code;
    append(code, uint64_t(binary->GetBufferSize()));
    auto first = static_cast<const uint8_t *>(binary->GetBufferPointer());
    code.insert(code.end(), first, first + binary->GetBufferSize());
    append(code, Id(0));
    capture.add(16, 9, 0x81, code);
    State state{};
    state.topology = 1;
    state.sampleMask = UINT32_MAX;
    state.omStart = 8;
    state.stages[5].shader = 15;
    state.stages[5].cb[0] = 2;
    state.stages[5].srv[0] = 6;
    state.csUav[0] = 9;
    state.csUav[1] = 12;
    state.csCount = 2;
    capture.add(17, 3, 3, snapshot(state));
    // Reset the append counter once, before both dispatches.
    std::vector<uint8_t> setter(16);
    put(setter, 8, Id(1));
    append(setter, 0u);
    append(setter, 2u);
    append(setter, uint8_t(1));
    append(setter, Id(9));
    append(setter, Id(12));
    append(setter, uint8_t(1));
    append(setter, UINT32_MAX);
    append(setter, 0u);
    capture.add(90, 7, 0x25e, setter);
    std::vector<uint8_t> dispatch;
    append(dispatch, Id(17));
    append(dispatch, Id(0));
    append(dispatch, Id(1));
    append(dispatch, 1u);
    append(dispatch, 1u);
    append(dispatch, 1u);
    capture.add(100, 7, 0x35, dispatch);
    capture.add(110, 7, 0x35, dispatch);
    std::vector<uint8_t> counter(16);
    put(counter, 8, Id(1));
    append(counter, Id(13));
    append(counter, 0u);
    append(counter, Id(12));
    capture.add(111, 7, 0x3f, counter);
    return capture;
}
uint32_t firstWord(Replay &replay, Id id) {
    auto b = replay.readBuffer(id);
    return Reader(b).read<uint32_t>();
}
} // namespace
class BufferEditTests final : public QObject {
    Q_OBJECT
  private slots:
    void typedConstantTransaction() {
        QTemporaryDir dir;
        auto capture = computeCapture();
        capture.save(dir.path() + "/compute.gpa_frame");
        Frame frame((dir.path() + "/compute.gpa_frame").toStdWString());
        Experiment project(frame);
        ReplayOptions options;
        options.warp = true;
        options.until = 100;
        options.before = true;
        Replay replay(frame, options);
        replay.run();
        auto constants = inspectConstants(frame, replay, options, 100, 2, replay.readBuffer(2));
        QCOMPARE(constants.size(), size_t(1));
        QCOMPARE(constants[0]["stage"], nlohmann::json("cs"));
        auto field = constants[0]["variables"][0]["fields"][0];
        QCOMPARE(field["value"], nlohmann::json({2, 0, 0, 0}));
        auto patches = constantPatches(field, nlohmann::json({20, 0, 0, 9}));
        QCOMPARE(patches.size(), size_t(2));
        project.setBufferPatches(frame, 100, 2, patches, "Constant c");
        QCOMPARE(project.revision(), size_t(1));
        auto saved = project.document();
        QVERIFY_THROWS_EXCEPTION(
            std::runtime_error,
            project.setBufferPatches(frame, 100, 2, {{0, word(30)}, {16, word(40)}}, "Invalid transaction"));
        QCOMPARE(project.document(), saved);
        project.setBufferPatches(frame, 100, 2, {}, "No change");
        QCOMPARE(project.document(), saved);
        project.save(dir.path() + "/constants.json");
        Experiment loaded(frame);
        loaded.load(dir.path() + "/constants.json", frame);
        loaded.apply(frame, options);
        Replay edited(frame, options);
        edited.run();
        edited.inspectEventInputs(100, [&] {
            auto value = inspectConstants(frame, edited, options, 100, 2, edited.readBuffer(2));
            QCOMPARE(value[0]["variables"][0]["fields"][0]["value"], nlohmann::json({20, 0, 0, 9}));
        });
        QCOMPARE(firstWord(edited, 2), 2u);
        QVERIFY(loaded.undo());
        loaded.apply(frame, options);
        QVERIFY(options.buffers.empty());
        QVERIFY(loaded.redo());
        loaded.apply(frame, options);
        options.until = 0;
        options.before = false;
        Replay submitted(frame, options);
        submitted.run();
        QCOMPARE(firstWord(submitted, 7), 46u);
        QCOMPARE(firstWord(submitted, 2), 2u);
    }
    void scopesAndCounters() {
        QTemporaryDir dir;
        auto capture = computeCapture();
        capture.save(dir.path() + "/compute.gpa_frame");
        Frame frame((dir.path() + "/compute.gpa_frame").toStdWString());
        Experiment project(frame);
        project.setBuffer(frame, 100, 2, 0, word(20));
        project.setBuffer(frame, 100, 4, 0, word(70));
        project.setBuffer(frame, 100, 7, 0, word(100));
        project.setBuffer(frame, 110, 10, 12, word(999));
        ReplayOptions options;
        options.warp = true;
        project.apply(frame, options);
        Replay replay(frame, options);
        replay.run();
        QCOMPARE(firstWord(replay, 2), 2u);
        QCOMPARE(firstWord(replay, 4), 7u);
        QCOMPARE(firstWord(replay, 7), 199u);
        QCOMPARE(firstWord(replay, 13), 2u);
        auto appended = replay.readBuffer(10);
        Reader r(appended);
        QCOMPARE(r.read<uint32_t>(), 70u);
        QCOMPARE(r.read<uint32_t>(), 7u);
        QCOMPARE(r.read<uint32_t>(), 0u);
        QCOMPARE(r.read<uint32_t>(), 999u);
        // A second run on the same device must not inherit clones, patches or counters.
        replay.run();
        QCOMPARE(firstWord(replay, 7), 199u);
        QCOMPARE(firstWord(replay, 13), 2u);
        project.setEnabled(frame, 100, false);
        project.apply(frame, options);
        Replay disabled(frame, options);
        disabled.run();
        QCOMPARE(firstWord(disabled, 7), 19u);
        QCOMPARE(firstWord(disabled, 13), 1u);
        QVERIFY(project.undo());
        while (project.undo()) {
        }
        project.apply(frame, options);
        Replay original(frame, options);
        original.run();
        QCOMPARE(firstWord(original, 7), 28u);
        QCOMPARE(firstWord(original, 13), 2u);
        QVERIFY(options.buffers.empty());
    }
    void previewsRollbackAndValidation() {
        QTemporaryDir dir;
        auto capture = computeCapture();
        capture.save(dir.path() + "/compute.gpa_frame");
        Frame frame((dir.path() + "/compute.gpa_frame").toStdWString());
        Experiment project(frame);
        project.setBuffer(frame, 100, 2, 0, word(20));
        project.setBuffer(frame, 100, 4, 0, word(70));
        project.setBuffer(frame, 100, 7, 0, word(100));
        ReplayOptions options;
        options.warp = true;
        options.until = 100;
        options.before = true;
        project.apply(frame, options);
        Replay replay(frame, options);
        replay.run();
        QCOMPARE(firstWord(replay, 7), 10u);
        replay.inspectEventInputs(100, [&] {
            QCOMPARE(firstWord(replay, 2), 20u);
            QCOMPARE(firstWord(replay, 4), 70u);
            QCOMPARE(firstWord(replay, 7), 100u);
        });
        QCOMPARE(firstWord(replay, 2), 2u);
        QCOMPARE(firstWord(replay, 4), 7u);
        QCOMPARE(firstWord(replay, 7), 10u);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, replay.inspectEventInputs(100, [&] {
            throw std::runtime_error("abort preview");
        }));
        QCOMPARE(firstWord(replay, 7), 10u);
        auto saved = project.document();
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                 project.setBuffer(frame, 100, 13, 0, word(1))); // Unbound buffer.
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, project.setBuffer(frame, 90, 2, 0, word(1))); // Setter.
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, project.setBuffer(frame, 100, 2, UINT64_MAX, word(1)));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, project.setBuffer(frame, 100, 2, 15, word(1)));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, project.setBuffer(frame, 100, 2, 0, Bytes{}));
        QCOMPARE(project.document(), saved);
        project.setBuffer(frame, 100, 2, 0, word(21)); // Later overlapping edits win.
        project.save(dir.path() + "/edit.json");
        Experiment loaded(frame);
        loaded.load(dir.path() + "/edit.json", frame);
        loaded.apply(frame, options);
        Replay later(frame, options);
        later.run();
        later.inspectEventInputs(100, [&] { QCOMPARE(firstWord(later, 2), 21u); });
        QVERIFY(loaded.undo());
        loaded.apply(frame, options);
        Replay previous(frame, options);
        previous.run();
        previous.inspectEventInputs(100, [&] { QCOMPARE(firstWord(previous, 2), 20u); });
        QVERIFY(loaded.redo());
        QCOMPARE(loaded.document(), project.document());
    }
};
QTEST_GUILESS_MAIN(BufferEditTests)
#include "BufferEditTests.moc"
