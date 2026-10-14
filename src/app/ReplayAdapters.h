#pragma once
#include <QComboBox>
#include <QStringList>
#include <cstdint>
#include <optional>
namespace flora {
// The first two entries remain default hardware and WARP. Positive item data
// selects the first DXGI adapter with that PCI vendor, matching the native worker.
void populateReplayAdapters(QComboBox &combo);
std::optional<uint32_t> selectedReplayVendor(const QComboBox &combo);
void restoreReplayAdapter(QComboBox &combo, bool warp, std::optional<uint32_t> vendor);
QStringList replayAdapterArguments(const QComboBox &combo, const QString &command);
} // namespace flora
