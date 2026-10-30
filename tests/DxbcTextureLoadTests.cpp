#include "StateCapture.h"
#include "TextureOperationOracle.h"
#include "application/DxbcGpuOperations.h"
#include "application/DxbcInputSnapshot.h"
#include "application/DxbcTypedBuffer.h"
#include "application/HlslCompilation.h"
#include "core/Dxbc.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>
#include <d3d11sdklayers.h>
using namespace flora;
using namespace flora::dxbc_exec;
namespace {
using Words = std::vector<uint32_t>;
Words op(uint32_t code, std::initializer_list<Words> args = {}) {
    Words out{code};
    for (const auto &arg : args)
        out.insert(out.end(), arg.begin(), arg.end());
    out[0] |= uint32_t(out.size()) << 24;
    return out;
}
Program single(uint32_t stage, uint32_t dimension, uint32_t returns, std::array<int32_t, 3> offset,
               uint32_t mask = 15) {
    uint32_t control = 1;
    for (unsigned c = 0; c < 3; ++c)
        control |= (uint32_t(offset[c]) & 15) << (9 + c * 4);
    const auto target = (stage == 5 ? 0x100002u : 0x102002u) | (mask << 4);
    std::vector<Words> rows{op(89, {{0x208e46, 0, 1}}),
                            op(88 | (dimension << 11), {{0x1070f2, 0}, {returns}})};
    if (stage == 5) {
        rows.push_back(op(104, {{1}}));
        rows.push_back(op(155, {{1, 1, 1}}));
    } else
        rows.push_back(op(101, {{target, 0}}));
    if (mask != 15)
        rows.push_back(op(54, {{target | 0xf0, 0}, {0x4002, 7, 8, 9, 10}}));
    rows.push_back(op(45 | 0x80000000, {{control}, {target, 0}, {0x208e46, 0, 0}, {0x1071b6, 0}}));
    rows.push_back(op(62));
    return Program(makeDxbc({{0x58454853, writeDxbcProgram({{(stage << 16) | 0x50, 0}, rows})}}));
}
void replace(testing::Capture &capture, Id id, uint16_t category, uint16_t type, const Words &desc, Id data) {
    std::erase_if(capture.entries, [&](auto &entry) { return entry.id == id; });
    std::vector<uint8_t> bytes(16);
    for (auto word : desc)
        testing::append(bytes, word);
    testing::append(bytes, data);
    capture.add(id, category, type, bytes);
}
void blob(testing::Capture &capture, Id id, Bytes bytes) {
    std::erase_if(capture.entries, [&](auto &entry) { return entry.id == id; });
    auto payload = testing::word(UINT(bytes.size()));
    payload.insert(payload.end(), bytes.begin(), bytes.end());
    capture.add(id, 9, 1, payload);
}
ReplayOptions options(uint32_t vendor) {
    ReplayOptions o;
    o.warp = !vendor;
    if (vendor)
        o.vendor = vendor;
    o.debug = true;
    o.before = true;
    o.until = 100;
    return o;
}
void validation(Replay &replay) {
    Com<ID3D11InfoQueue> queue;
    check(replay.nativeDevice()->QueryInterface(IID_PPV_ARGS(&queue)),
          "Read texture load reference validation");
    for (UINT64 i = 0; i < queue->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
        SIZE_T size = 0;
        check(queue->GetMessage(i, nullptr, &size), "Read texture load validation size");
        std::vector<uint8_t> bytes(size);
        auto *message = reinterpret_cast<D3D11_MESSAGE *>(bytes.data());
        check(queue->GetMessage(i, message, &size), "Read texture load validation");
        if (message->Severity <= D3D11_MESSAGE_SEVERITY_ERROR)
            throw std::runtime_error(message->pDescription);
    }
}

} // namespace
class DxbcTextureLoadTests final : public QObject {
    Q_OBJECT
  private slots:
    void loadResultCache_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<uint32_t>("stage");
        for (auto vendor : {0u, 4318u, 32902u})
            for (auto stage : {0u, 1u, 5u})
                QTest::newRow(qPrintable(QString("%1-%2").arg(vendor).arg(stage))) << vendor << stage;
    }
    void loadResultCache() {
        QFETCH(uint32_t, vendor);
        QFETCH(uint32_t, stage);
        try {
            QTemporaryDir temporary;
            const auto path = temporary.filePath("cache.gpa_frame");
            testing::computeCapture(true).save(path);
            Frame frame(path.toStdWString());
            Replay replay(frame, options(vendor));
            unsigned cancelAfter = 0, checks = 0;
            auto gpu = std::make_shared<GpuOperations>(
                replay.nativeDevice(), !vendor, [&] { return cancelAfter && ++checks >= cancelAfter; });
            TextureInputs inputs;
            auto &texture = inputs.resources[4];
            texture.resource.id = 4;
            texture.resource.type = 0x85;
            texture.resource.desc = {4, 4, 2, 1, 16, 1, 0, 0, 8, 0, 0};
            texture.subresources = textureSubresources(texture.resource);
            texture.storage = Buffer::known(std::vector<uint8_t>(160));
            for (uint32_t i = 0; i < 40; ++i) {
                const uint32_t word = 0x3f800000 + i * 1024;
                std::memcpy(texture.storage.bytes.data() + i * 4, &word, 4);
            }
            auto &view = inputs.views[{7, {0}}];
            view.resource = 4;
            view.format = 16;
            view.dimension = 4;
            view.mipCount = 2;
            view.nativeDescriptor = {16, 4, 0, 2, 0, 0};
            auto address = Value::known({1, 1, 0, 0});
            std::array<uint32_t, 4> swizzle{0, 1, 2, 3};
            bool assisted = false;
            auto load = [&] {
                return inputs.load(stage, {7, {0}}, 3, 0x5555, address, swizzle, 15, {}, gpu.get(), assisted);
            };
            const auto reference = load();
            const auto first = gpu->operations();
            QCOMPARE(first, uint64_t(1));
            for (unsigned i = 0; i < 8; ++i) {
                address.bits[2] = i * 7919;
                QCOMPARE(load(), reference);
                QVERIFY(assisted);
            }
            QCOMPARE(gpu->operations(), first);
            texture.storage.valid[40] = 0;
            QCOMPARE(load().valid, uint8_t(14));
            QCOMPARE(gpu->operations(), first);
            texture.storage.valid[40] = 1;
            QCOMPARE(load(), reference);
            const auto original = texture.storage.bytes;
            texture.storage.bytes[40] ^= 1;
            const auto changed = load();
            QVERIFY(changed.bits != reference.bits);
            QCOMPARE(gpu->operations(), first + 1);
            texture.storage.bytes = original;
            QCOMPARE(load(), reference);
            QCOMPARE(gpu->operations(), first + 2);
            swizzle = {1, 0, 3, 2};
            const auto routed = load();
            QCOMPARE(routed.bits, (std::array<uint32_t, 4>{reference.bits[1], reference.bits[0],
                                                           reference.bits[3], reference.bits[2]}));
            QCOMPARE(gpu->operations(), first + 3);
            view.firstMip = 1;
            view.mipCount = 1;
            view.nativeDescriptor[2] = 1;
            view.nativeDescriptor[3] = 1;
            QVERIFY(load().bits != routed.bits);
            QCOMPARE(gpu->operations(), first + 4);
            // A hit still observes cancellation; a GPU failure poisons earlier cached results.
            cancelAfter = 1;
            checks = 0;
            QVERIFY_THROWS_EXCEPTION(OperationCancelled, load());
            cancelAfter = 0;
            load();
            QCOMPARE(gpu->operations(), first + 4);
            address.bits[0] = 0;
            cancelAfter = 3;
            checks = 0;
            QVERIFY_THROWS_EXCEPTION(OperationCancelled, load());
            cancelAfter = 0;
            address.bits[0] = 1;
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, load());
            gpu = std::make_shared<GpuOperations>(replay.nativeDevice(), !vendor);
            QCOMPARE(load().valid, uint8_t(15));
            if (!vendor && stage == 5) {
                const auto count = gpu->operations();
                for (unsigned i = 0; i <= 4096; ++i) {
                    address.bits[0] = i + 16;
                    QCOMPARE(load().valid, uint8_t(15));
                }
                address.bits[0] = 1;
                load();
                QCOMPARE(gpu->operations(), count + 4098);
                texture.resource.desc = {1024, 1025, 1, 1, 16, 1, 0, 0, 8, 0, 0};
                texture.subresources = textureSubresources(texture.resource);
                texture.storage = Buffer::known(std::vector<uint8_t>(1024 * 1025 * 8));
                view.firstMip = 0;
                view.nativeDescriptor[2] = 0;
                const auto large = gpu->operations();
                load();
                load();
                QCOMPARE(gpu->operations(), large + 2);
            }
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
    void minimumPrecision_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<uint32_t>("dimension");
        QTest::addColumn<uint32_t>("stage");
        QTest::addColumn<uint32_t>("precision");
        QTest::addColumn<bool>("uav");
        for (bool uav : {false, true})
            for (uint32_t vendor : {0u, 0x10deu, 0x8086u})
                for (uint32_t dimension : {2u, 3u, 5u, 7u, 8u})
                    for (uint32_t stage : {0u, 1u, 5u})
                        for (uint32_t precision : {1u, 2u, 4u, 5u}) {
                            if (uav && stage == 1)
                                continue;
                            QTest::newRow(qPrintable(QString("%1-%2-%3-%4%5")
                                                         .arg(vendor)
                                                         .arg(dimension)
                                                         .arg(stage)
                                                         .arg(precision)
                                                         .arg(uav ? "-uav" : "")))
                                << vendor << dimension << stage << precision << uav;
                        }
    }
    void minimumPrecision() {
        QFETCH(uint32_t, vendor);
        QFETCH(uint32_t, dimension);
        QFETCH(uint32_t, stage);
        QFETCH(uint32_t, precision);
        QFETCH(bool, uav);
        try {
            QTemporaryDir temporary;
            auto capture = testing::computeCapture(true);
            const auto framePath = temporary.filePath("device.gpa_frame");
            capture.save(framePath);
            Frame frame(framePath.toStdWString());
            Replay replay(frame, options(vendor));
            bool cancelled = false;
            auto gpu = std::make_shared<GpuOperations>(replay.nativeDevice(), vendor == 0,
                                                       [&] { return cancelled; });
            TextureInput texture;
            const uint32_t format = precision < 4 ? 2 : precision == 4 ? 4 : 3;
            const uint32_t returns = precision < 4 ? 5 : precision == 4 ? 3 : 4;
            const uint32_t layers = dimension == 7 || dimension == 8 ? 2 : 1;
            texture.resource.type = dimension == 2 || dimension == 7 ? 0x84 : dimension == 5 ? 0x86 : 0x85;
            if (texture.resource.type == 0x84)
                texture.resource.desc = {4, 2, layers, format, 0, 8, 0, 0};
            else if (texture.resource.type == 0x85)
                texture.resource.desc = {4, 4, 2, layers, format, 1, 0, 0, 8, 0, 0};
            else
                texture.resource.desc = {4, 4, 2, 2, format, 0, 8, 0, 0};
            if (uav)
                texture.resource.desc[texture.resource.type == 0x85   ? 8
                                      : texture.resource.type == 0x86 ? 6
                                                                      : 5] |= 128;
            texture.subresources = textureSubresources(texture.resource);
            const auto &last = texture.subresources.back();
            texture.storage = Buffer::known(std::vector<uint8_t>(last.offset + last.size));
            for (size_t i = 0; i < texture.storage.bytes.size() / 4; ++i) {
                const uint32_t bits = precision < 4    ? 0x3f800101u + uint32_t(i * 193)
                                      : precision == 4 ? uint32_t(-65553 - int32_t(i))
                                                       : 65553u + uint32_t(i);
                std::memcpy(texture.storage.bytes.data() + i * 4, &bits, 4);
            }
            TextureInputView view;
            view.resource = 1;
            view.format = format;
            view.mipCount = 2;
            view.layerCount = layers;
            view.dimension = dimension == 2   ? 2
                             : dimension == 3 ? 4
                             : dimension == 5 ? 8
                             : dimension == 7 ? 3
                                              : 5;
            view.nativeDescriptor = {format, view.dimension, 0, 2, 0, layers > 1 ? layers : 0};
            Inputs inputs;
            inputs.gpu = gpu;
            inputs.textures = std::make_shared<TextureInputs>();
            inputs.textures->resources[1] = texture;
            inputs.textures->views[{7, {3}}] = view;
            auto annotated = [](Words operand, uint32_t value) {
                if (value) {
                    operand[0] |= 0x80000000u;
                    operand.insert(operand.begin() + 1, 1u | value << 14);
                }
                return operand;
            };
            nlohmann::json records = nlohmann::json::array();
            for (uint32_t addressPrecision : {0u, 4u, 5u})
                for (uint32_t mode = 0; mode < 4; ++mode)
                    for (uint32_t direct = 0; direct < (stage == 5 ? 1u : 2u); ++direct)
                        for (bool literalAddress : {false, true}) {
                            if (uav) {
                                view.firstMip = mode == 2 ? 1 : 0;
                                view.mipCount = 1;
                                view.nativeDescriptor = {format, view.dimension, view.firstMip, 0,
                                                         layers > 1 ? layers : 0};
                                if (dimension == 5) {
                                    view.depthSlices = std::array<uint32_t, 2>{0, 2u >> view.firstMip};
                                    view.nativeDescriptor[4] = (*view.depthSlices)[1];
                                }
                                inputs.textures->views[{30, {1}}] = view;
                            }
                            const uint32_t mask = mode & 1 ? 9 : 15;
                            const std::array<uint32_t, 4> address{mode == 1   ? 65537u
                                                                  : mode == 3 ? UINT32_MAX
                                                                              : 1u,
                                                                  1, 1, mode == 2 ? 1u : 0u};
                            inputs.constantBuffers[2] = {Value::known(address)};
                            const std::array<uint32_t, 4> seed =
                                precision < 4
                                    ? std::array<uint32_t, 4>{0x3f000000, 0x3f800000, 0x3fc00000, 0x3fe00000}
                                    : std::array<uint32_t, 4>{17, 23, 31, 47};
                            const uint32_t targetKind = direct ? 2 : 0, targetIndex = direct ? 0 : 3;
                            const auto destination = [&](uint32_t kind, uint32_t index,
                                                         uint32_t selected = 15) {
                                return Words{0x100002u | kind << 12 | selected << 4, index};
                            };
                            std::vector<Words> rows{
                                op(106 | (1u << 16)), op(89, {{0x208e46, 2, 1}}),
                                op((uav ? 156u : 88u) | dimension << 11,
                                   {{uav ? 0x11e0f2u : 0x1070f2u, uav ? 1u : 3u}, {returns * 0x1111}}),
                                op(104, {{4}})};
                            if (stage == 5) {
                                rows.push_back(op(155, {{1, 1, 1}}));
                                rows.push_back(op(158, {{0x11e0f2, 3}, {16}}));
                            } else
                                rows.push_back(
                                    op(101, {annotated(destination(2, 0), direct ? precision : 0)}));
                            rows.push_back(op(54, {destination(targetKind, targetIndex),
                                                   {0x4002, seed[0], seed[1], seed[2], seed[3]}}));
                            rows.push_back(op(
                                (uav ? 163u : 45u) | 0x80000000u,
                                {{2u | (dimension << 6) | 0x80000000u, 3u | ((returns * 0x1111) << 6)},
                                 annotated(destination(targetKind, targetIndex, mask), precision),
                                 annotated(literalAddress
                                               ? Words{0x4002, address[0], address[1], address[2], address[3]}
                                               : Words{0x208e46, 2, 0},
                                           addressPrecision),
                                 {uav ? 0x11e1b6u : 0x1071b6u, uav ? 1u : 3u}}));
                            if (!direct) {
                                rows.push_back(op(54, {destination(stage == 5 ? 0 : 2, stage == 5 ? 2 : 0),
                                                       {0x4002, seed[0], seed[1], seed[2], seed[3]}}));
                                rows.push_back(
                                    op(54, {destination(stage == 5 ? 0 : 2, stage == 5 ? 2 : 0, mask),
                                            annotated({0x100e46, 3}, precision)}));
                            }
                            if (stage == 5)
                                rows.push_back(
                                    op(168, {{0x11e0f2, 3}, {0x4001, 0}, {0x4001, 0}, {0x100e46, 2}}));
                            rows.push_back(op(62));
                            const std::string type = direct ? precision == 1   ? "min16float"
                                                              : precision == 2 ? "min10float"
                                                              : precision == 4 ? "min16int"
                                                                               : "min16uint"
                                                            : "uint";
                            const auto signature =
                                compileHlsl(stage == 5 ? "[numthreads(1,1,1)]void main(){}"
                                                       : type + "4 main():" +
                                                             (stage == 0 ? "SV_Target0" : "TEXCOORD0") +
                                                             "{return 0;}",
                                            stage == 0   ? "ps_5_0"
                                            : stage == 1 ? "vs_5_0"
                                                         : "cs_5_0")
                                    .bytecode;
                            auto parts = readDxbcParts(signature);
                            std::erase_if(parts, [](const auto &part) {
                                return part.first == 0x52444853 || part.first == 0x58454853 ||
                                       part.first == 0x54415453;
                            });
                            const auto programBytes = writeDxbcProgram({{stage << 16 | 0x50, 0}, rows});
                            parts.emplace_back(0x58454853, programBytes);
                            const auto code = makeDxbc(parts);
                            const auto expected = testing::directTexture(
                                replay.nativeDevice(), code, stage, texture, view, {address},
                                direct ? precision : 0, nullptr, 1, 0, 0, uav);
                            const Program program(code);
                            Lane lane(program, inputs);
                            lane.step();
                            const auto step = lane.step();
                            QVERIFY(step.memoryGpuAssisted);
                            QCOMPARE(step.changes.size(), size_t(1));
                            QCOMPARE(step.changes[0].before, Value::known(seed));
                            const auto actual = lane.value({targetKind, {targetIndex}});
                            QCOMPARE(actual.bits, expected);
                            QCOMPARE(actual.valid, uint8_t(15));
                            auto &validity = inputs.textures->resources.at(1).storage.valid;
                            std::fill(validity.begin(), validity.end(), 0);
                            Lane unknown(program, inputs);
                            unknown.step();
                            unknown.step();
                            bool zero = true;
                            for (unsigned c = 0; c < 4; ++c)
                                if (mask & (1u << c))
                                    zero &= expected[c] == 0;
                            const auto missing = unknown.value({targetKind, {targetIndex}});
                            QCOMPARE(missing.bits, expected);
                            QCOMPARE(missing.valid, uint8_t(zero ? 15 : 15 ^ mask));
                            std::fill(validity.begin(), validity.end(), 1);
                            if (uav && mode == 0 && !direct && !literalAddress) {
                                cancelled = true;
                                Lane interrupted(program, inputs);
                                interrupted.step();
                                QVERIFY_THROWS_EXCEPTION(OperationCancelled, interrupted.step());
                                QCOMPARE(interrupted.value({targetKind, {targetIndex}}), Value::known(seed));
                                cancelled = false;
                                Lane recovered(program, inputs);
                                recovered.step();
                                recovered.step();
                                QCOMPARE(recovered.value({targetKind, {targetIndex}}), actual);
                            }
                            records.push_back(
                                {{"address_precision", addressPrecision},
                                 {"view_descriptor", view.nativeDescriptor},
                                 {"literal_address", literalAddress},
                                 {"cancellation_checked", uav && mode == 0 && !direct && !literalAddress},
                                 {"mode", mode},
                                 {"address", address},
                                 {"output_destination", bool(direct)},
                                 {"mask", mask},
                                 {"expected", expected},
                                 {"actual", actual.bits},
                                 {"valid", actual.valid},
                                 {"unknown_valid", missing.valid},
                                 {"code", QByteArray(reinterpret_cast<const char *>(code.data()),
                                                     qsizetype(code.size()))
                                              .toHex()
                                              .toStdString()}});
                        }
            QCOMPARE(inputs.textures->resources.at(1).storage.bytes, texture.storage.bytes);
            validation(replay);
            const nlohmann::json report{{"schema", 1},
                                        {"probe", "minimum_precision_texture_load"},
                                        {"vendor", vendor},
                                        {"dimension", dimension},
                                        {"stage", stage},
                                        {"uav", uav},
                                        {"precision", precision},
                                        {"format", format},
                                        {"helper", gpu->identity()},
                                        {"records", records},
                                        {"passed", true}};
            const auto root = qEnvironmentVariable("FLORA_TEXTURE_PRECISION_EVIDENCE");
            if (!root.isEmpty()) {
                QDir directory(root);
                QVERIFY(directory.mkpath("."));
                QFile file(directory.filePath(QString::fromLatin1(QTest::currentDataTag()) + ".json"));
                QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::NewOnly));
                const auto text = report.dump(2) + "\n";
                QCOMPARE(file.write(text.data(), qint64(text.size())), qint64(text.size()));
            }
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
    void hardware_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<uint32_t>("dimension");
        QTest::addColumn<uint32_t>("format");
        QTest::addColumn<unsigned>("mode");
        for (uint32_t vendor : {0u, 0x10deu, 0x8086u})
            for (uint32_t dimension : {2u, 3u, 5u, 7u, 8u})
                for (uint32_t format : {2u, 3u, 10u, 28u, 42u})
                    for (unsigned mode = 0; mode < 10; ++mode)
                        QTest::newRow(qPrintable(
                            QString("%1-%2-%3-%4").arg(vendor).arg(dimension).arg(format).arg(mode)))
                            << vendor << dimension << format << mode;
    }
    void hardware() {
        QFETCH(uint32_t, vendor);
        QFETCH(uint32_t, dimension);
        QFETCH(uint32_t, format);
        QFETCH(unsigned, mode);
        runHardware(vendor, dimension, format, mode);
    }
    void formats_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<uint32_t>("dimension");
        QTest::addColumn<uint32_t>("format");
        QTest::addColumn<unsigned>("mode");
        for (uint32_t vendor : {0u, 0x10deu, 0x8086u})
            for (uint32_t dimension : {3u, 8u})
                for (uint32_t format :
                     {21u, 22u, 29u, 46u, 47u, 65u, 67u, 71u, 72u, 74u, 75u, 77u, 78u, 80u,
                      81u, 83u, 84u, 85u, 86u, 87u, 88u, 91u, 93u, 95u, 96u, 98u, 99u, 115u})
                    for (unsigned mode : {0u, 10u, 11u, 12u})
                        QTest::newRow(qPrintable(
                            QString("%1-%2-%3-%4").arg(vendor).arg(dimension).arg(format).arg(mode)))
                            << vendor << dimension << format << mode;
    }
    void formats() {
        QFETCH(uint32_t, vendor);
        QFETCH(uint32_t, dimension);
        QFETCH(uint32_t, format);
        QFETCH(unsigned, mode);
        runHardware(vendor, dimension, format, mode);
    }

  private:
    void runHardware(uint32_t vendor, uint32_t dimension, uint32_t format, unsigned mode) {
        try {
            auto capture = testing::computeCapture(true);
            std::erase_if(capture.entries,
                          [](auto &e) { return e.id == 6 || e.id == 12 || e.id == 16 || e.id == 111; });
            Resource resource{};
            resource.type = dimension == 2 || dimension == 7 ? 0x84 : dimension == 5 ? 0x86 : 0x85;
            const uint32_t layers = dimension == 7 || dimension == 8 ? 3 : 1;
            if (resource.type == 0x84)
                resource.desc = {8, 3, layers, format, 0, 8, 0, 0x80};
            else if (resource.type == 0x86)
                resource.desc = {8, 8, 4, 3, format, 0, 8, 0, 0x80};
            else
                resource.desc = {8, 8, 3, layers, format, 1, 0, 0, 8, 0, 0x80};
            const bool extended = !typedBufferFormat(format);
            if (extended) {
                resource.desc[0] = resource.desc[1] = 16;
                resource.desc[2] = 5;
                if (format == 21 || format == 22)
                    resource.desc[4] = 19;
                if (format == 46 || format == 47)
                    resource.desc[4] = 44;
            }
            replace(capture, 4, 5, resource.type, resource.desc, 5);
            const auto subs = textureSubresources(resource);
            std::vector<uint8_t> bytes(subs.back().offset + subs.back().size);
            for (size_t i = 0; i < bytes.size(); ++i)
                bytes[i] = uint8_t(i * 37 + i / 13 + 7);
            const bool compressed = (format >= 71 && format <= 84) || (format >= 95 && format <= 99);
            if (compressed) {
                const auto blockSize = format <= 72 || format == 80 || format == 81 ? 8u : 16u;
                for (size_t i = 0; i < bytes.size(); i += blockSize) {
                    if (format == 95 || format == 96)
                        bytes[i] &= 0xfc; // Valid BC6H mode 00.
                    if (format == 98 || format == 99)
                        bytes[i] = 0x40; // Valid BC7 mode 6.
                }
            }
            blob(capture, 5, bytes);
            D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.Format = DXGI_FORMAT(format);
            srv.ViewDimension = D3D11_SRV_DIMENSION(dimension == 2   ? 2
                                                    : dimension == 3 ? 4
                                                    : dimension == 5 ? 8
                                                    : dimension == 7 ? 3
                                                                     : 5);
            srv.Texture2DArray = {1, extended ? 4u : 2u, layers > 1 ? 1u : 0u, layers > 1 ? 2u : 0u};
            capture.add(6, 5, 0x8c, testing::statePack(Id(0), Id(0), Id(4), srv));
            replace(capture, 10, 5, 0x83, {16, 0, 128, 0, 64, 16}, 11);
            blob(capture, 11, std::vector<uint8_t>(16));
            D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
            uav.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
            uav.Buffer.NumElements = 1;
            capture.add(12, 5, 0x8f, testing::statePack(Id(0), Id(0), Id(10), uav));
            std::array<uint32_t, 4> address{1, 1, 1, 0};
            std::array<int32_t, 3> offset{};
            float lod = 0;
            if (mode == 1)
                address[0] = UINT32_MAX;
            if (mode == 2)
                address[3] = 1;
            if (mode == 3)
                address[3] = 2;
            if (mode == 4)
                offset = {-2, 1, -1};
            if (mode == 5)
                lod = 1.25f;
            if (mode == 6)
                lod = 2.5f;
            if (mode == 7)
                offset = {1, 1, -1};
            if (mode == 8)
                address[dimension == 7 ? 1 : 2] = UINT32_MAX;
            if (mode == 9)
                lod = 2;
            if (mode == 10)
                address = {5, 6, 1, 0};
            if (mode == 11)
                address = {1, 1, 1, 2};
            if (mode == 12)
                address = {0, 0, 1, 3};
            capture.add(95, 7, 0x3515, testing::statePack(Id(0), Id(1), Id(4), lod));
            blob(capture, 3, Bytes(reinterpret_cast<const uint8_t *>(address.data()), 16));
            const std::string shape = dimension == 2   ? "Texture1D"
                                      : dimension == 3 ? "Texture2D"
                                      : dimension == 5 ? "Texture3D"
                                      : dimension == 7 ? "Texture1DArray"
                                                       : "Texture2DArray";
            const auto returns = format == 22 || format == 47 ? 4u
                                 : extended                   ? 5u
                                                              : typedBufferFormat(format)->returnType;
            const auto type = returns == 4 ? "uint" : "float";
            const std::string coord = dimension == 2                     ? "int2(p.x,p.w)"
                                      : dimension == 3 || dimension == 7 ? "int3(p.xy,p.w)"
                                                                         : "int4(p.xyz,p.w)";
            const auto off =
                dimension == 2 || dimension == 7 ? std::to_string(offset[0])
                : dimension == 5
                    ? "int3(" + std::to_string(offset[0]) + "," + std::to_string(offset[1]) + "," +
                          std::to_string(offset[2]) + ")"
                    : "int2(" + std::to_string(offset[0]) + "," + std::to_string(offset[1]) + ")";
            const auto code = compileHlsl("cbuffer Params:register(b0){uint4 p;}" + shape + "<" + type +
                                              "4> src:register(t0);RWStructuredBuffer<uint4> "
                                              "result:register(u1);[numthreads(1,1,1)]"
                                              "void main(){result[0]=asuint(src.Load(" +
                                              coord + "," + off + ").wzyx);}",
                                          "cs_5_0")
                                  .bytecode;
            auto shader = testing::statePack(uint64_t(code.size()));
            shader.insert(shader.end(), code.begin(), code.end());
            testing::append(shader, Id(0));
            capture.add(16, 9, 0x81, shader);
            QTemporaryDir directory;
            const auto path = directory.filePath("texture-load.gpa_frame");
            capture.save(path);
            Frame frame(path.toStdWString());
            auto config = options(vendor);
            Replay replay(frame, config);
            const auto observed = snapshotResourceInputs(replay, 5);
            const auto before = replay.readTexture(4);
            QCOMPARE(observed.inputs.textures->resources.at(4).storage.bytes, before);
            size_t paddingChanges = 0;
            if (format == 21 || format == 22) {
                QCOMPARE(before.size(), bytes.size());
                for (size_t i = 0; i < before.size(); ++i) {
                    if (i % 8 < 5)
                        QCOMPARE(before[i], bytes[i]);
                    else
                        paddingChanges += before[i] != bytes[i];
                }
            } else
                QCOMPARE(before, bytes);
            config.before = false;
            Replay reference(frame, config);
            reference.run();
            const auto expected = reference.readBuffer(10);
            std::array<uint32_t, 4> expectedWords;
            std::memcpy(expectedWords.data(), expected.data(), 16);
            for (uint32_t stage : {0u, 1u, 5u}) {
                auto program = single(stage, dimension, returns * 0x1111, offset);
                Lane lane(program, observed.inputs);
                const auto step = lane.step();
                QVERIFY(step.memoryGpuAssisted);
                QCOMPARE(step.changes.size(), size_t(1));
                const auto value = lane.value({stage == 5 ? 0u : 2u, {0}});
                QCOMPARE(value.bits, expectedWords);
                QCOMPARE(value.valid, uint8_t(15));
            }
            Program program(observed.bytecode);
            Group group(program, observed.inputs);
            while (!group.finished())
                group.step();
            QCOMPARE(observed.inputs.memory->buffers().at(10).bytes, expected);
            QCOMPARE(replay.readTexture(4), before);
            validation(replay);
            validation(reference);
            qInfo().noquote() << QString::fromStdString(
                nlohmann::json({{"probe", "dxbc-texture-load"},
                                {"vendor", vendor},
                                {"dimension", dimension},
                                {"format", format},
                                {"mode", mode},
                                {"frame_sha256", observed.frameSha256},
                                {"expected", expectedWords},
                                {"initial_padding_differences", paddingChanges},
                                {"helper", observed.inputs.gpu->identity()},
                                {"passed", true},
                                {"reference_stage", "cs"},
                                {"executed_stages", {"ps", "vs", "cs"}}})
                    .dump());
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }

  private slots:
    void formatValidity_data() {
        QTest::addColumn<uint32_t>("format");
        QTest::addColumn<unsigned>("unknownByte");
        QTest::addColumn<unsigned>("validMask");
        // Masks are after the instruction's wzyx swizzle.
        QTest::newRow("bc3-alpha-independent") << 77u << 0u << 14u;
        QTest::newRow("bc3-color-independent") << 77u << 8u << 1u;
        QTest::newRow("bc5-red-independent") << 83u << 0u << 7u;
        QTest::newRow("bc5-green-independent") << 83u << 8u << 11u;
        QTest::newRow("bc4-default-components") << 80u << 0u << 7u;
        QTest::newRow("bc6-default-alpha") << 95u << 0u << 1u;
        QTest::newRow("bc7-shared-block") << 98u << 0u << 0u;
        QTest::newRow("bgra-byte-order") << 87u << 0u << 13u;
        QTest::newRow("bgrx-unused-byte") << 88u << 3u << 15u;
        QTest::newRow("alpha-only") << 65u << 0u << 14u;
        QTest::newRow("stencil-ignores-depth") << 47u << 0u << 15u;
        QTest::newRow("stencil-green-component") << 47u << 3u << 11u;
        QTest::newRow("depth-ignores-stencil") << 46u << 3u << 15u;
        QTest::newRow("depth-red-component") << 46u << 0u << 7u;
        QTest::newRow("depth-unused-padding") << 21u << 7u << 15u;
        QTest::newRow("shared-exponent") << 67u << 3u << 1u;
        QTest::newRow("srgb-alpha-independent") << 29u << 3u << 14u;
        QTest::newRow("b565-shared-byte") << 85u << 0u << 9u;
        QTest::newRow("b4444-shared-byte") << 115u << 0u << 9u;
    }
    void formatValidity() {
        QFETCH(uint32_t, format);
        QFETCH(unsigned, unknownByte);
        QFETCH(unsigned, validMask);
        try {
            QTemporaryDir directory;
            const auto path = directory.filePath("format-validity.gpa_frame");
            testing::computeCapture(true).save(path);
            Frame frame(path.toStdWString());
            Replay replay(frame, options(0));
            replay.run();
            Inputs inputs;
            inputs.gpu = std::make_shared<GpuOperations>(replay.nativeDevice(), true);
            inputs.textures = std::make_shared<TextureInputs>();
            TextureInput texture;
            const auto storageFormat = format == 21 ? 19u : format == 46 || format == 47 ? 44u : format;
            texture.resource.type = 0x85;
            texture.resource.desc = {8, 8, 1, 1, storageFormat, 1, 0, 0, 8, 0, 0};
            texture.subresources = textureSubresources(texture.resource);
            texture.storage = Buffer::known(std::vector<uint8_t>(texture.subresources[0].size, 0x34));
            if (format == 98)
                for (size_t i = 0; i < texture.storage.bytes.size(); i += 16)
                    texture.storage.bytes[i] = 0x40;
            inputs.textures->resources.emplace(4, texture);
            TextureInputView view;
            view.resource = 4;
            view.format = format;
            view.dimension = 4;
            view.nativeDescriptor = {format, 4, 0, 1, 0, 0};
            inputs.textures->views[{7, {0}}] = view;
            inputs.constantBuffers[0] = {Value::known({0, 0, 0, 0})};
            const auto program = single(5, 3, format == 47 ? 0x4444 : 0x5555, {});
            Lane known(program, inputs);
            known.step();
            const auto original = known.value({0, {0}});
            QCOMPARE(original.valid, uint8_t(15));
            inputs.textures->resources.at(4).storage.valid.at(unknownByte) = 0;
            Lane partial(program, inputs);
            const auto step = partial.step();
            const auto value = partial.value({0, {0}});
            QCOMPARE(value.bits, original.bits);
            QCOMPARE(value.valid, uint8_t(validMask));
            QCOMPARE(step.changes.at(0).after.valid, value.valid);
            inputs.constantBuffers[0][0] = Value::known({4, 4, 0, 0});
            Lane otherBlock(program, inputs);
            otherBlock.step();
            QCOMPARE(otherBlock.value({0, {0}}).valid, uint8_t(15));
            inputs.constantBuffers[0][0] = Value::known({8, 0, 0, 0});
            Lane outside(program, inputs);
            outside.step();
            QCOMPARE(outside.value({0, {0}}).valid, uint8_t(15));
            QCOMPARE(inputs.textures->resources.at(4).storage.bytes, texture.storage.bytes);
            validation(replay);
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
    void missingBindingsStillValidateInstructions() {
        Inputs inputs;
        inputs.constantBuffers[0] = {Value::known({0, 0, 0, 0})};
        for (uint32_t dimension : {0u, 4u, 6u, 9u, 10u}) {
            const auto program = single(5, dimension, 0x4444, {});
            Lane lane(program, inputs);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, lane.step());
        }
        for (uint32_t dimension : {2u, 3u, 5u, 7u, 8u}) {
            const auto program = single(5, dimension, 0x4444, {});
            Lane lane(program, inputs);
            const auto step = lane.step();
            QVERIFY(!step.memoryGpuAssisted);
            QCOMPARE(lane.value({0, {0}}).valid, uint8_t(0));
        }
    }
    void validitySelectionAndRecovery() {
        try {
            QTemporaryDir directory;
            const auto path = directory.filePath("texture-device.gpa_frame");
            testing::computeCapture(true).save(path);
            Frame frame(path.toStdWString());
            Replay replay(frame, options(0));
            replay.run();
            bool cancelled = false;
            Inputs inputs;
            inputs.gpu =
                std::make_shared<GpuOperations>(replay.nativeDevice(), true, [&] { return cancelled; });
            inputs.textures = std::make_shared<TextureInputs>();
            TextureInput texture;
            texture.resource.type = 0x85;
            texture.resource.desc = {4, 4, 2, 1, 3, 1, 0, 0, 8, 0, 0};
            texture.subresources = textureSubresources(texture.resource);
            texture.storage = Buffer::known(std::vector<uint8_t>(320, 0x11));
            texture.storage.valid[0] = 0;
            texture.storage.valid[16] = 0;
            inputs.textures->resources.emplace(4, texture);
            TextureInputView view;
            view.resource = 4;
            view.format = 3;
            view.dimension = 4;
            view.mipCount = 2;
            view.nativeDescriptor = {3, 4, 0, 2, 0, 0};
            inputs.textures->views[{7, {0}}] = view;
            inputs.constantBuffers[0] = {Value::known({0, 0, 0, 0})};
            const auto program = single(5, 3, 0x4444, {});
            Lane lane(program, inputs);
            const auto step = lane.step();
            QVERIFY(step.memoryGpuAssisted);
            QCOMPARE(lane.value({0, {0}}).valid, uint8_t(7));
            QCOMPARE(step.changes.at(0).before.valid, uint8_t(0));
            QCOMPARE(step.changes.at(0).after.valid, uint8_t(7));
            QCOMPARE(inputs.textures->resources.at(4).storage.bytes, texture.storage.bytes);
            QCOMPARE(inputs.textures->resources.at(4).storage.valid, texture.storage.valid);
            const auto maskedProgram = single(5, 3, 0x4444, {}, 9);
            Lane masked(maskedProgram, inputs);
            masked.step();
            masked.step();
            QCOMPARE(masked.value({0, {0}}).bits, (std::array<uint32_t, 4>{0x11111111, 8, 9, 0x11111111}));
            QCOMPARE(masked.value({0, {0}}).valid, uint8_t(7));
            inputs.constantBuffers[0][0].bits[0] = 2;
            Lane unrelatedUnknown(program, inputs);
            unrelatedUnknown.step();
            QCOMPARE(unrelatedUnknown.value({0, {0}}).valid, uint8_t(15));
            inputs.constantBuffers[0][0].bits[0] = UINT32_MAX;
            Lane outside(program, inputs);
            outside.step();
            QCOMPARE(outside.value({0, {0}}).bits, (std::array<uint32_t, 4>{}));
            QCOMPARE(outside.value({0, {0}}).valid, uint8_t(15));
            const auto operations = inputs.gpu->operations();
            inputs.constantBuffers[0][0].valid = 14;
            Lane unknown(program, inputs);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, unknown.step());
            QCOMPARE(inputs.gpu->operations(), operations);
            inputs.constantBuffers[0][0] = Value::known({0, 0, 0, 0});
            inputs.textures->views.at({7, {0}}).resource.reset();
            Lane unbound(program, inputs);
            const auto nullStep = unbound.step();
            QVERIFY(!nullStep.memoryGpuAssisted);
            QCOMPARE(unbound.value({0, {0}}).bits, (std::array<uint32_t, 4>{}));
            QCOMPARE(unbound.value({0, {0}}).valid, uint8_t(15));
            inputs.textures->views.at({7, {0}}) = view;
            inputs.textures->views.at({7, {0}}).firstMip = 1;
            Lane inconsistent(program, inputs);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, inconsistent.step());
            QCOMPARE(inputs.gpu->operations(), operations);
            inputs.textures->views.at({7, {0}}) = view;
            cancelled = true;
            Lane cancel(program, inputs);
            QVERIFY_THROWS_EXCEPTION(OperationCancelled, cancel.step());
            QCOMPARE(inputs.gpu->operations(), operations);
            cancelled = false;
            const auto retry = cancel.step();
            QVERIFY(retry.memoryGpuAssisted);
            QCOMPARE(cancel.value({0, {0}}).valid, uint8_t(7));
            unsigned checks = 0;
            inputs.gpu =
                std::make_shared<GpuOperations>(replay.nativeDevice(), true, [&] { return ++checks == 3; });
            Lane submitted(program, inputs);
            QVERIFY_THROWS_EXCEPTION(OperationCancelled, submitted.step());
            QCOMPARE(checks, 3u);
            QCOMPARE(inputs.gpu->operations(), uint64_t(0));
            QCOMPARE(submitted.value({0, {0}}).valid, uint8_t(0));
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, submitted.step());
            inputs.gpu = std::make_shared<GpuOperations>(replay.nativeDevice(), true);
            Lane recreated(program, inputs);
            recreated.step();
            QCOMPARE(recreated.value({0, {0}}).valid, uint8_t(7));
            inputs.textures->resources.clear();
            Lane unavailable(program, inputs);
            unavailable.step();
            QCOMPARE(unavailable.value({0, {0}}).valid, uint8_t(0));
            validation(replay);
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
};
QTEST_GUILESS_MAIN(DxbcTextureLoadTests)
#include "DxbcTextureLoadTests.moc"
