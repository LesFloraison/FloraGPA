#include "StateCapture.h"
#include "core/TextureEdits.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QtTest>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
using namespace flora;
using namespace flora::testing;
using Json = nlohmann::json;
namespace {
Resource texture(uint32_t format = 28, uint32_t samples = 1, uint32_t bind = 40) {
    Resource r;
    r.type = 0x85;
    r.desc = {4, 4, 1, 2, format, samples, 0, 0, bind, 0, 0};
    return r;
}
Json subJson(const TextureSubresource &s) {
    return {{"mip", s.mip},
            {"layer", s.layer},
            {"width", s.width},
            {"height", s.height},
            {"depth", s.depth},
            {"row_pitch", s.rowPitch},
            {"slice_pitch", s.slicePitch},
            {"offset", s.offset},
            {"size", s.size}};
}
Json encodingJson(const MsaaEditEncoding &e) {
    return {{"storage", e.storage},
            {"access", e.access},
            {"display", e.display},
            {"integer", e.integerBits},
            {"depth", e.depth ? Json(*e.depth) : Json(nullptr)}};
}
uint32_t uintValue(const Json &j) {
    if ((!j.is_number_integer() && !j.is_number_unsigned()) ||
        (j.is_number_integer() && !j.is_number_unsigned() && j.get<int64_t>() < 0) ||
        j.get<uint64_t>() > UINT32_MAX)
        throw std::runtime_error("Expected uint32 integer");
    return j.get<uint32_t>();
}
std::optional<uint32_t> optionalUint(const Json &j, const char *key) {
    return j.contains(key) && !j.at(key).is_null() ? std::optional(uintValue(j.at(key))) : std::nullopt;
}
} // namespace
class TextureEditTests : public QObject {
    Q_OBJECT
  private slots:
    void layouts() {
        auto r = texture();
        r.desc[2] = 3;
        auto subs = textureSubresources(r);
        QCOMPARE(subs.size(), size_t(6));
        QCOMPARE(subs[3].offset, uint64_t(84));
        QCOMPARE(subs[5].size, uint64_t(4));
        QCOMPARE(subs[5].index, 5u);
        r.type = 0x86;
        r.desc = {4, 4, 4, 3, 28, 0, 40, 0, 0};
        subs = textureSubresources(r);
        QCOMPARE(subs[1].offset, uint64_t(256));
        QCOMPARE(subs[1].size, uint64_t(32));
        QCOMPARE(subs[1].depth, 2u);
        r = texture(71);
        r.desc[0] = 5;
        r.desc[1] = 7;
        QCOMPARE(textureSubresources(r)[0].size, uint64_t(32));
        r = texture(104);
        QCOMPARE(textureSubresources(r)[0].size, uint64_t(48));
        r.desc[10] = 4;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, textureSubresources(r));
        r = texture();
        r.desc[2] = 0;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, textureSubresources(r));
        r.desc[2] = 33;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, textureSubresources(r));
        r = texture();
        r.desc[3] = UINT32_MAX;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, textureSubresources(r));
        r.type = 0x86;
        r.desc = {1, UINT32_MAX, UINT32_MAX, 1, 2, 0, 40, 0, 0};
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, textureSubresources(r));
    }
    void exactSampleBytes() {
        TexturePatch p{0, 1, 0, {}, std::vector<uint8_t>(128)};
        auto r = texture(20, 4, 64);
        validateTexturePatch(r, p);
        put(p.bytes, 0, -0.0f);
        validateTexturePatch(r, p);
        put(p.bytes, 0, 1u);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, validateTexturePatch(r, p));
        put(p.bytes, 0, 1.0f);
        p.bytes[5] = 1;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, validateTexturePatch(r, p));
        p.bytes[5] = 0;
        p.bytes[4] = 255;
        validateTexturePatch(r, p);
        r = texture(41, 4);
        p.bytes.resize(64);
        put(p.bytes, 0, 0x7fc01234u);
        validateTexturePatch(r, p); // Integer-family transfer preserves NaN payloads.
        p.sample.reset();
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, validateTexturePatch(r, p));
        p.sample = 4;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, validateTexturePatch(r, p));
        p.sample = 0;
        p.typedFormat = 28;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, validateTexturePatch(r, p));
        r = texture(26, 4);
        p.typedFormat.reset();
        p.bytes.assign(64, 0);
        put(p.bytes, 0, 0x7c0u);
        validateTexturePatch(r, p); // Positive infinity is supported.
        put(p.bytes, 0, 0x7c1u);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, validateTexturePatch(r, p));
        r = texture(88, 4);
        p.bytes.assign(64, 0);
        p.bytes[3] = 255;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, validateTexturePatch(r, p));
        p.bytes[3] = 0;
        validateTexturePatch(r, p);
        r = texture();
        p.sample.reset();
        p.typedFormat = 0;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, validateTexturePatch(r, p));
    }
    void bindingScope() {
        QTemporaryDir dir;
        auto c = stateCapture();
        auto path = dir.path() + "/binding.gpa_frame";
        c.save(path);
        Frame frame(path.toStdWString());
        State s{};
        Event e{};
        e.type = 0x37;
        s.stages[4].srv[0] = 6;
        s.stages[0].srv[3] = 6;
        QCOMPARE(textureInputViews(frame, e, s, 5), std::vector<Id>{6});
        TexturePatch p;
        p.mip = 1;
        p.layer = 1;
        validateTextureBinding(frame, e, s, 5, p, false); // Input resource need not expose the edited mip.
        e.type = 0x35;
        QVERIFY(textureInputViews(frame, e, s, 5).empty());
        s.stages[5].srv[127] = 6;
        QCOMPARE(textureInputViews(frame, e, s, 5), std::vector<Id>{6});
        e.type = 0x37;
        s.rtv[0] = 7;
        s.rtCount = 1;
        s.omStart = 1;
        auto out = textureOutputBindings(frame, e, s, 5);
        QCOMPARE(out.size(), size_t(1));
        QVERIFY(out[0].contains(1, 1));
        QVERIFY(!out[0].contains(0, 1));
        validateTextureBinding(frame, e, s, 5, p, true);
        p.layer = 0;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, validateTextureBinding(frame, e, s, 5, p, true));
        s.rtv[0] = 27;
        out = textureOutputBindings(frame, e, s, 24);
        QVERIFY(out[0].contains(1, 0));
        QCOMPARE(out[0].depthSlices->at(1), 1u);
        s.rtv[0] = 0;
        s.dsv = 9;
        out = textureOutputBindings(frame, e, s, 5);
        QCOMPARE(out[0].dsvFlags, 1u);
        QCOMPARE(out[0].slot, -1);
        QVERIFY(out[0].contains(0, 0));
        s.rtCount = 65;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, textureOutputBindings(frame, e, s, 5));
    }
};
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (argc == 3 && std::string(argv[1]) == "--oracle") {
        std::ifstream input(argv[2]);
        Json request;
        input >> request;
        Json result = Json::array();
        for (const auto &item : request.at("cases"))
            try {
                Resource r;
                r.type = item.at("type").get<uint16_t>();
                r.desc = item.at("desc").get<std::vector<uint32_t>>();
                Json value;
                if (item.at("kind") == "layout") {
                    value = Json::array();
                    for (const auto &sub : textureSubresources(r))
                        value.push_back(subJson(sub));
                } else {
                    TexturePatch p;
                    p.mip = uintValue(item.at("mip"));
                    p.layer = uintValue(item.at("layer"));
                    p.sample = optionalUint(item, "sample");
                    p.typedFormat = optionalUint(item, "typed_format");
                    const auto data =
                        QByteArray::fromHex(QByteArray::fromStdString(item.at("bytes").get<std::string>()));
                    p.bytes.assign(data.begin(), data.end());
                    validateTexturePatch(r, p);
                    value = textureInfo(r).samples > 1 ? encodingJson(msaaEditEncoding(r, p.typedFormat))
                                                       : Json(nullptr);
                }
                result.push_back({{"status", "ok"}, {"value", value}});
            } catch (const std::exception &e) {
                result.push_back({{"status", "error"}, {"error", e.what()}});
            }
        std::cout << result.dump();
        return 0;
    }
    TextureEditTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "TextureEditTests.moc"
