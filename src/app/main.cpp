#include "MainWindow.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QFile>
#include <QFontDatabase>
#include <QStyleFactory>
#include <QTimer>

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    if (app.platformName() == "offscreen") {
        const auto fonts = qEnvironmentVariable("WINDIR") + "/Fonts/";
        for (const auto& file : {"segoeui.ttf", "segoeuib.ttf", "consola.ttf"})
            QFontDatabase::addApplicationFont(fonts + file);
    }
    app.setApplicationName("FloraGPA");
    app.setOrganizationName("FloraGPA");
    app.setApplicationVersion("0.1.0");
    app.setStyle(QStyleFactory::create("Fusion"));
    app.setFont(QFont("Segoe UI", 10));
    app.setStyleSheet(R"(
QWidget { background: #303b47; color: #c5d2df; selection-background-color: #155974; selection-color: #eefaff; }
QMainWindow, QMenuBar, QStatusBar { background: #25303b; }
QMenuBar::item { padding: 5px 12px; background: transparent; }
QMenuBar::item:selected, QMenu::item:selected { background: #166486; }
QMenu { border: 1px solid #4a5a68; padding: 4px; }
QMenu::item { padding: 6px 28px; }
QToolBar { border: 0; border-bottom: 1px solid #202a34; spacing: 5px; padding: 5px; background: #2b3642; }
QToolButton { padding: 5px 8px; border: 1px solid transparent; }
QToolButton:hover, QPushButton:hover { background: #405468; border: 1px solid #5c7488; }
QToolButton:pressed, QPushButton:pressed { background: #155974; }
QWidget:disabled { color: #6d7c89; }
QLineEdit, QComboBox, QSpinBox { background: #26323e; border: 1px solid #455463; padding: 5px 8px; min-height: 19px; }
QLineEdit:focus, QComboBox:focus { border-color: #28b9e7; }
QComboBox::drop-down { border: 0; width: 18px; }
QTabWidget::pane { border: 0; border-top: 1px solid #202a34; }
QTabBar::tab { background: #293440; border-top: 2px solid transparent; padding: 8px 12px; color: #adbdcb; }
QTabBar::tab:selected { background: #354453; color: #e6f6ff; border-top: 2px solid #28b9e7; }
QTabBar::tab:disabled { color: #667582; }
QTableView, QTreeView, QPlainTextEdit { background: #2c3743; alternate-background-color: #303d49; border: 0; outline: 0; }
QTableView::item, QTreeView::item { padding: 3px; border-bottom: 1px solid #2a3440; }
QTableView::item:selected, QTreeView::item:selected { background: #175974; color: #e7f7ff; }
QHeaderView::section { background: #26323e; border: 0; border-bottom: 1px solid #435260; padding: 6px; color: #93aabe; }
QDockWidget::title { background: #25313c; padding: 6px; color: #98b0c3; }
QSplitter::handle { background: #202a34; }
QScrollBar:vertical { background: #26313c; width: 10px; margin: 0; }
QScrollBar::handle:vertical { background: #526574; min-height: 24px; }
QScrollBar:horizontal { background: #26313c; height: 10px; margin: 0; }
QScrollBar::handle:horizontal { background: #526574; min-width: 24px; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }
QProgressBar { border: 0; background: #24313e; }
QProgressBar::chunk { background: #2db8e4; }
QToolTip { background: #1d2934; color: #dae8f1; border: 1px solid #4a7189; padding: 5px; }
QStatusBar::item { border: 0; }
)");
    QCommandLineParser p;
    p.addHelpOption();
    p.addVersionOption();
    p.addPositionalArgument("capture", "Capture to open", "[capture]");
    p.addOption({"screenshot", "Save this application's rendered window after replay", "path"});
    p.addOption({"collect", "Collect GPU timestamps after loading"});
    p.process(app);
    flora::MainWindow window;
    window.show();
    bool collecting = false;
    QObject::connect(&window, &flora::MainWindow::taskFinished, &window, [&](bool success) {
        if (!success)
            return;
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
