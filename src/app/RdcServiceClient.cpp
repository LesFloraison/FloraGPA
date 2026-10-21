#include "RdcServiceClient.h"
#include <QDir>
#include <QFileInfo>
#define NOMINMAX
#include <Windows.h>

namespace flora {
using Json = nlohmann::json;
RdcServiceClient::RdcServiceClient(QObject *parent) : QObject(parent) {
    process_.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) { args->flags |= CREATE_NO_WINDOW; });
    connect(&process_, &QProcess::started, this, [this] {
        processJob_ = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        auto handle = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE, DWORD(process_.processId()));
        const bool isolated = processJob_ && handle &&
            SetInformationJobObject(processJob_, JobObjectExtendedLimitInformation, &info, sizeof info) &&
            AssignProcessToJobObject(processJob_, handle);
        if (handle) CloseHandle(handle);
        if (!isolated) { failed("Cannot isolate analysis service"); return; }
        dispatch();
    });
    connect(&process_, &QProcess::readyReadStandardOutput, this, &RdcServiceClient::read);
    connect(&process_, &QProcess::readyReadStandardError, this, [this] {
        diagnostics_ += process_.readAllStandardError();
        if (diagnostics_.size() > 65536) diagnostics_ = diagnostics_.right(65536);
    });
    connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart && !stopping_) failed(process_.errorString());
    });
    connect(&process_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, [this](int, QProcess::ExitStatus) {
        if (!stopping_) failed("Analysis service exited unexpectedly");
    });
    timeout_.setSingleShot(true);
    connect(&timeout_, &QTimer::timeout, this, [this] { failed("Analysis service timed out"); });
}
RdcServiceClient::~RdcServiceClient() { close(); }
void RdcServiceClient::stopProcess() {
    stopping_ = true;
    timeout_.stop();
    if (processJob_) { CloseHandle(processJob_); processJob_ = nullptr; }
    if (process_.state() != QProcess::NotRunning) { process_.kill(); process_.waitForFinished(3000); }
    input_.clear(); stopping_ = false;
}
void RdcServiceClient::close() {
    busy_ = false; completed_ = {}; key_.clear(); job_ = nullptr;
    stopProcess();
}
void RdcServiceClient::send(const char *command, const Json &extra) {
    Json message{{"schema", 1}, {"command", command}, {"id", std::to_string(++serial_)},
                 {"generation", std::to_string(generation_)}};
    if (extra.is_object()) message.update(extra);
    const auto bytes = QByteArray::fromStdString(message.dump() + "\n");
    if (process_.write(bytes) != bytes.size()) throw std::runtime_error("Cannot send analysis request");
}
void RdcServiceClient::start() {
    input_.clear(); diagnostics_.clear();
    timeout_.start(180000);
    process_.start(executable_, {"--serve"});
}
void RdcServiceClient::dispatch() {
    try {
        if (retrying_) { ++generation_; send("open", {{"job", job_}}); retrying_ = false; }
        send("execute", {{"job", job_}}); requestId_ = serial_;
        timeout_.start(180000);
    } catch (const std::exception &e) { failed(QString::fromUtf8(e.what())); }
}
void RdcServiceClient::submit(const QString &executable, const QString &key, const Json &job,
                              std::function<void(int, QString, bool)> completed) {
    if (busy_) throw std::runtime_error("Analysis service already has a request");
    const bool reopen = key != key_ || executable != executable_;
    job_ = job; completed_ = std::move(completed); busy_ = true; retries_ = 0;
    executable_ = executable;
    if (reopen) { stopProcess(); key_ = key; }
    retrying_ = process_.state() == QProcess::NotRunning;
    if (retrying_) start(); else dispatch();
}
void RdcServiceClient::cancelSelection() {
    if (!busy_ || process_.state() != QProcess::Running || !requestId_) return;
    try { send("cancel", {{"through", std::to_string(requestId_)}}); }
    catch (const std::exception &e) { failed(QString::fromUtf8(e.what())); }
}
void RdcServiceClient::failed(const QString &error) {
    if (stopping_) return;
    stopProcess();
    if (!busy_) { key_.clear(); return; }
    // Preserve partial evidence and give each attempt a fresh output directory.
    const QFileInfo output(QString::fromStdString(job_.at("out").get<std::string>()));
    if (output.exists()) {
        const auto destination = output.absoluteFilePath() + QString("-failed-%1").arg(retries_);
        if (!QDir().rename(output.absoluteFilePath(), destination)) {
            busy_ = false; const auto callback = std::move(completed_);
            callback(1, "Cannot retain failed analysis artifacts", false); return;
        }
    }
    if (retries_++ == 0) { retrying_ = true; start(); return; }
    busy_ = false; key_.clear();
    const auto callback = std::move(completed_); callback(1, error + "\n" + QString::fromUtf8(diagnostics_), true);
}
void RdcServiceClient::read() {
    input_ += process_.readAllStandardOutput();
    if (input_.size() > 1048576) { failed("Analysis response exceeds protocol limit"); return; }
    while (input_.contains('\n')) {
        const auto end = input_.indexOf('\n'); const auto bytes = input_.left(end); input_.remove(0, end + 1);
        try {
            const auto response = Json::parse(bytes.begin(), bytes.end());
            if (response.at("schema") != 1 || response.at("generation") != std::to_string(generation_))
                throw std::runtime_error("Analysis response generation mismatch");
            if (response.at("command") == "open" && !response.at("ok").get<bool>()) {
                failed(QString::fromStdString(response.value("error", "Cannot open analysis session"))); return;
            }
            if (response.at("command") != "execute" || response.at("id") != std::to_string(requestId_)) continue;
            timeout_.stop(); busy_ = false;
            const auto callback = std::move(completed_);
            callback(response.value("ok", false) ? 0 : 1,
                     QString::fromStdString(response.value("error", "")), false);
        } catch (const std::exception &e) { failed(QString::fromUtf8(e.what())); return; }
    }
}
}
