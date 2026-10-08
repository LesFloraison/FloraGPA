#pragma once
#include <Windows.h>
#include <Psapi.h>
#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QSaveFile>
#include <QWindow>
#include <QWidget>
#include <algorithm>
#include <array>
#include <nlohmann/json.hpp>
#include <vector>

namespace flora::testing {
// Current-process metadata only. No window titles, other-process contents,
// native-window creation, allocation hooks or changes to production behavior.
// These observations are sequential, not an atomic allocation-owner census.
class GuiResourceSnapshot {
    using Json = nlohmann::json;
    struct Windows {
        Json rows = Json::array();
        bool complete = true;
    };
    static BOOL CALLBACK window(HWND handle, LPARAM value) noexcept {
        auto &out = *reinterpret_cast<Windows *>(value);
        DWORD pid = 0;
        const auto thread = GetWindowThreadProcessId(handle, &pid);
        if (pid != GetCurrentProcessId()) return TRUE;
        try {
            std::array<wchar_t, 256> name{};
            const auto size = GetClassNameW(handle, name.data(), int(name.size()));
            if (!size || size == int(name.size()) - 1) { out.complete = false; return TRUE; }
            out.rows.push_back({{"handle", uintptr_t(handle)}, {"thread", thread},
                {"class", QString::fromWCharArray(name.data(), size).toStdString()},
                {"visible", IsWindowVisible(handle) != FALSE}});
        } catch (...) { out.complete = false; return FALSE; }
        return TRUE;
    }
  public:
    static Json counts() {
        DWORD handles = 0;
        if (!GetProcessHandleCount(GetCurrentProcess(), &handles))
            throw std::runtime_error("Cannot count GUI probe process handles");
        SetLastError(0);
        const auto gdi = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
        const auto gdiError = gdi ? 0 : GetLastError();
        SetLastError(0);
        const auto user = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
        const auto userError = user ? 0 : GetLastError();
        if (gdiError || userError) throw std::runtime_error("Cannot count GUI probe resources");
        return {{"handles", handles}, {"gdi_objects", gdi}, {"user_objects", user}};
    }
    static Json capture() {
        const auto before = counts();
        Windows native;
        const bool enumerated = EnumWindows(window, reinterpret_cast<LPARAM>(&native)) != FALSE;
        std::sort(native.rows.begin(), native.rows.end(), [](const auto &a, const auto &b) {
            return a.at("handle").template get<uintptr_t>() < b.at("handle").template get<uintptr_t>();
        });
        Json widgets = Json::array(), windows = Json::array(), modules = Json::array();
        for (auto widget : QApplication::topLevelWidgets()) {
            widgets.push_back({{"class", widget->metaObject()->className()},
                {"object_name", widget->objectName().toStdString()}, {"visible", widget->isVisible()},
                {"native_created", widget->testAttribute(Qt::WA_WState_Created)},
                {"window_type", int(widget->windowType())}});
        }
        for (auto value : QGuiApplication::allWindows())
            windows.push_back({{"class", value->metaObject()->className()},
                {"object_name", value->objectName().toStdString()}, {"visible", value->isVisible()},
                {"exposed", value->isExposed()}, {"has_parent", value->parent() != nullptr}});
        std::sort(widgets.begin(), widgets.end());
        std::sort(windows.begin(), windows.end());
        std::array<HMODULE, 2048> handles{};
        DWORD needed = 0;
        bool modulesComplete = EnumProcessModulesEx(GetCurrentProcess(), handles.data(), DWORD(sizeof(handles)),
                                                    &needed, LIST_MODULES_ALL) &&
                               needed <= sizeof(handles) && needed % sizeof(HMODULE) == 0;
        if (modulesComplete) {
            for (size_t i = 0; i < needed / sizeof(HMODULE); ++i) {
                std::array<wchar_t, 32768> path{};
                const auto size = GetModuleFileNameW(handles[i], path.data(), DWORD(path.size()));
                if (!size || size >= path.size()) { modulesComplete = false; continue; }
                const auto text = QString::fromWCharArray(path.data(), int(size));
                modules.push_back({{"name", QFileInfo(text).fileName().toStdString()}, {"path", text.toStdString()}});
            }
        }
        std::sort(modules.begin(), modules.end());
        return {{"schema", "FloraGPA GUI resource snapshot 1"}, {"process_id", GetCurrentProcessId()},
            {"qt_platform", QGuiApplication::platformName().toStdString()},
            {"before", before}, {"after", counts()}, {"native_windows_complete", enumerated && native.complete},
            {"modules_complete", modulesComplete}, {"native_top_windows", native.rows},
            {"qt_top_widgets", widgets}, {"qt_windows", windows}, {"modules", modules},
            {"scope", "Current process top-level desktop windows and loaded code modules; child/message-only windows, data-file modules and allocation ownership are not enumerated. Handles may be reused."}};
    }
    static Json save(const QString &path) {
        if (QFileInfo::exists(path)) throw std::runtime_error("GUI snapshot already exists");
        auto value = capture();
        const auto bytes = value.dump(2);
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes.data(), qint64(bytes.size())) != qint64(bytes.size()) ||
            !file.commit()) throw std::runtime_error("Cannot save GUI resource snapshot");
        return {{"before", value["before"]}, {"after", value["after"]},
            {"native_windows_complete", value["native_windows_complete"]},
            {"modules_complete", value["modules_complete"]}};
    }
};
} // namespace flora::testing
