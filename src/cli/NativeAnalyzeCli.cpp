#include "NativeAnalyzeCli.h"
#include "application/NativeAnalysisJobs.h"
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QTextStream>

int nativeAnalyzeCli(QStringList arguments) {
    using Json = nlohmann::json;
    try {
        arguments[0] = "FloraGPA.Cli analyze";
        QStringList normalized;
        for (qsizetype i = 0; i < arguments.size(); ++i) {
            if (arguments[i] == "--") {
                normalized.append(arguments.mid(i));
                break;
            }
            normalized << arguments[i];
            if (arguments[i] == "--group" || arguments[i] == "--thread") {
                if (i + 3 >= arguments.size())
                    throw std::invalid_argument("Expected three compute coordinates");
                normalized << arguments.mid(i + 1, 3).join(',');
                i += 3;
            }
        }
        QCommandLineParser parser;
        parser.setApplicationDescription("Isolated native DX11 analysis of a GPA frame");
        parser.addHelpOption();
        parser.addPositionalArgument("capture", "DX11 .gpa_frame file");
        parser.addPositionalArgument(
            "action",
            "inventory | events | texture | mesh | counters (history and VS/PS/CS debugging pending parity)");
        parser.addOption({"out", "New or empty output directory", "path"});
        parser.addOption({"experiment", "Compatible FloraGPA experiment project", "path"});
        parser.addOption({"timeout", "Worker timeout in seconds", "seconds", "180"});
        parser.addOption({"warp", "Select the WARP software adapter"});
        parser.addOption({"debug-device", "Enable D3D11 validation"});
        parser.addOption({"gpu-vendor", "Select a PCI vendor ID (decimal or 0x hex)", "id"});
        parser.addOption({"stage", "Mesh stage: final, vs, hs, ds or gs", "stage"});
        parser.addOption({"group", "Compute group X Y Z", "coordinates"});
        parser.addOption({"thread", "Compute local thread X Y Z", "coordinates"});
        for (const auto key :
             {"gpa-event", "resource", "start-event", "end-event", "x", "y", "mip", "layer", "slice",
              "sample", "typed-format", "instance", "vertex", "index", "stream", "max-bytes"})
            parser.addOption({key, "Native analysis selection", "integer"});
        if (!parser.parse(normalized))
            throw std::invalid_argument(parser.errorText().toStdString());
        if (parser.isSet("help") || parser.isSet("help-all")) {
            QTextStream(stdout)
                << "Usage: FloraGPA.Cli analyze <frame.gpa_frame> <action> [options] --out <directory>\n"
                << parser.helpText();
            return 0;
        }
        const auto positional = parser.positionalArguments();
        if (positional.size() != 2 || !parser.isSet("out"))
            throw std::invalid_argument("Expected analyze <frame.gpa_frame> <action> --out <directory>");
        auto number = [](const QString &text, bool hexadecimal = false) -> uint64_t {
            bool valid = false;
            const auto value =
                text.toULongLong(&valid, hexadecimal && text.startsWith("0x", Qt::CaseInsensitive) ? 16 : 10);
            if (!valid || text != text.trimmed() || text.startsWith('-') || text.startsWith('+'))
                throw std::invalid_argument("Invalid unsigned analysis selection");
            return value;
        };
        Json options = Json::object();
        for (const auto &key : parser.optionNames()) {
            const auto field = QString(key).replace('-', '_').toStdString();
            if (key == "out" || key == "experiment" || key == "timeout")
                continue;
            if (key == "warp" || key == "debug-device")
                options[field] = true;
            else if (key == "stage")
                options[field] = parser.value(key).toStdString();
            else if (key == "group" || key == "thread") {
                options[field] = Json::array();
                for (const auto &coordinate : parser.value(key).split(','))
                    options[field].push_back(number(coordinate));
            } else
                options[field] = number(parser.value(key), key == "gpu-vendor");
        }
        bool valid = false;
        const auto timeout = parser.value("timeout").toDouble(&valid);
        if (!valid)
            throw std::invalid_argument("Invalid analysis timeout");
        const auto worker = QCoreApplication::applicationDirPath() + "/FloraGPA.Worker.exe";
        const auto job =
            flora::prepareNativeAnalysisJob(positional[0], positional[1].toStdString(), parser.value("out"),
                                            worker, options, parser.value("experiment"));
        QTextStream(stdout) << flora::runNativeAnalysisJob(job, worker, timeout) << Qt::endl;
        return 0;
    } catch (const std::exception &error) {
        QTextStream(stderr) << QByteArray::fromStdString(Json{{"ok", false}, {"error", error.what()}}.dump())
                            << Qt::endl;
        return 1;
    }
}
