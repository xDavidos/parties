#include "app_process.h"

#include "file_io.h"
#include "log.h"

#include <windows.h>

namespace parties::update::launcher {

namespace {

// Our own TerminateProcess code; exit_kind() reports it as KilledHung through
// killed_, never through the code, which a crash could share.
constexpr UINT kTerminatedByLauncher = 0xE0DE0001;

} // namespace

ExitKind classify_exit_code(unsigned long code) {
    switch (code) {
    case 0: return ExitKind::Clean;
    case 1: return ExitKind::Terminated;
    case 2: return ExitKind::InitFailed;
    case 3: return ExitKind::DataUnavailable;
    case 4: return ExitKind::DataTooNew;
    default: return ExitKind::Abnormal;
    }
}

std::wstring quote_argument(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) return arg;
    std::wstring out = L"\"";
    size_t backslashes = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        if (c == L'"') out.append(backslashes * 2 + 1, L'\\');
        else out.append(backslashes, L'\\');
        backslashes = 0;
        out.push_back(c);
    }
    out.append(backslashes * 2, L'\\');
    out.push_back(L'"');
    return out;
}

AppJob::AppJob() {
    job_ = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job_ || !SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &limits, sizeof limits))
        log("cannot create the app job object: {}", last_error_text());
}

AppJob::~AppJob() {
    if (job_) CloseHandle(job_);
}

std::expected<AppProcess, std::string> AppProcess::spawn(const AppJob& job, ExecLease lease,
                                                         const std::vector<std::wstring>& args,
                                                         const fs::path& working_dir) {
    std::wstring command = quote_argument(lease.exe().wstring());
    for (const std::wstring& a : args) command += L" " + quote_argument(a);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(lease.exe().c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr,
                        working_dir.c_str(), &si, &pi))
        return std::unexpected("cannot start " + utf8(lease.exe()) + ": " + last_error_text());
    if (!AssignProcessToJobObject(job.handle(), pi.hProcess)) {
        std::string why = last_error_text();
        TerminateProcess(pi.hProcess, kTerminatedByLauncher);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return std::unexpected("cannot put the app in its job object: " + why);
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    log("started {} (pid {})", utf8(lease.exe()), pi.dwProcessId);
    return AppProcess(pi.hProcess, pi.dwProcessId);
}

AppProcess::AppProcess(AppProcess&& other) noexcept
    : process_(other.process_), pid_(other.pid_), killed_(other.killed_) {
    other.process_ = nullptr;
}

AppProcess::~AppProcess() {
    if (process_) CloseHandle(process_);
}

void AppProcess::terminate() {
    killed_ = true;
    TerminateProcess(process_, kTerminatedByLauncher);
    WaitForSingleObject(process_, INFINITE);
}

ExitKind AppProcess::exit_kind() const {
    if (killed_) return ExitKind::KilledHung;
    DWORD code = 0;
    if (!GetExitCodeProcess(process_, &code)) return ExitKind::Abnormal;
    log("app exited with code {:#x}", code);
    return classify_exit_code(code);
}

} // namespace parties::update::launcher
