#include "NativeAnalyzeCli.h"
#include "application/NativeAnalysisJobs.h"
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QTextStream>

int nativeAnalyzeCli(QStringList arguments) {
    using Json = nlohmann::json;
    try {
        arguments[0] = "FloraGPA.Cli analyze";
        if ((arguments.size() > 2 && arguments[2].startsWith("debug-")) ||
            arguments.contains("--debug-scope") || arguments.contains("private-draw") ||
            arguments.contains("private-primitive"))
            throw std::invalid_argument("Execution debugging and private-fragment analysis were removed from the GPA feature scope");
        QCommandLineParser parser;
        parser.addOption({"vertex-storage", "Mesh vertex storage: selected or remaining", "scope"});
        parser.setApplicationDescription("Isolated native DX11 analysis of a GPA frame");
        parser.addHelpOption();
        parser.addPositionalArgument("capture", "DX11 .gpa_frame file");
        parser.addPositionalArgument("action", "inventory | events | texture | mesh | counters | history");
        parser.addOption({"out", "New or empty output directory", "path"});
        parser.addOption({"experiment", "Compatible FloraGPA experiment project", "path"});
        parser.addOption({"timeout", "Worker timeout in seconds", "seconds", "180"});
        parser.addOption({"warp", "Select the WARP software adapter"});
        parser.addOption({"debug-device", "Enable D3D11 validation"});
        parser.addOption({"gpu-vendor", "Select a PCI vendor ID (decimal or 0x hex)", "id"});
        parser.addOption({"stage", "Mesh stage: final, vs, vs-index, vs-inputs, hs, ds or gs", "stage"});
        parser.addOption({"history-scope", "gpa (event diagnostics)", "scope"});
        for (const auto key : {"gpa-event", "resource", "start-event", "end-event", "x", "y", "mip", "layer",
                               "slice", "sample", "typed-format", "instance", "stream", "max-bytes",
                               "max-records"})
            parser.addOption({key, "Native analysis selection", "integer"});
        if (!parser.parse(arguments))
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
            else if (key == "stage" || key == "history-scope" ||
                     key == "vertex-storage")
                options[field] = parser.value(key).toStdString();
            else
                options[field] = number(parser.value(key), key == "gpu-vendor");
        }
        if (parser.value("history-scope") == "events")
            throw std::invalid_argument("Event storage history is internal validation only; use --history-scope gpa");
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
