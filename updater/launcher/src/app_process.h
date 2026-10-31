#pragma once

// One app process inside the launcher's kill-on-close job object, so an app
// can never outlive the launcher and run while the launcher restores data.

#include "build_store.h"

#include <parties/update/probation.h>

#include <expected>
#include <string>
#include <vector>

namespace parties::update::launcher {

// contracts.md, "App exit codes".
ExitKind classify_exit_code(unsigned long code);

// Quotes one argument so CommandLineToArgvW returns it unchanged.
std::wstring quote_argument(const std::wstring& arg);

class AppJob {
public:
    AppJob();
    ~AppJob();
    AppJob(const AppJob&) = delete;
    AppJob& operator=(const AppJob&) = delete;

    void* handle() const { return job_; }

private:
    void* job_ = nullptr;
};

class AppProcess {
public:
    // Starts `lease`'s executable suspended, assigns it to the job, then lets
    // it run. The lease is released only after CreateProcess has mapped the
    // image.
    static std::expected<AppProcess, std::string> spawn(const AppJob& job, ExecLease lease,
                                                        const std::vector<std::wstring>& args,
                                                        const fs::path& working_dir);

    AppProcess(AppProcess&& other) noexcept;
    AppProcess& operator=(AppProcess&&) = delete;
    ~AppProcess();

    void* handle() const { return process_; }
    unsigned long pid() const { return pid_; }

    // Kills the process and waits for it.
    void terminate();
    // Valid once handle() is signalled.
    ExitKind exit_kind() const;

private:
    AppProcess(void* process, unsigned long pid) : process_(process), pid_(pid) {}

    void* process_;
    unsigned long pid_;
    bool killed_ = false;
};

} // namespace parties::update::launcher
