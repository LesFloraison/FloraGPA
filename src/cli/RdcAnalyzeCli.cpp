#include "RdcAnalyzeCli.h"
#include "application/RdcJobs.h"
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>
#include <utility>
int rdcAnalyzeCli(QStringList args) {
    try {
        args[0] = "FloraGPA.Cli rdc-analyze";
        QStringList normalized;
        for (qsizetype i = 0; i < args.size(); ++i) {
            if (args[i] == "--") {
                normalized.append(args.mid(i));
                break;
            }
            normalized << args[i];
            if (args[i] == "--group" || args[i] == "--thread") {
                if (i + 3 >= args.size())
                    throw std::runtime_error("--group and --thread require three coordinates");
                normalized << (args[i + 1] + ',' + args[i + 2] + ',' + args[i + 3]);
                i += 3;
            }
        }
        QCommandLineParser p;
        p.setApplicationDescription(
            "Headless native analysis of an independently generated RenderDoc capture");
        p.addHelpOption();
        p.addPositionalArgument("capture", "Independent .rdc capture");
        p.addPositionalArgument("action", "inventory | postmesh | history | debug-pixel | debug-vertex | "
                                          "debug-thread | counters | texture");
        p.addOption({"out", "New or empty output directory", "path"});
        p.addOption({"renderdoc", "RenderDoc 1.45 native library", "path",
                     QDir(qEnvironmentVariable("ProgramFiles", "C:/Program Files"))
                         .filePath("RenderDoc/renderdoc.dll")});
        p.addOption({"timeout", "Worker timeout in seconds", "seconds", "180"});
        for (const auto &[key, description] :
             {std::pair{"gpa-event", "Original GPA draw, dispatch or resource-write command ID"},
              std::pair{"eid", "Native RenderDoc event ID (alternative to --gpa-event)"},
              std::pair{"resource", "Original GPA texture resource ID (default: color target 0)"},
              std::pair{"x", "Pixel X coordinate"}, std::pair{"y", "Pixel Y coordinate"},
              std::pair{"mip", "Texture mip level"}, std::pair{"layer", "Texture array layer"},
              std::pair{"sample", "MSAA sample index"}, std::pair{"instance", "Zero-based draw instance"},
              std::pair{"vertex", "Vertex invocation within the draw"},
              std::pair{"index", "Override actual vertex index (default: derive from draw/index buffer)"}})
            p.addOption({key, description, "integer"});
        p.addOption({"stage", "Post-shader stage: VSOut or GSOut", "stage", "VSOut"});
        p.addOption({"group", "Compute group: X Y Z", "coordinates"});
        p.addOption({"thread", "Compute thread: X Y Z", "coordinates"});
        if (!p.parse(normalized))
            throw std::runtime_error(p.errorText().toStdString());
        if (p.isSet("help") || p.isSet("help-all")) {
            const auto help = p.helpText();
            QTextStream(stdout) << "Usage: FloraGPA.Cli rdc-analyze [options] capture action\n"
                                << help.mid(help.indexOf('\n') + 1);
            return 0;
        }
        const auto positional = p.positionalArguments();
        if (positional.size() != 2 || !p.isSet("out"))
            throw std::runtime_error("Expected rdc-analyze <capture.rdc> <action> --out <directory>");
        using Json = nlohmann::json;
        Json options = Json::object();
        auto number = [](const QString &text) -> uint64_t {
            bool valid = false;
            const auto value = text.toULongLong(&valid);
            if (!valid || text.trimmed().startsWith('-'))
                throw std::runtime_error("Invalid unsigned analysis parameter");
            return value;
        };
        for (const auto key : {"gpa-event", "eid", "resource", "x", "y", "mip", "layer", "sample", "instance",
                               "vertex", "index"})
            if (p.isSet(key))
                options[QString(key).replace('-', '_').toStdString()] = number(p.value(key));
        options["stage"] = p.value("stage").toStdString();
        for (const auto key : {"group", "thread"})
            if (p.isSet(key)) {
                const auto parts = p.value(key).split(',');
                if (parts.size() != 3)
                    throw std::runtime_error("Expected three compute coordinates");
                options[key] = Json::array();
                for (const auto &part : parts)
                    options[key].push_back(number(part));
            }
        bool valid = false;
        const auto timeout = p.value("timeout").toDouble(&valid);
        if (!valid)
            throw std::runtime_error("Invalid analysis timeout");
        const auto job = flora::prepareRdcJob(positional[0], positional[1].toStdString(), p.value("out"),
                                              p.value("renderdoc"), options);
        const auto report =
            flora::runRdcAnalysis(job, QCoreApplication::applicationDirPath() + "/FloraGPA.Rdc.exe", timeout);
        QTextStream(stdout) << report << Qt::endl;
        return 0;
    } catch (const std::exception &e) {
        QTextStream(stderr) << QJsonDocument(
                                   QJsonObject{{"completed", false}, {"error", QString::fromUtf8(e.what())}})
                                   .toJson(QJsonDocument::Compact)
                            << Qt::endl;
        return 1;
    }
}
