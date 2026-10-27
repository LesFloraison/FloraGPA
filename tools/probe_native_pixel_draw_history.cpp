#include "application/NativePixelDrawHistory.h"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <iostream>
using namespace flora;
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    try {
        const auto a = app.arguments();
        if (a.size() != 12)
            throw std::runtime_error("Usage: FloraNativePixelDrawHistoryProbe FRAME EVENT RESOURCE X Y MIP "
                                     "LAYER SLICE SAMPLE VENDOR NEW_DIRECTORY");
        auto number = [&](int i) {
            bool ok = false;
            const auto n = a[i].toULongLong(&ok, 0);
            if (!ok)
                throw std::runtime_error("Invalid pixel history probe number");
            return n;
        };
        for (int i = 4; i <= 10; ++i)
            if (number(i) > UINT32_MAX)
                throw std::runtime_error("Pixel selection exceeds uint32");
        QDir dir(a[11]);
        if (dir.exists() || !QDir().mkpath(a[11]))
            throw std::runtime_error("Pixel history probe directory must be new");
        auto save = [&](const QString &name, Bytes bytes) {
            QFile f(dir.filePath(name));
            if (!f.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
                f.write(reinterpret_cast<const char *>(bytes.data()), qint64(bytes.size())) !=
                    qint64(bytes.size()))
                throw std::runtime_error("Cannot save pixel history probe evidence");
        };
        QElapsedTimer timer;
        timer.start();
        const CancelCheck cancel = [&] { return timer.elapsed() > 120000; };
        Frame f(a[1].toStdWString());
        const Id event = number(2);
        const auto vendor = uint32_t(number(10));
        ReplayOptions o;
        o.debug = true;
        o.until = event;
        o.before = o.prepareBeforeDraw = true;
        o.warp = !vendor;
        if (vendor)
            o.vendor = vendor;
        Replay r(f, o);
        r.run([&](Id, size_t, size_t) { checkCancellation(cancel); });
        NativeHistorySelection s;
        s.resource = number(3);
        s.x = uint32_t(number(4));
        s.y = uint32_t(number(5));
        s.mip = uint32_t(number(6));
        s.layer = uint32_t(number(7));
        s.slice = uint32_t(number(8));
        s.sample = uint32_t(number(9));
        if (!s.resource)
            r.inspectNativeState([&](auto *context, const auto &objects) {
                Com<ID3D11RenderTargetView> view;
                context->OMGetRenderTargets(1, &view, nullptr);
                if (!view)
                    throw std::runtime_error("Pixel probe requires color target zero when resource is zero");
                Com<ID3D11Resource> resource;
                view->GetResource(&resource);
                for (const auto &[id, object] : objects) {
                    Com<ID3D11Resource> candidate;
                    if (SUCCEEDED(object.As(&candidate)) && candidate == resource) {
                        s.resource = id;
                        break;
                    }
                }
            });
        NativePixelDrawHistoryLimits limits;
        limits.timeoutMs = 60000;
        auto report = collectNativePixelDrawHistory(r, event, s, limits, cancel);
        o.before = o.prepareBeforeDraw = false;
        Replay reference(f, o);
        reference.run([&](Id, size_t, size_t) { checkCancellation(cancel); });
        auto expected = reference.readTexturePoint(s.resource, s.x, s.y, s.slice, s.mip, s.layer);
        if (!expected)
            throw std::runtime_error("Real pixel probe requires a narrow non-MSAA color reference");
        save("reference.bin", *expected);
        const auto actual = report.at("after").at("bytes").get<std::vector<uint8_t>>();
        save("private.bin", actual);
        const bool matched = *expected == actual && report.at("original_storage_restored") == true &&
                             report.at("unattributed_changes").empty();
        report["probe"] = {{"status", matched ? "ok" : "different"},
                           {"reference_sha256", sha256(*expected)},
                           {"private_sha256", sha256(actual)},
                           {"elapsed_ms", timer.elapsed()}};
        const auto text = report.dump(2);
        save("report.json", Bytes(reinterpret_cast<const uint8_t *>(text.data()), text.size()));
        return matched ? 0 : 2;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
