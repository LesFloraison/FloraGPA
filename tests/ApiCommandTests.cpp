#include "SyntheticCapture.h"
#include "application/ApiCommands.h"
#include <QTemporaryDir>
#include <QtTest>
using namespace flora;
using namespace flora::testing;
using Json = nlohmann::json;
namespace {
using Raw = std::vector<uint8_t>;
template <class... T> Raw pack(T... values) {
    Raw raw;
    (append(raw, values), ...);
    return raw;
}
Raw head(Id owner = 1) { return pack(Id(0), owner); }
void extend(Raw &a, const Raw &b) { a.insert(a.end(), b.begin(), b.end()); }
Capture apiCapture() {
    Capture c;
    c.add(1, 5, 0x99, pack(Id(0), Id(0), 0u, 0u));
    c.buffer(2, 3, 8, 0, {1, 2, 3, 4});
    c.uav(4, 2, 0);
    // A mipmapped texture array and its SRV first-slice selection.
    c.add(5, 5, 0x85, pack(Id(0), Id(0), 16u, 16u, 4u, 3u, 28u, 1u, 0u, 0u, 8u, 0u, 0u, Id(0)));
    c.add(6, 5, 0x8c, pack(Id(0), Id(0), Id(5), 28u, 5u, 2u, 1u, 1u, 1u));
    State s{};
    s.soCounts[0] = 19;
    c.add(7, 3, 3, snapshot(s));
    c.add(8, 5, 0x96, pack(Id(0), Id(0), 5u, 0u));
    c.add(9, 5, 0x91, pack(Id(0), Id(0), Id(0), Id(0), Id(0), Id(10), Id(0)));
    c.add(10, 9, 0x85, pack(0u, 1u, 16u, 3u));
    s.stages[3].shader = 9;
    s.soCounts[3] = 22;
    c.add(11, 3, 3, snapshot(s));
    c.add(12, 9, 0x85, pack(0u, 1u, 16u, UINT32_MAX));
    c.add(13, 5, 0x91, pack(Id(0), Id(0), Id(0), Id(0), Id(0), Id(12), Id(0)));
    s.stages[3].shader = 13;
    c.add(14, 3, 3, snapshot(s));
    Id id = 100;
    std::vector<std::pair<uint16_t, Raw>> samples;
    auto sample = [&](uint16_t type, Raw tail = Raw{}, Id owner = 1) {
        auto raw = head(owner);
        extend(raw, tail);
        samples.emplace_back(type, raw);
    };
    for (uint16_t t : {0x249, 0x24f, 0x34e5, 0x34e6}) {
        auto tail = pack(1u, 2u, uint8_t(1), Id(2), Id(0));
        if (t == 0x24f)
            extend(tail, pack(uint8_t(1), 0u, 16u, uint8_t(1), 4u, 8u));
        sample(t, tail);
        auto absent = pack(1u, UINT32_MAX, uint8_t(0));
        if (t == 0x24f)
            extend(absent, pack(uint8_t(0), uint8_t(0)));
        sample(t, absent);
    }
    sample(0x34e7, pack(Id(20), 2u, uint8_t(1), Id(21), Id(22)));
    sample(0x34f0, pack(2u, 1u, uint8_t(1), Id(2), uint8_t(1), 16u, uint8_t(1), 4u));
    for (uint16_t t : {0x34ef, 0x3509, 0x241, 0x243, 0x245})
        sample(t, pack(Id(6)));
    sample(0x34f1, pack(Id(2), 42u, 4u));
    sample(0x34f6, pack(4u));
    sample(0x359d, pack(Id(0)));
    sample(0x359d, pack(Id(UINT64_MAX)));
    sample(0x34ed, pack(Id(5), 5u));
    sample(0x248, pack(Id(8), 1u));
    sample(0x3502, pack(Id(9), 3u));
    sample(0x34ff, pack(1u, uint8_t(1), Id(4), Id(0)));
    sample(0x3500, pack(1u, uint8_t(1), Id(4), Id(0), 1u, 1u, uint8_t(1), Id(4), uint8_t(1), UINT32_MAX));
    sample(0x3501, pack(Id(0), uint8_t(1), -0.0f, 1.0f, 2.0f, 3.0f, UINT32_MAX));
    sample(0x350a, pack(1u, uint8_t(1), 0.f, 1.f, 16.f, 32.f, 0.f, 1.f));
    sample(0x350b, pack(1u, uint8_t(1), -1, -2, 16, 32));
    for (uint16_t t : {0x30bc, 0x31bc, 0x3325, 0x33eb, 0x3503})
        sample(t, pack(1u, uint8_t(1), Id(2), uint8_t(0)));
    for (uint16_t t : {0x25e, 0x3522})
        sample(t, pack(0u, 1u, uint8_t(1), Id(4), uint8_t(1), 9u));
    for (uint16_t t : {0x246, 0x34ec})
        sample(t, pack(int32_t(0), Id(5), 5u, 1u, 0u, Id(3)));
    sample(0x247, pack(Id(5), 5u, uint8_t(1), 0u, 0u, 0u, 8u, 8u, 1u, Id(3), 32u, 256u));
    sample(0x3e, pack(Id(2), Id(2)));
    sample(0x3f, pack(Id(2), 4u, Id(4)));
    sample(0x40, pack(Id(5), 0u, 1u, 2u, 0u, Id(5), 1u, uint8_t(0)));
    sample(0x42, pack(Id(5), 0u, Id(5), 1u, 28u));
    sample(0x31, pack(Id(6), 3u, 1.f, uint8_t(255)));
    for (uint16_t t : {0x32, 0x33, 0x34})
        sample(t, pack(Id(4), uint8_t(1), 0x7fc01234u, 0x80000000u, 0x7f800000u, 0xff800000u));
    for (uint16_t t : {0x242, 0x244})
        sample(t);
    for (uint16_t t : {0x3013, 0x3014, 0x3250, 0x3251, 0x3576, 0x3577, 0x304c, 0x304d, 0x4029, 0x402a, 0x3279,
                       0x327a, 0x327e})
        sample(t, pack(3u));
    for (uint16_t t : {0x3019, 0x3146, 0x313b, 0x300e})
        sample(t, pack(uint8_t(1), 3u));
    for (uint16_t t : {0x3012, 0x324f, 0x3256, 0x3575, 0x304b, 0x3278})
        sample(t, pack(0, Id(0x1122334455667788), Id(0x99aabbccddeeff00), Id(0xfffffffffffffff0)));
    sample(0x3257, pack(0, 1u, 0u));
    for (uint16_t t : {0x3074, 0x3235, 0x33a8, 0x3471, 0x34b2, 0x358d})
        sample(t, pack(0, uint8_t(1), 2u, 0u, Id(0)));
    for (uint16_t t : {0x3495, 0x34d6, 0x35b1})
        sample(t, pack(-1));
    for (uint16_t t : {0x30b4, 0x31b4, 0x331d, 0x33e3, 0x34fb})
        sample(t, pack(0, Id(8), uint8_t(1), 1u, 4u, 0u));
    sample(0x3151, pack(8u), 30);
    sample(0x3152, pack(uint8_t(1), 2u, 0u), 30);
    sample(0x34fb, pack(0, Id(30), uint8_t(1), 0xffffffffu, 8u, 0u));
    sample(0x3017, pack(0, Id(11), Id(12), 4u, Id(0x7ffffff01234)));
    sample(0x3597, pack(0, Id(11), Id(12), uint8_t(1), 4u, Id(0x7ffffff01234)));
    sample(0x302e, pack(uint8_t(1), 28u, 5u, 1u, 1u, 1u, 1u));
    sample(0x3578,
           pack(0, uint8_t(1), 16u, 0u, 8u, 0u, 0u, 0u, uint8_t(1), Id(0x7ffffff01234), 0u, 0u, Id(2)));
    sample(0x3261, pack(0, uint8_t(1), 1280u, 720u, 60u, 1u, 28u, 0u, 0u, 1u, 0u, 32u, 2u, 0u, Id(0x11223344),
                        1u, 0u, 0u, 0u));
    for (uint16_t t : {0x30ea, 0x31ea, 0x3353, 0x3419, 0x3531})
        sample(t, pack(uint8_t(1), 4u));
    sample(0x3528, pack(Id(99), uint8_t(1), 1u, uint8_t(1), Id(1000)));
    sample(0x3537, pack(1u, uint8_t(1), Id(6), Id(0)));
    sample(0x353d, pack(uint8_t(1), 1u, uint8_t(1), 0.f, 0.f, 100.f, 200.f, 0.f, 1.f));
    sample(0x327b, pack(0, 8u, uint16_t('A'), uint16_t(0xd800), uint16_t('\n'), uint16_t(0)));
    sample(0x327c, pack(0));
    sample(0x327d, pack(0u));
    for (uint16_t t : {0x41, 0x30d1})
        sample(t, pack(Id(0xffffffff00000001), 1u));
    for (uint16_t t : {0x3109, 0x3209, 0x3372, 0x3438, 0x3550})
        sample(t, pack(0, 1u, Id(0)));
    sample(0xffff, pack(Id(6)));                  // Unknown layout remains raw after its common prefix.
    sample(0x353d, pack(uint8_t(0), uint8_t(1))); // Present array without a returned count.
    sample(0x25e, pack(0u, 65537u, uint8_t(1)));  // Oversized array rejected before iteration.
    sample(0x32, pack(Id(4), uint8_t(2)));        // Invalid optional flag.
    for (auto &[type, raw] : samples)
        c.add(id++, 7, type, raw);
    // Every byte-prefix of each layout: no fabricated fields or lost undecoded suffixes.
    for (auto &[type, raw] : samples) {
        for (size_t n = 0; n < raw.size(); ++n)
            c.add(id++, 7, type, Raw(raw.begin(), raw.begin() + n));
        auto trailing = raw;
        trailing.push_back(0xaa);
        c.add(id++, 7, type, trailing);
    }
    for (uint16_t t : {0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d}) {
        auto raw = pack(Id(7), Id(0), Id(1));
        if (t == 0x36 || t == 0x3b || t == 0x3d)
            extend(raw, pack(Id(2), 4u));
        else if (t == 0x35)
            extend(raw, pack(1u, 2u, 3u));
        else if (t == 0x37)
            extend(raw, pack(3u, 0u));
        else if (t == 0x39)
            extend(raw, pack(3u, 0u, -2));
        else if (t == 0x3a)
            extend(raw, pack(3u, 2u, 0u, -2, 0u));
        else if (t == 0x3c)
            extend(raw, pack(3u, 2u, 0u, 0u));
        c.add(id++, 7, t, raw);
    }
    c.add(id++, 7, 0x38, pack(Id(11), Id(0), Id(1)));
    c.add(id++, 7, 0x38, pack(Id(14), Id(0), Id(1)));
    return c;
}
} // namespace
class ApiCommandTests final : public QObject {
    Q_OBJECT
  private slots:
    void wireAndReferences() {
        QTemporaryDir dir;
        auto capture = apiCapture();
        capture.save(dir.path() + "/api.gpa_frame");
        Frame f((dir.path() + "/api.gpa_frame").toStdWString());
        auto rows = inspectCommands(f);
        QVERIFY(rows.size() > 3000);
        size_t valid = 0, invalid = 0;
        for (auto &r : rows) {
            valid += r["status"] == "decoded";
            invalid += r["status"] == "invalid";
            size_t end = 0;
            for (auto &field : r["fields"]) {
                QCOMPARE(field["offset"].get<size_t>(), end);
                end += field["size"].get<size_t>();
                QVERIFY(end <= r["wire_size"].get<size_t>());
            }
            if (r.contains("remaining_offset"))
                QCOMPARE(r["remaining_offset"].get<size_t>(), end);
        }
        QVERIFY(valid > 100);
        QVERIFY(invalid > 2000);
        auto binding = rows[0];
        QVERIFY(commandMatches(binding, "set decoded", 2));
        QVERIFY(!commandMatches(binding, "", 0));
        QVERIFY(!commandMatches(binding, "missing"));
        exportCommands(f, std::filesystem::path(dir.path().toStdWString()) / "filtered", "decoded", 2, true);
        QFile exported(dir.path() + "/filtered/commands.json");
        QVERIFY(exported.open(QIODevice::ReadOnly));
        auto saved = Json::parse(exported.readAll().toStdString());
        QVERIFY(!saved["commands"].empty());
        for (auto &row : saved["commands"]) {
            QVERIFY(row["type"] >= 0x31 && row["type"] <= 0x42);
            QVERIFY(commandMatches(row, "decoded", 2));
        }
        auto selected = commandResourceSelection(f, {{"id", 6}, {"resource", 5}});
        QCOMPARE(selected["mip"], Json(2));
        QCOMPARE(selected["layer"], Json(1));
        QCOMPARE(selected["typed_format"], Json(28));
        bool sawNegative = false, sawAnnotation = false, sawPartial = false;
        size_t immediateContextRecords = 0;
        for (auto &r : rows) {
            if (r["status"] != "decoded")
                continue;
            if (r["type"] == 0x359d) {
                QCOMPARE(r["name"], Json("Device5.GetImmediateContext"));
                QCOMPARE(r["wire_size"], Json(24));
                QCOMPARE(r["fields"].back()["name"], Json("returned_context"));
                QCOMPARE(r["fields"].back()["value"],
                         Json(immediateContextRecords == 0 ? Id(0) : Id(UINT64_MAX)));
                ++immediateContextRecords;
            }
            if (r["type"] == 0x39) {
                QCOMPARE(r["parameters"]["base_vertex"], Json(-2));
                sawNegative = true;
            }
            if (r["type"] == 0x327b) {
                QCOMPARE(r["annotation"]["utf16_valid"], Json(false));
                QCOMPARE(r["annotation"]["display_name"], Json("A\\x00\\xd8\\u000a"));
                sawAnnotation = true;
            }
            if (r.contains("query_result") && r["query_result"]["object"] == 30) {
                QCOMPARE(r["query_result"]["status"], Json("partial"));
                QCOMPARE(r["query_result"]["fields"][0]["known_low_value"], Json(UINT32_MAX));
                sawPartial = true;
            }
        }
        QVERIFY(sawNegative && sawAnnotation && sawPartial);
        QCOMPARE(immediateContextRecords, size_t(2));
        auto evidence = qEnvironmentVariable("FLORA_API_EVIDENCE_DIR");
        if (!evidence.isEmpty()) {
            QVERIFY(QDir().mkpath(evidence));
            capture.save(evidence + "/api.gpa_frame");
        }
    }
    void metadataOrder() {
        Capture c;
        Id n = 100;
        auto add = [&](uint16_t t, Id owner, Raw tail) {
            auto raw = head(owner);
            extend(raw, tail);
            c.add(n++, 7, t, raw);
        };
        add(0x34fb, 1, pack(0, Id(50), uint8_t(1), 1u, 4u, 0u));
        add(0x3152, 50, pack(uint8_t(1), 0u, 0u));
        add(0x34fb, 1, pack(0, Id(50), uint8_t(1), 1u, 4u, 0u));
        add(0x34fb, 1, pack(1, Id(50), uint8_t(1), 0xdeadbeefu, 4u, 0u));
        add(0x3152, 50, pack(uint8_t(1), 2u, 0u));
        add(0x34fb, 1, pack(0, Id(50), uint8_t(1), 1u, 4u, 0u));
        QTemporaryDir dir;
        c.save(dir.path() + "/query.gpa_frame");
        Frame f((dir.path() + "/query.gpa_frame").toStdWString());
        auto rows = inspectCommands(f);
        QCOMPARE(rows[0]["query_result"]["status"], Json("unknown_type"));
        QVERIFY(rows[0]["query_result"]["metadata"].empty());
        QCOMPARE(rows[2]["query_result"]["status"], Json("complete"));
        QCOMPARE(rows[2]["query_result"]["fields"][0]["value"], Json(true));
        QCOMPARE(rows[3]["query_result"]["status"], Json("not_ready"));
        QVERIFY(rows[3]["query_result"]["fields"].empty());
        QCOMPARE(rows[5]["query_result"]["status"], Json("metadata_conflict"));
        auto evidence = qEnvironmentVariable("FLORA_API_EVIDENCE_DIR");
        if (!evidence.isEmpty())
            c.save(evidence + "/queries.gpa_frame");
    }
    void queryResults() {
        Capture capture;
        Id event = 1000, query = 100;
        std::map<Id, std::string> expected;
        auto add = [&](uint16_t type, Id owner, Raw tail) {
            auto raw = head(owner);
            extend(raw, tail);
            capture.add(event++, 7, type, raw);
        };
        auto result = [&](uint32_t type, uint32_t flags, int hr, uint32_t size, bool present,
                          const char *status) {
            ++query;
            add(0x3152, query, pack(uint8_t(1), type, flags));
            auto raw = pack(hr, query, uint8_t(present));
            if (present)
                extend(raw, pack(0xffffffffu));
            extend(raw, pack(size, 0u));
            expected[event] = status;
            add(0x34fb, 1, raw);
        };
        for (uint32_t kind = 0; kind < 16; ++kind) {
            auto size =
                kind == 0 || kind == 5 || kind == 7 || kind == 9 || kind == 11 || kind == 13 || kind == 15
                    ? 4u
                : kind == 1 || kind == 2 ? 8u
                : kind == 4              ? 88u
                                         : 16u;
            result(kind, 0, 0, size, true, size == 4 ? "complete" : "partial");
        }
        result(99, 0, 0, 4, true, "unsupported_type");
        result(0, 0, 1, 4, true, "not_ready");
        result(0, 0, -1, 4, true, "failed");
        result(0, 0, 2, 4, true, "unexpected_hresult");
        result(0, 0, 0, 0, false, "status_only");
        result(0, 0, 0, 4, false, "missing_bytes");
        result(0, 0, 0, 8, true, "size_mismatch");
        result(5, 1, 0, 4, true, "predicate_hint");
        result(0, 1, 0, 4, true, "metadata_conflict");
        expected[event] = "unresolved_identity";
        add(0x34fb, 1, pack(0, Id(0), uint8_t(1), 1u, 4u, 0u));
        expected[event] = "invalid_record";
        add(0x34fb, 1, pack(0));
        QTemporaryDir dir;
        capture.save(dir.path() + "/query-results.gpa_frame");
        Frame frame((dir.path() + "/query-results.gpa_frame").toStdWString());
        auto rows = inspectCommands(frame);
        for (auto &row : rows)
            if (row.contains("query_result")) {
                auto status = row["query_result"]["status"];
                QCOMPARE(status, Json(expected.at(row["id"].get<Id>())));
            }
        auto evidence = qEnvironmentVariable("FLORA_API_EVIDENCE_DIR");
        if (!evidence.isEmpty())
            capture.save(evidence + "/query-results.gpa_frame");
    }
    void unicodeExport() {
        QTemporaryDir dir;
        auto capture = apiCapture();
        auto path = dir.path() + QString::fromUtf8("/帧.gpa_frame");
        capture.save(path);
        Frame frame(path.toStdWString());
        exportCommands(frame, std::filesystem::path(dir.path().toStdWString()) / "export", "DrawAuto");
        QFile file(dir.path() + "/export/commands.json");
        QVERIFY(file.open(QIODevice::ReadOnly));
        auto report = Json::parse(file.readAll().toStdString());
        QCOMPARE(report["frame"], Json("帧.gpa_frame"));
        QCOMPARE(report["commands"].size(), size_t(3));
    }
};
QTEST_GUILESS_MAIN(ApiCommandTests)
#include "ApiCommandTests.moc"
