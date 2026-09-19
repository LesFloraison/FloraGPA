#include "core/Frame.h"
#include "replay/Replay.h"
#include <Psapi.h>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QTextStream>

using namespace flora;
void save(const QString &path, const QByteArray &data) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit())
        throw std::runtime_error("Cannot save " + path.toStdString());
}
QString digest(const QByteArray &data) {
    return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
}
QJsonArray modules() {
    HMODULE handles[1024];
    DWORD bytes = 0;
    QJsonArray out;
    if (EnumProcessModules(GetCurrentProcess(), handles, sizeof handles, &bytes)) {
        for (DWORD i = 0; i < std::min<DWORD>(bytes / sizeof(HMODULE), 1024); ++i) {
            wchar_t path[32768]{};
            if (GetModuleFileNameW(handles[i], path, 32768))
                out.append(QString::fromWCharArray(path));
        }
    }
    return out;
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    app.setApplicationName("FloraGPA.Cli");
    app.setApplicationVersion("0.1.0");
    QCommandLineParser p;
    p.setApplicationDescription("Native DX11 capture inspection and isolated replay");
    p.addHelpOption();
    p.addVersionOption();
    p.addPositionalArgument("command", "inventory | replay | shader | buffer");
    p.addPositionalArgument("capture", "DX11 .gpa_frame file");
    p.addOption({"out", "New or empty output directory", "path"});
    p.addOption({"warp", "Use the WARP software adapter"});
    p.addOption({"timings", "Collect native GPU timestamps and pipeline statistics"});
    p.addOption({"debug-device", "Enable D3D11 validation"});
    p.addOption({"event", "Stop at API event", "id"});
    p.addOption({"before", "Stop before the selected event"});
    p.addOption({"id", "Resource ID", "id"});
    p.addOption({"subresource", "Texture subresource", "index", "0"});
    p.addOption({"suppress-draws", "Disable draw submissions (negative control)"});
    p.addOption({"disable", "Comma-separated disabled event IDs", "ids"});
    p.process(app);
    try {
        const auto args = p.positionalArguments();
        if (args.size() != 2)
            throw std::runtime_error("Expected command and capture path");
        Frame frame(std::filesystem::path(args[1].toStdWString()));
        auto command = args[0];
        auto parseId = [&](const QString &key) {
            bool valid = false;
            auto id = p.value(key).toULongLong(&valid);
            if (!valid)
                throw std::runtime_error("Invalid unsigned identifier: " + key.toStdString());
            return Id(id);
        };
        QJsonObject report{{"schema", "FloraGPA native result 1"},
                           {"capture", args[1]},
                           {"width", int(frame.width())},
                           {"height", int(frame.height())},
                           {"entries", int(frame.entries().size())},
                           {"reference_pixels_used", false}};
        QString out = p.value("out");
        if (!out.isEmpty()) {
            QDir dir(out);
            if (dir.exists() &&
                !dir.entryList(QDir::NoDotAndDotDot | QDir::AllEntries | QDir::Hidden).empty())
                throw std::runtime_error("Output directory must be new or empty");
            if (!QDir().mkpath(out))
                throw std::runtime_error("Cannot create output directory");
        }
        if (command == "inventory") {
            QJsonArray events, resources;
            uint64_t draws = 0, dispatches = 0;
            for (const auto &[id, e] : frame.entries()) {
                if (e.category == 7) {
                    QJsonObject row{{"id", QString::number(id)},
                                    {"name", QString::fromStdString(commandName(e.type))},
                                    {"type", e.type},
                                    {"bytes", int(e.size)}};
                    if (isDraw(e.type)) {
                        auto ev = frame.event(id);
                        row.insert("state", QString::number(ev.state));
                        row.insert("context", QString::number(ev.context));
                        QJsonArray a;
                        for (auto x : ev.args)
                            a.append(qint64(x));
                        row.insert("parameters", a);
                        if (e.type == 0x35 || e.type == 0x36)
                            ++dispatches;
                        else
                            ++draws;
                    }
                    events.append(row);
                }
                if (e.category == 5) {
                    QJsonObject row{{"id", QString::number(id)},
                                    {"name", QString::fromStdString(resourceName(e.type))},
                                    {"type", e.type},
                                    {"bytes", int(e.size)}};
                    resources.append(row);
                }
            }
            report.insert("commands", events);
            report.insert("resources", resources);
            report.insert("draws", qint64(draws));
            report.insert("dispatches", qint64(dispatches));
        } else if (command == "shader") {
            if (!p.isSet("id") || out.isEmpty())
                throw std::runtime_error("shader requires --id and --out");
            auto res = frame.resource(parseId("id"));
            auto bytes = frame.shader(res.data);
            save(out + "/shader.dxbc",
                 QByteArray(reinterpret_cast<const char *>(bytes.data()), qsizetype(bytes.size())));
            save(out + "/shader.asm", QByteArray::fromStdString(disassemble(bytes)));
            report.insert("id", p.value("id"));
        } else if (command == "replay" || command == "buffer") {
            if (out.isEmpty())
                throw std::runtime_error("Replay requires --out");
            ReplayOptions options;
            options.timings=p.isSet("timings");
            options.warp = p.isSet("warp");
            options.debug = p.isSet("debug-device");
            options.suppressDraws = p.isSet("suppress-draws");
            options.before = p.isSet("before");
            if (p.isSet("event"))
                options.until = parseId("event");
            for (auto x : p.value("disable").split(',', Qt::SkipEmptyParts)) {
                bool ok;
                auto id = x.toULongLong(&ok);
                if (!ok || frame.entry(id).category != 7)
                    throw std::runtime_error("Invalid disabled event");
                options.disabled.insert(id);
            }
            Replay replay(frame, options);
            report.insert("adapter", QString::fromStdString(replay.adapter()));
            replay.run([](Id event, size_t done, size_t total) {
                QTextStream(stderr) << "progress " << event << ' ' << done << ' ' << total << Qt::endl;
            });
            if (command == "buffer") {
                if (!p.isSet("id"))
                    throw std::runtime_error("buffer requires --id");
                auto bytes = replay.readBuffer(parseId("id"));
                QByteArray raw(reinterpret_cast<const char *>(bytes.data()), qsizetype(bytes.size()));
                save(out + "/buffer.bin", raw);
                report.insert("sha256", digest(raw));
            } else {
                auto image = replay.output(p.isSet("id") ? parseId("id") : 0, UINT(parseId("subresource")));
                QByteArray raw(reinterpret_cast<const char *>(image.rgba.data()),
                               qsizetype(image.rgba.size()));
                save(out + "/frame.rgba", raw);
                QImage preview(image.rgba.data(), int(image.width), int(image.height), int(image.width * 4),
                               QImage::Format_RGBA8888);
                if (!preview.save(out + "/frame.png"))
                    throw std::runtime_error("Cannot write PNG");
                report.insert("width", int(image.width));
                report.insert("height", int(image.height));
                report.insert("resource", QString::number(image.resource));
                report.insert("rgba_sha256", digest(raw));
            }
            QJsonObject counts;
            for (auto &[key, value] : replay.counts)
                counts.insert(QString::fromStdString(key), qint64(value));
            report.insert("counts", counts);
            if(options.timings){QJsonArray timings;for(auto& row:replay.timings)timings.append(QJsonObject{{"event",QString::number(row.event)},{"microseconds",row.microseconds}});report.insert("timings",timings);
                auto& s=replay.statistics;report.insert("pipeline_statistics",QJsonObject{{"IA Vertices",QString::number(s.IAVertices)},{"IA Primitives",QString::number(s.IAPrimitives)},{"VS Invocations",QString::number(s.VSInvocations)},{"GS Invocations",QString::number(s.GSInvocations)},{"GS Primitives",QString::number(s.GSPrimitives)},{"Clipper Invocations",QString::number(s.CInvocations)},{"Clipper Primitives",QString::number(s.CPrimitives)},{"PS Invocations",QString::number(s.PSInvocations)},{"HS Invocations",QString::number(s.HSInvocations)},{"DS Invocations",QString::number(s.DSInvocations)},{"CS Invocations",QString::number(s.CSInvocations)}});}

            report.insert("loaded_modules", modules());
            report.insert("completed", true);
        } else
            throw std::runtime_error("Unknown command");
        auto json = QJsonDocument(report).toJson();
        if (!out.isEmpty())
            save(out + "/report.json", json);
        QTextStream(stdout) << json;
        return 0;
    } catch (const std::exception &e) {
        QJsonObject error{{"error", QString::fromUtf8(e.what())}, {"completed", false}};
        QTextStream(stderr) << QJsonDocument(error).toJson(QJsonDocument::Compact) << Qt::endl;
        return 1;
    }
}
