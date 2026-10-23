#pragma once
#include "NativeAnalysis.h"
#include <QByteArray>
#include <QMap>
namespace flora {
using NativeArtifactBytes = QMap<QString, QByteArray>;
// Enumerate completed action exports before publishing result.json.
nlohmann::json nativeArtifactManifest(const QString &directory, const CancelCheck &cancelled = {});
// Retain verified bytes so consumers do not reopen files after validation.
NativeArtifactBytes readNativeArtifacts(const QString &directory, const nlohmann::json &manifest,
                                        uint64_t maximumBytes, const CancelCheck &cancelled = {});
void verifyNativeArtifacts(const QString &directory, const nlohmann::json &manifest,
                           const CancelCheck &cancelled = {});
} // namespace flora
