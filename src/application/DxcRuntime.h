#pragma once
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <stdexcept>

namespace flora {
inline constexpr auto dxcRuntimeSha256 =
    "5888d3e590f5bfd8743484e7f31313996734adec5741506271a5c6368cbe8ba9";
inline constexpr auto dxcRuntimeRelease = "v1.8.2505.1";

// Resolve and verify the explicit library before any loader or backend can fall
// back to a development-machine SDK. Shared by disassembly and DX12 analysis.
inline QString requireDxcRuntime(const QString &requested) {
    const auto canonical = QFileInfo(requested).canonicalFilePath();
    QFile file(canonical);
    if (canonical.isEmpty() || !file.open(QIODevice::ReadOnly))
        throw std::runtime_error("DXC runtime is missing; deploy the pinned DXCompiler/dxcompiler.dll");
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file))
        throw std::runtime_error("Cannot hash DXC runtime");
    if (hash.result().toHex() != dxcRuntimeSha256)
        throw std::runtime_error("DXC runtime differs from the pinned v1.8.2505.1 x64 release");
    return canonical;
}
} // namespace flora
