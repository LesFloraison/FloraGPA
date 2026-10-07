#pragma once
#include "ImagePresentation.h"
#include <QByteArray>
#include <QString>
#include <array>
#include <nlohmann/json.hpp>

namespace flora {
inline constexpr std::array<const char *, 4> coverageOutputFiles{
    "coverage.json", "coverage.png", "after_draw.png", "overlay.png"};
inline constexpr std::array<const char *, 6> quadOutputFiles{
    "data/locks.u32le", "data/counts.u32le", "data/live.u32le",
    "data/histogram.u32le", "data/reference.u32le", "data/quad_counts.png"};

struct CoverageOutput {
    nlohmann::json report;
    std::array<QByteArray, 4> files;
    PreparedImage diagnostic;
    QImage mask;
    QImage maskDisplay;
};
struct QuadOutput {
    nlohmann::json report;
    std::array<QByteArray, 6> files;
    PreparedImage image;
    QString reportText;
};
// Fixed filenames, owned export bytes and QImage-only preparation. Call from a
// cancellable background job; consumers must not reread the worker directory.
CoverageOutput readCoverageOutput(const QString &directory, const CancelCheck &cancelled = {});
QuadOutput readQuadOutput(const QString &directory, const QString &contextKey,
                          const CancelCheck &cancelled = {});
} // namespace flora
