#pragma once
#include <QDir>
#include <QFileInfo>
#include <QSaveFile>
#include <nlohmann/json.hpp>

namespace flora::testing {
// Test evidence only. Never replace a file that a progress observer may have
// opened without Windows delete-sharing. Each snapshot and final report is new.
class RecoveryJournal {
    QString final_, directory_, error_;
    qulonglong sequence_ = 0;
    bool initialized_ = false, finished_ = false;

    bool writeNew(const QString &path, const nlohmann::json &value) {
        if (QFileInfo::exists(path)) {
            error_ = "Journal output already exists: " + path;
            return false;
        }
        const auto bytes = value.dump(2);
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly) ||
            file.write(bytes.data(), qint64(bytes.size())) != qint64(bytes.size()) || !file.commit()) {
            error_ = "Cannot save journal " + path + ": " + file.errorString();
            return false;
        }
        return true;
    }

  public:
    explicit RecoveryJournal(QString path) : final_(std::move(path)), directory_(final_ + ".progress") {}
    const QString &error() const { return error_; }
    bool save(const nlohmann::json &state) {
        if (final_.isEmpty()) return true;
        error_.clear();
        if (finished_) {
            error_ = "Journal is already complete";
            return false;
        }
        if (!initialized_) {
            if (QFileInfo::exists(final_) || QFileInfo::exists(directory_) || !QDir().mkpath(directory_)) {
                error_ = "Journal destination exists or cannot be created: " + directory_;
                return false;
            }
            initialized_ = true;
        }
        if (state.value("completed", false)) {
            finished_ = writeNew(final_, state);
            return finished_;
        }
        auto snapshot = nlohmann::json::object();
        for (auto it = state.begin(); it != state.end(); ++it)
            if (it.key() != "observations") snapshot[it.key()] = it.value();
        const auto &rows = state.at("observations");
        if (!rows.empty()) snapshot["last_observation"] = rows.back();
        snapshot["sequence"] = sequence_ + 1;
        const auto path = QDir(directory_).filePath(QString("%1.json").arg(sequence_ + 1, 8, 10, QChar('0')));
        if (!writeNew(path, snapshot)) return false;
        ++sequence_;
        return true;
    }
};
} // namespace flora::testing
