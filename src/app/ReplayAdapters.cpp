#include "ReplayAdapters.h"
#include "application/ReplayCommands.h"
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <set>
#include <stdexcept>
namespace flora {
namespace {
QString vendorLabel(uint32_t vendor) {
    switch (vendor) {
    case 0x8086: return "Intel · DX11";
    case 0x10de: return "NVIDIA · DX11";
    case 0x1002: return "AMD · DX11";
    default: return QString("PCI 0x%1 · DX11").arg(vendor, 4, 16, QChar('0'));
    }
}
void addVendor(QComboBox &combo, uint32_t vendor, const QString &description) {
    combo.addItem(vendorLabel(vendor), vendor);
    combo.setItemData(combo.count() - 1,
                      QString("%1\nFirst DXGI adapter with PCI vendor 0x%2. No fallback.")
                          .arg(description).arg(vendor, 4, 16, QChar('0')),
                      Qt::ToolTipRole);
}
} // namespace
void populateReplayAdapters(QComboBox &combo) {
    combo.clear();
    combo.addItem("Hardware · DX11", 0u);
    combo.addItem("WARP · DX11", 0u);
    combo.setToolTip("Native replay adapter. Intel metric collection selects its own Intel device.");
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    const auto created = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(created)) {
        combo.setToolTip(QString("Adapter enumeration failed (0x%1). Default hardware and WARP remain available.")
                             .arg(uint32_t(created), 8, 16, QChar('0')));
        return;
    }
    // Use the same enumeration range and first-vendor rule as createDx11Device.
    std::set<UINT> seen;
    for (UINT index = 0; index < 16; ++index) {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        const auto result = factory->EnumAdapters1(index, &adapter);
        if (result == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(result)) {
            combo.setToolTip("Adapter enumeration is incomplete. Unavailable selections fail without fallback.");
            break;
        }
        DXGI_ADAPTER_DESC1 desc{};
        if (FAILED(adapter->GetDesc1(&desc))) {
            combo.setToolTip("An adapter description is unavailable. Unavailable selections fail without fallback.");
            continue;
        }
        if (!desc.VendorId || desc.VendorId > 0xffff || !seen.insert(desc.VendorId).second ||
            (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) continue;
        addVendor(combo, desc.VendorId, QString::fromWCharArray(desc.Description));
    }
}
std::optional<uint32_t> selectedReplayVendor(const QComboBox &combo) {
    const auto vendor = combo.currentData().toUInt();
    return vendor ? std::optional<uint32_t>(vendor) : std::nullopt;
}
void restoreReplayAdapter(QComboBox &combo, bool warp, std::optional<uint32_t> vendor) {
    if (vendor && (warp || !*vendor || *vendor > 0xffff))
        throw std::invalid_argument("Invalid replay GPU vendor selection");
    if (!vendor) { combo.setCurrentIndex(warp ? 1 : 0); return; }
    auto index = combo.findData(*vendor);
    if (index < 0) {
        addVendor(combo, *vendor, "Saved adapter is not currently enumerated");
        index = combo.count() - 1;
    }
    combo.setCurrentIndex(index);
}
QStringList replayAdapterArguments(const QComboBox &combo, const QString &command) {
    if (!usesNativeReplayDevice(command.toStdString())) return {};
    if (combo.currentIndex() == 1) return {"--warp"};
    if (const auto vendor = selectedReplayVendor(combo))
        return {"--gpu-vendor", QString::number(*vendor)};
    return {};
}
} // namespace flora
