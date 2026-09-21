#include "ExternalShaderTools.h"
#include "HlslCompilation.h"
#include "HlslRecovery.h"
#include "ShaderInspector.h"
#include "replay/Replay.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QTemporaryDir>
#include <algorithm>

namespace flora {
namespace {
using Json = nlohmann::json;
[[noreturn]] void fail(const std::string &message) { throw std::runtime_error(message); }
struct Handle {
    HANDLE value{};
    explicit Handle(HANDLE h = nullptr) : value(h == INVALID_HANDLE_VALUE ? nullptr : h) {}
    ~Handle() { close(); }
    void close() {
        if (value) {
            CloseHandle(value);
            value = nullptr;
        }
    }
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;
};
QByteArray read(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        fail("Cannot read " + path.toStdString());
    return file.readAll();
}
void save(const QString &path, const QByteArray &data) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit())
        fail("Cannot save " + path.toStdString());
}
QByteArray array(Bytes bytes) {
    return {reinterpret_cast<const char *>(bytes.data()), qsizetype(bytes.size())};
}
Bytes span(const QByteArray &bytes) {
    return {reinterpret_cast<const uint8_t *>(bytes.data()), size_t(bytes.size())};
}
QString executablePath(const QString &path) {
    QFileInfo file(path);
    if (path.isEmpty() || !file.isFile() || file.canonicalFilePath().isEmpty())
        fail("Select an existing shader tool executable");
    return QDir::toNativeSeparators(file.canonicalFilePath());
}
std::wstring quote(const QString &argument) {
    if (argument.contains(QChar(0)))
        fail("Shader tool argument contains NUL");
    // CommandLineToArgvW / CRT escaping: double backslashes before a quote
    // and before the closing quote, including paths ending in a separator.
    std::wstring result = L"\"";
    unsigned slashes = 0;
    for (const auto c : argument.toStdWString()) {
        if (c == L'\\') {
            ++slashes;
            continue;
        }
        result.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        slashes = 0;
        result += c;
    }
    result.append(slashes * 2, L'\\');
    return result + L'"';
}
Json toolIdentity(const QString &executable) {
    return {{"tool", executable.toStdString()}, {"tool_sha256", sha256(span(read(executable)))}};
}
} // namespace

void runShaderTool(const QString &executable, const QStringList &arguments, const QString &directory,
                   const QString &logName, unsigned timeoutMs) {
    if (!timeoutMs || timeoutMs == INFINITE)
        fail("Invalid shader tool timeout");
    const auto path = executablePath(executable);
    const auto working = QFileInfo(directory).canonicalFilePath();
    if (working.isEmpty() || !QFileInfo(working).isDir())
        fail("Missing shader tool output directory");
    QTemporaryDir capture(QDir::tempPath() + "/FloraGPA-tool-XXXXXX");
    if (!capture.isValid())
        fail("Cannot create shader tool log directory");
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    auto stream = [&](const QString &name) {
        return CreateFileW(reinterpret_cast<LPCWSTR>(name.utf16()), GENERIC_WRITE, FILE_SHARE_READ, &security,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    };
    const auto stdoutPath = capture.filePath("stdout.bin"), stderrPath = capture.filePath("stderr.bin");
    Handle output(stream(stdoutPath)), error(stream(stderrPath));
    Handle input(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    Handle job(CreateJobObjectW(nullptr, nullptr));
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!output.value || !error.value || !input.value || !job.value ||
        !SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
        fail("Cannot isolate shader tool process");
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<uint8_t> attributes(size);
    auto list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
    if (!InitializeProcThreadAttributeList(list, 1, 0, &size))
        fail("Cannot initialize shader tool process attributes");
    struct AttributeCleanup {
        LPPROC_THREAD_ATTRIBUTE_LIST list;
        ~AttributeCleanup() { DeleteProcThreadAttributeList(list); }
    } cleanup{list};
    HANDLE inherited[]{input.value, output.value, error.value};
    if (!UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited),
                                   nullptr, nullptr))
        fail("Cannot restrict shader tool handle inheritance");
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = input.value;
    startup.StartupInfo.hStdOutput = output.value;
    startup.StartupInfo.hStdError = error.value;
    startup.lpAttributeList = list;
    auto command = quote(path);
    for (const auto &arg : arguments)
        command += L' ' + quote(arg);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(reinterpret_cast<LPCWSTR>(path.utf16()), command.data(), nullptr, nullptr, TRUE,
                        CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr,
                        reinterpret_cast<LPCWSTR>(working.utf16()), &startup.StartupInfo, &process))
        fail("Cannot start shader tool (Windows error " + std::to_string(GetLastError()) + ")");
    Handle processHandle(process.hProcess), thread(process.hThread);
    if (!AssignProcessToJobObject(job.value, processHandle.value)) {
        TerminateProcess(processHandle.value, 1);
        WaitForSingleObject(processHandle.value, 5000);
        fail("Cannot assign shader tool to process isolation");
    }
    if (ResumeThread(thread.value) == DWORD(-1))
        fail("Cannot resume shader tool");
    const auto wait = WaitForSingleObject(processHandle.value, timeoutMs);
    DWORD exitCode = 1;
    if (wait != WAIT_OBJECT_0) {
        TerminateJobObject(job.value, 1);
        WaitForSingleObject(processHandle.value, 5000);
    } else if (!GetExitCodeProcess(processHandle.value, &exitCode))
        exitCode = 1;
    // No background descendants survive completion, timeout or outer worker cancellation.
    job.close();
    output.close();
    error.close();
    input.close();
    save(QDir(directory).filePath(logName), read(stdoutPath) + '\n' + read(stderrPath));
    if (wait == WAIT_TIMEOUT)
        fail("Shader tool timed out; inspect " + logName.toStdString());
    if (wait != WAIT_OBJECT_0 || exitCode)
        fail("Shader tool failed; inspect " + logName.toStdString());
}

Json exportRecoveredHlsl(Bytes bytecode, const QString &directory, const QString &executable,
                         const Json &savedSource, unsigned timeoutMs) {
    const QDir out(QFileInfo(directory).absoluteFilePath());
    save(out.filePath("reconstructed.dxbc"), array(bytecode));
    std::string nativeFailure;
    std::optional<RecoveredHlsl> native;
    try {
        native = reconstructHlsl(bytecode, savedSource);
    } catch (const std::exception &e) {
        nativeFailure = e.what();
    }
    if (native) {
        auto source = QByteArray::fromStdString(native->source);
        if (native->report.at("source_kind") != "saved_applied_hlsl")
            source.replace("\n", "\r\n");
        save(out.filePath("reconstructed.hlsl"), source);
        save(out.filePath("reconstructed.recompiled.dxbc"), array(native->recompiled));
        return native->report;
    }
    if (executable.isEmpty())
        fail("Native HLSL recovery unavailable: " + nativeFailure +
             "; use an external decompiler or exact DXBC assembly editing");
    const auto tool = executablePath(executable);
    runShaderTool(tool, {"-D", out.filePath("reconstructed.dxbc")}, out.absolutePath(), "decompiler.log",
                  timeoutMs);
    if (!QFileInfo::exists(out.filePath("reconstructed.hlsl")))
        fail("External decompiler failed; inspect decompiler.log");
    auto report = toolIdentity(tool);
    report.update({{"source_kind", "reconstructed_hlsl_not_original"},
                   {"native", false},
                   {"native_failure", nativeFailure},
                   {"semantic_equivalence", "not_verified"}});
    const auto source = read(out.filePath("reconstructed.hlsl"));
    std::optional<HlslCompilation> compiled;
    try {
        compiled =
            compileHlsl(source.toStdString(), inspectShader(bytecode).at("profile").get<std::string>());
    } catch (const std::exception &e) {
        report.update({{"recompiles", false}, {"diagnostics", e.what()}});
    }
    if (compiled) {
        save(out.filePath("reconstructed.recompiled.dxbc"), array(compiled->bytecode));
        report.update({{"recompiles", true}, {"diagnostics", compiled->diagnostics}});
    }
    return report;
}
Json assembleShader(Bytes reference, const QByteArray &source, const QString &directory,
                    const QString &executable, unsigned timeoutMs) {
    const QDir out(QFileInfo(directory).absoluteFilePath());
    const auto tool = executablePath(executable);
    const auto original = inspectShader(reference);
    if (original.at("stage") == "signature")
        fail("Output signature only; no executable shader assembly to edit");
    save(out.filePath("edited.asm"), source);
    save(out.filePath("original.dxbc"), array(reference));
    runShaderTool(tool,
                  {"-a", "--copy-reflection", out.filePath("original.dxbc"), out.filePath("edited.asm")},
                  out.absolutePath(), "assembler.log", timeoutMs);
    if (!QFileInfo::exists(out.filePath("edited.shdr")))
        fail("Assembler failed; inspect assembler.log");
    const auto bytes = read(out.filePath("edited.shdr"));
    auto report = inspectShader(span(bytes));
    if (report.at("stage") != original.at("stage"))
        fail("Assembled shader stage changed");
    report.update(toolIdentity(tool));
    report["original_sha256"] = sha256(reference);
    report["replacement_sha256"] = sha256(span(bytes));
    save(out.filePath("replacement.dxbc"), bytes);
    return report;
}
} // namespace flora
