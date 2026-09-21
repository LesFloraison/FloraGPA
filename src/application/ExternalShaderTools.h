#pragma once
#include "core/Frame.h"
#include <QByteArray>
#include <QString>
#include <QStringList>
#include <nlohmann/json.hpp>

namespace flora {
// Child starts suspended and joins a kill-on-close job before executing.
// The application never loads the optional tool into its own process.
void runShaderTool(const QString &executable, const QStringList &arguments, const QString &directory,
                   const QString &logName, unsigned timeoutMs = 60000);
nlohmann::json exportRecoveredHlsl(Bytes bytecode, const QString &directory, const QString &executable = {},
                                   const nlohmann::json &savedSource = nullptr, unsigned timeoutMs = 60000);
nlohmann::json assembleShader(Bytes reference, const QByteArray &source, const QString &directory,
                              const QString &executable, unsigned timeoutMs = 60000);
} // namespace flora
