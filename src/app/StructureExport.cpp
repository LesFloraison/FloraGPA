#include "StructureExport.h"
#include <QSaveFile>
namespace flora {
namespace {
class Writer final : public nlohmann::detail::output_adapter_protocol<char> {
  public:
    Writer(QSaveFile &file, const CancelCheck &cancelled) : file_(file), cancelled_(cancelled) { buffer_.reserve(limit); }
    void write_character(char value) override { write_characters(&value, 1); }
    void write_characters(const char *data, size_t length) override {
        while (length) {
            const auto count = std::min<size_t>(length, size_t(limit - buffer_.size()));
            buffer_.append(data, qsizetype(count)); data += count; length -= count;
            if (buffer_.size() == limit) flush();
        }
    }
    void flush() {
        checkCancellation(cancelled_);
        if (!buffer_.isEmpty() && file_.write(buffer_) != buffer_.size()) throw std::runtime_error("Cannot stage complete structure export");
        buffer_.resize(0);
    }
  private:
    static constexpr qsizetype limit = 1024 * 1024;
    QSaveFile &file_;
    const CancelCheck &cancelled_;
    QByteArray buffer_;
};
}
void exportStructureFile(const QString &path, const nlohmann::json &document, const CancelCheck &cancelled) {
    checkCancellation(cancelled);
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) throw std::runtime_error("Cannot stage structure export");
    auto writer = std::make_shared<Writer>(file, cancelled);
    nlohmann::detail::serializer<nlohmann::json> serializer(writer, ' ');
    serializer.dump(document, true, false, 2);
    writer->write_character('\n'); writer->flush(); checkCancellation(cancelled);
    if (!file.commit()) throw std::runtime_error("Cannot publish structure export");
}
}
