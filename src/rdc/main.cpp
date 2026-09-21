#include "Backend.h"
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
    try {
        const auto args = app.arguments();
        if (args.size() != 3 || args[1] != "--job")
            throw std::runtime_error("Expected --job <JSON file>");
        QFile file(args[2]);
        if (!file.open(QIODevice::ReadOnly) || file.size() > 1048576)
            throw std::runtime_error("Cannot read bounded RenderDoc job");
        const auto bytes = file.readAll();
        const auto job = Json::parse(bytes.begin(), bytes.end());
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
        result = flora::runRdcJob(job);
    } catch (const std::exception &e) {
        result["error"] = e.what();
        fprintf(stderr, "%s\n", e.what());
    }
    Json modules = Json::array();
    HMODULE handles[2048];
    DWORD length{};
    if (EnumProcessModules(GetCurrentProcess(), handles, sizeof(handles), &length)) {
        for (DWORD i = 0; i < (std::min)(length / DWORD(sizeof(HMODULE)), DWORD(2048)); ++i) {
            wchar_t name[32768]{};
            if (GetModuleFileNameW(handles[i], name, 32768))
                modules.push_back(QString::fromWCharArray(name).toStdString());
        }
    }
    result["loaded_modules"] = modules;
    if (!output.isEmpty()) {
        QSaveFile file(QDir(output).filePath("result.json"));
        const auto bytes = result.dump(2);
        if (!file.open(QIODevice::WriteOnly) ||
            file.write(bytes.data(), qint64(bytes.size())) != qint64(bytes.size()) || !file.commit())
            return 2;
    }
    return result.value("ok", false) ? 0 : 1;
}
