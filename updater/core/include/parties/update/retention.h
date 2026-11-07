#pragma once

// Which database copies under backups\ the launcher may delete. Only copies
// taken automatically before a build opens the data are ever deleted; every
// copy that holds data the user would otherwise lose is kept forever.

#include <parties/update/supervisor_state.h>

#include <chrono>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace parties::update {

enum class BackupKind : uint8_t {
    // "pre-<build>-g<n>" (the state machine, before a build first opens the
    // data) and "pre-migration-v<n>-<utc>" (the app, before it migrates).
    Automatic,
    // Everything else: rolled-back-*, pre-rollback-*, pre-restore-*,
    // pre-downgrade-*, legacy-*, pre-uninstall-*, and any name not listed above.
    Preserved,
};

BackupKind classify_backup(std::string_view stem);

struct BackupFile {
    std::string stem;  // backups\<stem>.db
    std::chrono::file_clock::time_point written;
};

inline constexpr size_t kAutomaticBackupsKept = 10;

// The stems to delete: automatic copies older than the newest `keep`, except
// any copy the state still refers to (a pending restore, the pre-candidate
// snapshot of a probation, the copy a notice names).
std::vector<std::string> backups_to_prune(std::span<const BackupFile> backups, const SupervisorState& state,
                                          size_t keep = kAutomaticBackupsKept);

} // namespace parties::update
