#pragma once
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QSettings>

namespace flora {
// Resolve a path only: opening the analyzer must not load RenderDoc into the GUI.
inline QString resolveRenderDocLibrary(const QString &applicationDirectory,
                                      const QString &configuredPath,
                                      const QString &programFiles) {
    if (!configuredPath.isEmpty() && QFileInfo(configuredPath).isFile())
        return QDir::cleanPath(configuredPath);
    const auto bundled = QDir(applicationDirectory).filePath("RenderDoc/renderdoc.dll");
    if (QFileInfo(bundled).isFile())
        return bundled;
    return QDir(programFiles).filePath("RenderDoc/renderdoc.dll");
}
inline QString defaultRenderDocLibrary() {
    return resolveRenderDocLibrary(QCoreApplication::applicationDirPath(), {},
                                   qEnvironmentVariable("ProgramFiles", "C:/Program Files"));
}
inline QString configuredRenderDocLibrary() {
    return resolveRenderDocLibrary(QCoreApplication::applicationDirPath(),
                                   QSettings().value("analysis/renderdoc").toString(),
                                   qEnvironmentVariable("ProgramFiles", "C:/Program Files"));
}
} // namespace flora
