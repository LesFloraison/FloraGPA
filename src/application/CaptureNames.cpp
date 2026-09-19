#include "CaptureNames.h"
namespace flora {
QString debugDisplayName(Bytes bytes) {
    if (!bytes.empty() && bytes.back() == 0)
        bytes = bytes.first(bytes.size() - 1);
    QString text;
    auto escaped = [&](uint8_t b) { text += QString("\\x%1").arg(b, 2, 16, QChar('0')); };
    for (size_t i = 0; i < bytes.size();) {
        auto b = bytes[i];
        if (b < 128) {
            if (b < 32 || b == 127)
                escaped(b);
            else
                text += QChar(b);
            ++i;
            continue;
        }
        unsigned n = b >= 0xc2 && b <= 0xdf ? 2 : b >= 0xe0 && b <= 0xef ? 3 : b >= 0xf0 && b <= 0xf4 ? 4 : 0;
        uint32_t code = n ? b & ((1u << (7 - n)) - 1) : 0;
        bool valid = n && n <= bytes.size() - i;
        if (valid)
            for (unsigned k = 1; k < n; ++k) {
                if ((bytes[i + k] & 0xc0) != 0x80) {
                    valid = false;
                    break;
                }
                code = (code << 6) | (bytes[i + k] & 63);
            }
        if (valid)
            valid = code < 0x110000 && !(code >= 0xd800 && code <= 0xdfff) && (n != 3 || code >= 0x800) &&
                    (n != 4 || code >= 0x10000);
        if (valid) {
            text += QString::fromUtf8(reinterpret_cast<const char *>(bytes.data() + i), n);
            i += n;
        } else {
            escaped(b);
            ++i;
        }
    }
    return text;
}
nlohmann::json capturedNames(const Frame &frame) {
    using Json = nlohmann::json;
    const std::array<uint8_t, 16> debugGuid{0x22, 0x8c, 0x9b, 0x42, 0x88, 0x91, 0x0c, 0x4b,
                                            0x87, 0x42, 0xac, 0xb0, 0xbf, 0x85, 0xc2, 0x00};
    Json records = Json::array(), issues = Json::array();
    for (const auto &[id, entry] : frame.entries()) {
        if (entry.category != 9 || entry.type != 0x103)
            continue;
        try {
            Reader r(frame.payload(id));
            auto owner = r.read<Id>();
            auto guid = r.array<uint8_t, 16>();
            auto bytes = r.take(r.read<uint32_t>());
            r.end();
            if (guid != debugGuid)
                continue;
            if (frame.entry(owner).category != 5)
                throw std::runtime_error("Debug name owner is not a captured resource");
            records.push_back({{"record_id", std::to_string(id)},
                               {"resource_id", std::to_string(owner)},
                               {"name", debugDisplayName(bytes).toStdString()},
                               {"data_hex", QByteArray(reinterpret_cast<const char *>(bytes.data()),
                                                       qsizetype(bytes.size()))
                                                .toHex()
                                                .toStdString()}});
        } catch (const std::exception &error) {
            issues.push_back({{"record_id", std::to_string(id)}, {"error", error.what()}});
        }
    }
    return {{"records", records},
            {"issues", issues},
            {"note", "Capture metadata records do not establish an event rename timeline."}};
}
} // namespace flora
