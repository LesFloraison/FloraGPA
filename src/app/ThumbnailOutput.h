#pragma once
#include "ImagePresentation.h"
#include <QString>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <nlohmann/json.hpp>

namespace flora {
struct ThumbnailRow {
    uint64_t resource{}, view{}, event{};
    nlohmann::json identity;
    QImage display;
    std::optional<QString> error;
};
struct ThumbnailOutput {
    uint64_t event{};
    std::map<std::string, ThumbnailRow> rows;
};
// Reads one completed report and prepares paint pixels without touching widgets.
ThumbnailOutput readThumbnailOutput(const QString &directory, const CancelCheck &cancelled = {});
} // namespace flora
