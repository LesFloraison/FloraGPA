#include "Appearance.h"
#include "MainWindow.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QFile>
#include <QTimer>

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    app.setApplicationName("FloraGPA");
    app.setOrganizationName("FloraGPA");
    app.setApplicationVersion("0.1.0");
    flora::applyAppearance(app);
    QCommandLineParser p;
    p.addHelpOption();
    p.addVersionOption();
    p.addPositionalArgument("capture", "Capture to open", "[capture]");
    p.addOption({"screenshot", "Save this application's rendered window after replay", "path"});
    p.addOption({"collect", "Collect GPU timestamps after loading"});
    p.addOption({"size", "Window size for layout verification", "widthxheight"});
    p.process(app);
    flora::MainWindow window;
    if (p.isSet("size")) {
        auto dims = p.value("size").split('x');
        if (dims.size() != 2)
            return 2;
        bool a = false, b = false;
        auto w = dims[0].toInt(&a), h = dims[1].toInt(&b);
        if (!a || !b || w < 1000 || h < 640)
            return 2;
        window.resize(w, h);
    }
    window.show();
    bool collecting = false;
    QObject::connect(&window, &flora::MainWindow::taskFinished, &window, [&](bool success) {
        if (!success) {
            if (p.isSet("screenshot"))
                app.exit(1);
            return;
        }
        if (p.isSet("collect") && !collecting) {
            collecting = true;
            QTimer::singleShot(0, &window, [&] { window.replay(true); });
            return;
        }
        if (p.isSet("screenshot"))
            QTimer::singleShot(250, &window, [&] {
                bool ok = window.grab().save(p.value("screenshot"));
                app.exit(ok ? 0 : 2);
            });
    });
    if (!p.positionalArguments().isEmpty())
        QTimer::singleShot(0, &window, [&] { window.openCapture(p.positionalArguments().first()); });
    return app.exec();
}
