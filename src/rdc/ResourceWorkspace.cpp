#include "ResourceWorkspace.h"
#include "Assets.h"
#include "application/RdcEvents.h"
#include <QCryptographicHash>
#include <QDir>
#include <QImage>
#include <QSaveFile>
#include <charconv>
#include <cmath>

namespace flora {
namespace {
using Json = nlohmann::json;
uint64_t number(const Json &j) {
    if (!j.is_string()) throw std::runtime_error("Resource identity/range must be a decimal string");
    const auto s = j.get<std::string>();
    uint64_t value{};
    const auto [end, error] = std::from_chars(s.data(), s.data() + s.size(), value);
    if (error != std::errc{} || end != s.data() + s.size())
        throw std::runtime_error("Invalid resource identity/range");
    return value;
}
void save(const QString &path, const void *data, size_t size) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(static_cast<const char *>(data), qint64(size)) != qint64(size) ||
        !file.commit()) throw std::runtime_error("Cannot publish resource artifact");
}
}
Json readResourceWorkspace(IReplayController &controller, const Json &job, const Json &queryPolicy) {
    const auto &requests = job.at("resource_requests");
    if (!requests.is_array() || requests.empty() || requests.size() > 16)
        throw std::runtime_error("Resource read requires 1 to 16 requests");
    const QDir directory(QString::fromStdString(job.at("out").get<std::string>()));
    Json results = Json::array();
    std::map<std::string, Json> shared;
    const auto resources = controller.GetResources();
    const auto textures = controller.GetTextures();
    const auto buffers = controller.GetBuffers();
    for (size_t i = 0; i < requests.size(); ++i) {
        const auto &request = requests[i];
        auto content = request; content.erase("key");
        const auto cacheKey = content.dump();
        if (auto cached = shared.find(cacheKey); cached != shared.end()) {
            auto result = cached->second; result["request"] = request;
            results.push_back(std::move(result)); continue;
        }
        Json result{{"request", request}};
        try {
            const auto object = number(request.at("object")), creation = number(request.at("creation"));
            if (!object || !creation) throw std::runtime_error("Missing resource lifetime");
            if (!queryPolicy.is_null()) {
                auto check = job; check["action"] = "texture"; check["resource"] = request.at("object");
                check["resource_creation"] = request.at("creation");
                requireRdcQueryIsolation(queryPolicy, check);
            }
            const auto marker = "Flora DX12 resource " + std::to_string(object) + " creation " + std::to_string(creation);
            ResourceId id;
            for (const auto &r : resources) if (std::string(r.name.c_str()) == marker) {
                if (id != ResourceId()) throw std::runtime_error("Ambiguous captured resource marker");
                id = r.resourceId;
            }
            if (id == ResourceId()) throw std::runtime_error("Captured resource lifetime is absent at this boundary");
            const auto stem = QString("resource-%1").arg(i);
            if (request.value("buffer", false)) {
                const auto found = std::find_if(buffers.begin(), buffers.end(), [&](const auto &b) { return b.resourceId == id; });
                const auto offset = number(request.at("offset")), length = number(request.at("length"));
                if (found == buffers.end() || !length || length > 256 * 1024 * 1024 ||
                    offset > found->length || length > found->length - offset)
                    throw std::runtime_error("Buffer range exceeds resource or 256 MiB limit");
                const auto bytes = controller.GetBufferData(id, offset, length);
                if (bytes.size() != length) throw std::runtime_error("Incomplete buffer readback");
                save(directory.filePath(stem + ".bin"), bytes.data(), bytes.size());
                result["file"] = (stem + ".bin").toStdString();
                result["bytes"] = std::to_string(bytes.size());
                result["sha256"] = QCryptographicHash::hash(
                    QByteArrayView(reinterpret_cast<const char *>(bytes.data()), qsizetype(bytes.size())),
                    QCryptographicHash::Sha256).toHex().toStdString();
            } else {
                const auto found = std::find_if(textures.begin(), textures.end(), [&](const auto &t) { return t.resourceId == id; });
                if (found == textures.end()) throw std::runtime_error("Selected resource is not a texture");
                const auto &t = *found;
                const auto mip = request.at("mip").get<uint32_t>(), layer = request.at("layer").get<uint32_t>();
                const auto slice = request.value("slice", 0u), sample = request.at("sample").get<uint32_t>();
                if (mip >= t.mips || mip >= 32 || layer >= t.arraysize ||
                    slice >= std::max(1u, t.depth >> mip) || (sample != UINT32_MAX && sample >= t.msSamp))
                    throw std::runtime_error("Texture subresource exceeds resource");
                const uint64_t pixels = uint64_t(std::max(1u, t.width >> mip)) * std::max(1u, t.height >> mip);
                if (pixels > (512ULL * 1024 * 1024) / 16 / std::max(1u, t.depth >> mip))
                    throw std::runtime_error("Selected texture exceeds the 512 MiB display budget");
                TextureSave params;
                params.resourceId = id; params.destType = FileType::PNG; params.mip = int(mip);
                params.slice.sliceIndex = int(t.dimension == 3 ? slice : layer);
                params.sample.sampleIndex = sample;
                params.typeCast = CompType(request.value("component_type", uint32_t(CompType::Typeless)));
                const auto plane = request.value("plane", std::string("color"));
                const bool stencil = t.format.type == ResourceFormatType::D24S8 || t.format.type == ResourceFormatType::D32S8;
                if (plane == "stencil" && !stencil) throw std::runtime_error("Texture has no stencil plane");
                params.channelExtract = plane == "stencil" ? 1 : plane == "depth" ? 0 : -1;
                const auto channel = request.value("channel", std::string("RGBA"));
                if (plane == "color" && channel.size() == 1) {
                    const auto position = std::string("RGBA").find(channel);
                    if (position != std::string::npos) params.channelExtract = int(position);
                }
                params.comp.blackPoint = request.value("low", 0.f);
                params.comp.whitePoint = request.value("high", 1.f);
                if (!std::isfinite(params.comp.blackPoint) || !std::isfinite(params.comp.whitePoint) ||
                    params.comp.whitePoint <= params.comp.blackPoint)
                    throw std::runtime_error("Invalid display range");
                const auto png = directory.filePath(stem + ".png");
                const auto status = controller.SaveTexture(params, png.toStdString().c_str());
                if (status.code != ResultCode::Succeeded) throw std::runtime_error("RenderDoc could not display this texture view");
                QImage image(png);
                if (image.isNull()) throw std::runtime_error("Texture display image is missing");
                // Integer RG/R textures have no alpha component. SaveTexture can
                // export its implicit alpha value 1 as byte 1 instead of opaque
                // 255. Do not let that synthetic channel hide valid RGB data.
                const bool opaque = (channel == "RGB" && plane == "color") ||
                                    (t.format.compCount > 0 && t.format.compCount < 4);
                if (opaque) image = image.convertToFormat(QImage::Format_RGB32);
                if (request.value("thumbnail", false)) {
                    image = image.scaled(96, 96, Qt::KeepAspectRatio, Qt::SmoothTransformation);
                    QSaveFile output(png);
                    if (!output.open(QIODevice::WriteOnly) || !image.save(&output, "PNG") || !output.commit())
                        throw std::runtime_error("Cannot publish thumbnail");
                } else {
                    if (opaque) {
                        QSaveFile output(png);
                        if (!output.open(QIODevice::WriteOnly) || !image.save(&output, "PNG") || !output.commit())
                            throw std::runtime_error("Cannot publish RGB display");
                    }
                    // Preserve API-provided storage separately from the display transform.
                    // For a resolved display, the raw artifact is explicitly sample zero.
                    const auto bytes = controller.GetTextureData(id, Subresource(mip, layer, sample == UINT32_MAX ? 0 : sample));
                    if (bytes.empty() || bytes.size() > 512ULL * 1024 * 1024)
                        throw std::runtime_error("Texture storage readback is empty or exceeds 512 MiB");
                    save(directory.filePath(stem + ".bin"), bytes.data(), bytes.size());
                    result["file"] = (stem + ".bin").toStdString();
                    result["bytes"] = std::to_string(bytes.size());
                    result["sha256"] = QCryptographicHash::hash(
                        QByteArrayView(reinterpret_cast<const char *>(bytes.data()), qsizetype(bytes.size())),
                        QCryptographicHash::Sha256).toHex().toStdString();
                    result["storage_scope"] = "RenderDoc selected mip/array layer; all volume slices and packed depth/stencil components; resolve display exports raw sample zero";
                    result["storage_description"] = rdcTextureReadbackDescription(t, Subresource(mip, layer, sample == UINT32_MAX ? 0 : sample));
                }
                result["image"] = (stem + ".png").toStdString();
                result["width"] = image.width(); result["height"] = image.height();
            }
        } catch (const std::exception &e) { result["error"] = e.what(); }
        shared[cacheKey] = result;
        results.push_back(std::move(result));
    }
    return {{"resource_results", results}, {"boundary", job.at("boundary")}};
}
}
