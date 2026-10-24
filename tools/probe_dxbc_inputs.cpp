#include "application/DxbcGpuOperations.h"
#include "application/DxbcInputSnapshot.h"
#include "replay/Replay.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <algorithm>
#include <iostream>
#include <nlohmann/json.hpp>
using namespace flora;
using Json = nlohmann::json;
namespace {
std::string hash(Bytes bytes) {
    return QCryptographicHash::hash(
               QByteArrayView(reinterpret_cast<const char *>(bytes.data()), bytes.size()),
               QCryptographicHash::Sha256)
        .toHex()
        .toStdString();
}
uint64_t number(const QString &text) {
    bool ok = false;
    const auto result = text.toULongLong(&ok, 0);
    if (!ok)
        throw std::runtime_error("Invalid numeric probe argument");
    return result;
}
} // namespace
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    const bool execute = args.size() == 7 && args[6] == "--execute-group";
    if (args.size() != 6 && !execute) {
        std::cerr << "Usage: FloraDxbcInputProbe <frame> <event> <native-stage-0..5> <vendor-or-0-for-WARP> "
                     "<report.json> [--execute-group]\n";
        return 2;
    }
    Json report{
        {"schema", execute ? 3 : 2},
        {"scope", execute ? "native_dxbc_compute_group_probe" : "native_dxbc_resource_input_observation"},
        {"complete_release_acceptance", false},
        {"shader_execution_performed", false},
        {"capture", args[1].toStdString()}};
    QElapsedTimer timer;
    timer.start();
    int status = 0;
    try {
        const auto event = number(args[2]), stage = number(args[3]), vendor = number(args[4]);
        if (stage > 5 || vendor > UINT32_MAX)
            throw std::runtime_error("Probe stage or vendor out of range");
        if (execute && stage != 5)
            throw std::runtime_error("Group execution requires a compute shader");
        report["event"] = event;
        report["stage"] = stage;
        report["requested_vendor"] = vendor;
        Frame frame(args[1].toStdWString());
        ReplayOptions options;
        options.until = event;
        options.before = true;
        options.warp = !vendor;
        if (vendor)
            options.vendor = uint32_t(vendor);
        Replay replay(frame, options);
        QElapsedTimer executionTimer;
        const auto cancelled = [&] { return executionTimer.isValid() && executionTimer.elapsed() >= 30000; };
        const auto snapshot =
            dxbc_exec::snapshotResourceInputs(replay, unsigned(stage), 256 * 1024 * 1024, cancelled);
        report["source"] = dxbc_exec::InputSnapshot::source;
        report["frame_sha256"] = snapshot.frameSha256;
        report["shader"] = snapshot.shader;
        report["shader_sha256"] = hash(snapshot.bytecode);
        report["adapter_vendor"] = snapshot.adapterVendor;
        report["adapter_device"] = snapshot.adapterDevice;
        report["adapter_luid"] = snapshot.adapterLuid;
        report["replay_generation"] = snapshot.replayGeneration;
        report["bindings"] = Json::array();
        report["unavailable"] = Json::array();
        report["buffers"] = Json::array();
        for (const auto &[reg, binding] : snapshot.bindings)
            if (binding.resource || binding.view)
                report["bindings"].push_back({{"kind", reg.kind},
                                              {"slot", reg.indices.at(0)},
                                              {"resource", binding.resource},
                                              {"view", binding.view},
                                              {"offset", binding.offset},
                                              {"size", binding.size},
                                              {"stride", binding.stride},
                                              {"format", binding.format},
                                              {"experimental_view", binding.experimentalView}});
        for (const auto &[reg, reason] : snapshot.unavailable)
            report["unavailable"].push_back(
                {{"kind", reg.kind}, {"slot", reg.indices.at(0)}, {"reason", reason}});
        for (const auto &[id, buffer] : snapshot.inputs.memory->buffers())
            report["buffers"].push_back(
                {{"resource", id},
                 {"bytes", buffer.bytes.size()},
                 {"sha256", hash(buffer.bytes)},
                 {"validity_sha256", hash(buffer.valid)},
                 {"known_bytes", std::count(buffer.valid.begin(), buffer.valid.end(), uint8_t(1))}});
        report["counters"] = Json::array();
        report["textures"] = Json::array();
        report["texture_views"] = Json::array();
        for (const auto &[id, texture] : snapshot.inputs.textures->resources) {
            Json item{{"resource", id},
                      {"type", texture.resource.type},
                      {"descriptor", texture.resource.desc},
                      {"min_lod", texture.minLod},
                      {"bytes", texture.storage.bytes.size()},
                      {"sha256", hash(texture.storage.bytes)},
                      {"validity_sha256", hash(texture.storage.valid)},
                      {"known_bytes",
                       std::count(texture.storage.valid.begin(), texture.storage.valid.end(), uint8_t(1))},
                      {"subresources", Json::array()}};
            for (const auto &sub : texture.subresources)
                item["subresources"].push_back({{"mip", sub.mip},
                                                {"layer", sub.layer},
                                                {"width", sub.width},
                                                {"height", sub.height},
                                                {"depth", sub.depth},
                                                {"offset", sub.offset},
                                                {"size", sub.size},
                                                {"row_pitch", sub.rowPitch},
                                                {"slice_pitch", sub.slicePitch}});
            report["textures"].push_back(item);
        }
        for (const auto &[reg, view] : snapshot.inputs.textures->views) {
            if (!view.resource)
                continue;
            Json item{{"kind", reg.kind},
                      {"slot", reg.indices.at(0)},
                      {"resource", *view.resource},
                      {"format", view.format},
                      {"dimension", view.dimension},
                      {"first_mip", view.firstMip},
                      {"mip_count", view.mipCount},
                      {"first_layer", view.firstLayer},
                      {"layer_count", view.layerCount},
                      {"native_descriptor", view.nativeDescriptor},
                      {"plane_slice", nullptr},
                      {"depth_slices", nullptr},
                      {"storage_available", snapshot.inputs.textures->resources.contains(*view.resource)}};
            if (view.planeSlice)
                item["plane_slice"] = *view.planeSlice;
            if (view.depthSlices)
                item["depth_slices"] = *view.depthSlices;
            report["texture_views"].push_back(item);
        }
        for (const auto &[id, counter] : snapshot.inputs.memory->counters()) {
            Json item{{"view", id},
                      {"kind", counter.kind == dxbc_exec::CounterKind::Ordered ? "ordered" : "append"},
                      {"value", nullptr}};
            if (counter.value)
                item["value"] = *counter.value;
            if (snapshot.unavailableCounters.contains(id))
                item["reason"] = snapshot.unavailableCounters.at(id);
            report["counters"].push_back(item);
        }
        report["observed"] = true;
        if (execute) {
            auto &execution = report["execution"];
            execution = {{"attempted", true},
                         {"completed", false},
                         {"steps", 0},
                         {"group", {0, 0, 0}},
                         {"model", dxbc_exec::Group::executionModel},
                         {"step_limit", 100000},
                         {"timeout_ms", 30000},
                         {"native_output_parity_verified", false}};
            executionTimer.start();
            uint64_t steps = 0;
            try {
                const dxbc_exec::Program program(snapshot.bytecode);
                execution["thread_group_size"] = program.threadGroupSize();
                dxbc_exec::Group group(program, snapshot.inputs, {}, 100000, cancelled);
                while (!group.finished()) {
                    const auto step = group.step();
                    ++steps;
                    execution["last_completed_step"] = {{"lane", step.lane},
                                                        {"instruction", step.step.instruction},
                                                        {"word_offset", step.step.wordOffset}};
                }
                execution["completed"] = true;
            } catch (const std::exception &error) {
                execution["error"] = error.what();
                status = 1;
            }
            execution["steps"] = steps;
            execution["elapsed_ms"] = executionTimer.elapsed();
            report["shader_execution_performed"] = steps != 0;
            if (snapshot.inputs.gpu) {
                execution["helper"] = snapshot.inputs.gpu->identity();
                execution["gpu_operations"] = snapshot.inputs.gpu->operations();
            }
            execution["buffers_after"] = Json::array();
            for (const auto &[id, buffer] : snapshot.inputs.memory->buffers())
                execution["buffers_after"].push_back({{"resource", id},
                                                      {"sha256", hash(buffer.bytes)},
                                                      {"validity_sha256", hash(buffer.valid)}});
        }
    } catch (const std::exception &error) {
        report["observed"] = false;
        report["error"] = error.what();
        status = 1;
    }
    report["elapsed_ms"] = timer.elapsed();
    QFile file(args[5]);
    const auto text = report.dump(2) + "\n";
    if (!file.open(QIODevice::WriteOnly) || file.write(text.data(), text.size()) != qint64(text.size())) {
        std::cerr << "Cannot write probe report\n";
        return 2;
    }
    std::cout << (status ? "Probe incomplete" : "Probe recorded") << " in " << timer.elapsed() << " ms\n";
    return status;
}
