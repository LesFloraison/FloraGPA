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
    if (args.size() != 6) {
        std::cerr << "Usage: FloraDxbcInputProbe <frame> <event> <native-stage-0..5> <vendor-or-0-for-WARP> "
                     "<report.json>\n";
        return 2;
    }
    Json report{{"schema", 1},
                {"scope", "native_dxbc_buffer_input_observation"},
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
        const auto snapshot = dxbc_exec::snapshotBufferInputs(replay, unsigned(stage));
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
    std::cout << (status ? "Input observation failed" : "Input observation recorded") << " in "
              << timer.elapsed() << " ms\n";
    return status;
}
