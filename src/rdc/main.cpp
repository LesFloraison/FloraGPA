#include "Assets.h"
#include "Backend.h"
#include "application/ReplayRequest.h"
#include "application/RdcEvents.h"
#include "application/DrawResources.h"
#include <optional>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <cstdio>
#define NOMINMAX
#include <Windows.h>

#include <Psapi.h>

extern "C" __declspec(dllexport) void __cdecl renderdoc__replay__marker() {}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    using Json = nlohmann::json;
    Json result{{"ok", false}};
    QString output;
    std::optional<flora::ReplayRequestIdentity> identity;
    try {
        const auto args = app.arguments();
        if (args.size() != 3 || args[1] != "--job")
            throw std::runtime_error("Expected --job <JSON file>");
        QFile file(args[2]);
        if (!file.open(QIODevice::ReadOnly) || file.size() > 1048576)
            throw std::runtime_error("Cannot read bounded RenderDoc job");
        const auto bytes = file.readAll();
        const auto job = Json::parse(bytes.begin(), bytes.end());
        if (job.contains("request_identity")) identity = flora::parseReplayIdentity(job.at("request_identity"));
        const auto destination = QString::fromStdString(job.at("out").get<std::string>());
        if (destination.isEmpty())
            throw std::runtime_error("Missing output directory");
        QDir dir(destination);
        if (dir.exists() &&
            !dir.entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot).isEmpty())
            throw std::runtime_error("Output directory must be new or empty");
        if (!QDir().mkpath(destination))
            throw std::runtime_error("Cannot create output directory");
        output = destination;
        if (job.at("action") == "resource-workspace") {
            const auto &requests = job.at("resource_requests");
            if (!requests.is_array() || requests.empty() || requests.size() > 16)
                throw std::runtime_error("Resource workspace requires 1 to 16 requests");
            for (const auto &request : requests) flora::validateDx12ResourceRequest(request);
        }
        if (identity && identity->isolateMissingQueries && job.at("action") != "inventory" && job.at("action") != "texture" && job.at("action") != "resource-workspace" &&
            job.at("action") != "resource-history" && job.at("action") != "history" && job.at("action") != "pipeline" &&
            job.at("action") != "postmesh" &&
            !flora::rdcShaderDebugAction(job.at("action").get<std::string>()))
            throw std::runtime_error("Unsupported query-isolated analysis");
        result = flora::runRdcJob(job);
    } catch (const std::exception &e) {
        result["error"] = e.what();
        fprintf(stderr, "%s\n", e.what());
    }
    Json modules = Json::array();
    try {
        modules = result.contains("loaded_modules") ? result["loaded_modules"] : flora::rdcLoadedModules();
    } catch (const std::exception &e) {
        result["ok"] = false;
        result["module_inventory_error"] = e.what();
        fprintf(stderr, "%s\n", e.what());
        if (!result.contains("error"))
            result["error"] = e.what();
    }
    result["loaded_modules"] = modules;
    if (identity) result["request_identity"] = flora::replayIdentityJson(*identity);
    const bool evaluated = result.value("ok", false);
    if (evaluated) {
        Json forbidden = Json::array();
        for (const auto &module : modules) {
            const auto path = QString::fromStdString(module.get<std::string>()).replace('/', '\\').toLower();
            const auto name = path.section('\\', -1);
            if (path.contains("\\intelswtools\\gpa") || name == "dx11_player.dll" ||
                name == "dx11_playback.dll" || name == "shimd3d64.dll" || name == "gpa.dll")
                forbidden.push_back(module);
        }
        result["gpa_modules_loaded"] = forbidden;
        if (!forbidden.empty()) {
            result["ok"] = false;
            result["error"] = "Unexpected GPA modules loaded";
            fprintf(stderr, "Unexpected GPA modules loaded\n");
        }
    }
    if (!output.isEmpty()) {
        if (!result.value("ok", false) && QFile::exists(QDir(output).filePath("backend-diagnostic.log")))
            result["backend_diagnostic_log"] = "backend-diagnostic.log";
        if (identity) {
            try { flora::writeReplayIdentity(QDir(output).filePath("response-identity.json"), *identity); }
            catch (const std::exception &e) { fprintf(stderr, "%s\n", e.what()); return 2; }
        }
        if (evaluated) {
            QSaveFile modulesFile(QDir(output).filePath("loaded_modules.json"));
            const auto bytes = QByteArray::fromStdString(modules.dump(2));
            if (!modulesFile.open(QIODevice::WriteOnly) || modulesFile.write(bytes) != bytes.size() ||
                !modulesFile.commit())
                return 2;
        }
        QSaveFile file(QDir(output).filePath("result.json"));
        const auto bytes =
            result.value("action", "") == "inventory" ? flora::rdcInventoryText(result) : result.dump(2);
        if (!file.open(QIODevice::WriteOnly) ||
            file.write(bytes.data(), qint64(bytes.size())) != qint64(bytes.size()) || !file.commit())
            return 2;
    }
    return result.value("ok", false) ? 0 : 1;
}
