#include "application/DxbcGpuOperations.h"
#include "application/DxbcInputSnapshot.h"
#include "application/PostTransform.h"
#include "replay/Replay.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QSaveFile>
#include <bit>
#include <cmath>
#include <iostream>
using namespace flora;
using Json = nlohmann::json;
namespace {
uint64_t number(const QString &text) {
    bool ok = false;
    const auto value = text.toULongLong(&ok, 0);
    if (!ok)
        throw std::runtime_error("Invalid vertex probe number");
    return value;
}
Json registers(const std::map<dxbc_exec::Register, dxbc_exec::Value> &values) {
    Json result = Json::array();
    for (const auto &[reg, value] : values)
        result.push_back(
            {{"kind", reg.kind}, {"indices", reg.indices}, {"bits", value.bits}, {"valid", value.valid}});
    return result;
}
} // namespace
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    if (args.size() != 7 &&
        !(args.size() == 8 && (args[7] == "--execute" || args[7] == "--compare-native"))) {
        std::cerr << "Usage: FloraDxbcVertexInputProbe <frame> <event> <reference> <instance> "
                     "<vendor-or-0-for-WARP> <report.json> [--execute | --compare-native]\n";
        return 2;
    }
    QElapsedTimer elapsed;
    elapsed.start();
    Json report{{"schema", 1},
                {"scope", "native_vertex_input_observation"},
                {"complete_release_acceptance", false},
                {"capture", args[1].toStdString()}};
    int status = 0;
    try {
        const auto event = number(args[2]), reference = number(args[3]), instance = number(args[4]),
                   vendor = number(args[5]);
        if (reference > UINT32_MAX || instance > UINT32_MAX || vendor > UINT32_MAX)
            throw std::runtime_error("Vertex probe selection exceeds uint32");
        Frame frame(args[1].toStdWString());
        ReplayOptions options;
        options.until = event;
        options.before = options.prepareBeforeDraw = true;
        options.warp = !vendor;
        if (vendor)
            options.vendor = uint32_t(vendor);
        Replay replay(frame, options);
        const auto cancelled = [&] { return elapsed.elapsed() >= 120000; };
        const auto snapshot = dxbc_exec::snapshotVertexInputs(
            replay, {uint32_t(reference), uint32_t(instance)}, 256ull * 1024 * 1024, cancelled);
        const auto &resources = snapshot.resources;
        report["input_observation_ms"] = elapsed.nsecsElapsed() / 1e6;
        report["invocation"] = snapshot.invocation;
        report["frame_sha256"] = resources.frameSha256;
        report["shader_sha256"] =
            QCryptographicHash::hash(QByteArrayView(reinterpret_cast<const char *>(resources.bytecode.data()),
                                                    resources.bytecode.size()),
                                     QCryptographicHash::Sha256)
                .toHex()
                .toStdString();
        report["adapter_luid"] = resources.adapterLuid;
        report["adapter_vendor"] = resources.adapterVendor;
        report["adapter_device"] = resources.adapterDevice;
        report["input_registers"] = registers(resources.inputs.registers);
        report["input_observation_complete"] = true;
        if (args.size() == 8) {
            dxbc_exec::Program program(resources.bytecode);
            dxbc_exec::Lane lane(program, resources.inputs);
            uint64_t steps = 0;
            while (!lane.finished()) {
                checkCancellation(cancelled);
                lane.step();
                ++steps;
            }
            report["execution_steps"] = steps;
            report["final_registers"] = registers(lane.registers());
            report["execution_complete"] = true;
            if (resources.inputs.gpu) {
                report["execution_gpu_identity"] = resources.inputs.gpu->identity();
                report["execution_gpu_operations"] = resources.inputs.gpu->operations();
            }
            if (args[7] == "--compare-native") {
                PostTransformOptions inspection;
                inspection.stage = "vs-inputs";
                inspection.instance = uint32_t(instance);
                const auto geometry = inspectPostTransform(replay, event, inspection);
                const auto &references = geometry.report.at("input_assembly").at("row_references");
                size_t selected = references.size();
                for (size_t i = 0; i < references.size(); ++i)
                    if (references[i] == reference) {
                        if (selected != references.size())
                            throw std::runtime_error("Ambiguous native output input reference");
                        selected = i;
                    }
                if (selected == references.size())
                    throw std::runtime_error("Native output input reference was not captured");
                const auto stride = geometry.report.at("stride").get<size_t>();
                const auto bytes = Bytes(geometry.bytes).subspan(selected * stride, stride);
                Json comparison = Json::array();
                uint64_t differences = 0, unknown = 0;
                for (const auto &field : geometry.report.at("attributes")) {
                    const auto reg = field.at("register").get<uint32_t>();
                    const auto value = lane.value({2, {reg}});
                    size_t offset = field.at("offset");
                    for (const auto &component : field.at("register_components")) {
                        const auto c = component.get<uint32_t>();
                        const auto native = Reader(bytes.subspan(offset)).read<uint32_t>();
                        offset += 4;
                        const bool valid = (value.valid & (1u << c)) != 0;
                        const bool equal = valid && value.bits[c] == native;
                        if (!valid)
                            ++unknown;
                        else if (!equal)
                            ++differences;
                        Json item{{"register", reg},
                                  {"component", c},
                                  {"type", field.at("component_type")},
                                  {"native_bits", native},
                                  {"executor_bits", value.bits[c]},
                                  {"executor_valid", valid},
                                  {"exact", equal}};
                        if (field.at("component_type") == 3 && valid) {
                            const auto a = std::bit_cast<float>(native),
                                       b = std::bit_cast<float>(value.bits[c]);
                            if (std::isfinite(a) && std::isfinite(b))
                                item["absolute_difference"] = std::abs(double(a) - b);
                        }
                        comparison.push_back(item);
                    }
                }
                report["native_comparison"] = {{"components", comparison},
                                               {"different_components", differences},
                                               {"unknown_components", unknown},
                                               {"exact", differences == 0 && unknown == 0},
                                               {"reference", "original-shader-native-point-output"},
                                               {"original_schedule_recovered", false}};
            }
        }
    } catch (const std::exception &error) {
        report["error"] = error.what();
        status = 1;
    }
    report["elapsed_ms"] = elapsed.nsecsElapsed() / 1e6;
    report["status"] = status == 0 ? "ok" : "failed";
    const auto text = report.dump(2) + "\n";
    QSaveFile file(args[6]);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(text.data(), qint64(text.size())) != qint64(text.size()) || !file.commit()) {
        std::cerr << "Cannot write vertex input probe report\n";
        return 3;
    }
    std::cout << text;
    return status;
}
