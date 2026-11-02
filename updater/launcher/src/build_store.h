#pragma once

// Sole owner of builds\. A directory named builds\<version>-<sha8> is
// complete by construction: it is assembled as builds\.staging-<name>,
// verified file by file against the signed entry, and only then renamed.

#include "paths.h"
#include "update_source.h"

#include <parties/update/supervisor_state.h>

#include <atomic>
#include <expected>
#include <optional>
#include <string>

namespace parties::update::launcher {

// The verified app executable, held open with read-only sharing so nobody can
// change it between the hash check and CreateProcess. Move-only.
class ExecLease {
public:
    ExecLease(ExecLease&& other) noexcept;
    ExecLease& operator=(ExecLease&&) = delete;
    ~ExecLease();

    const fs::path& exe() const { return exe_; }

private:
    friend class BuildStore;
    ExecLease(void* handle, fs::path exe) : handle_(handle), exe_(std::move(exe)) {}

    void* handle_;
    fs::path exe_;
};

class BuildStore {
public:
    explicit BuildStore(InstallPaths install) : install_(std::move(install)) {}

    fs::path dir_of(const BuildId& id) const { return install_.builds() / id.dir_name(); }

    // The record of a sealed directory, if its build.json and every listed
    // file check out and the directory has the name its contents imply.
    std::expected<BuildRecord, std::string> adopt(const fs::path& dir) const;

    // Fetches, verifies and seals entry's files. Re-running it for a build
    // that is already sealed and intact returns the same record.
    std::expected<BuildRecord, std::string> seal(const BuildEntry& entry, UpdateSource& source,
                                                 const Progress& progress, const std::atomic<bool>& cancel) const;

    // nullopt: the app executable is missing or does not match the record.
    std::optional<ExecLease> lease_app(const BuildRecord& record) const;

    // Refuses every build the committed state still needs.
    bool remove(const BuildId& id, const SupervisorState& committed, std::string& error) const;

    // Deletes leftovers (.staging-*, .trash-*) and sealed directories the
    // committed state does not track. Never touches `keep_dir`, the running
    // launcher's own directory. Run only while nothing is being sealed.
    void collect_garbage(const SupervisorState& committed, const fs::path& keep_dir) const;

private:
    InstallPaths install_;
};

} // namespace parties::update::launcher
