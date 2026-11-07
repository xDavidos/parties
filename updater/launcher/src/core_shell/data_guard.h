#pragma once

// The launcher's only access to user data. Owns backups and their retention
// (parties/update/retention.h). Every method runs only while no app process
// exists, so the launcher and the app never write the database at the same
// time. On macOS the caller proves that by holding data.lock.

#include "executor.h"

#include <parties/update/supervisor_state.h>

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace parties::update::launcher {

namespace fs = std::filesystem;

// Where the live database and its backups are. Windows keeps the database in
// a data\ subdirectory; macOS keeps it directly in the data root.
struct DataFiles {
    fs::path db;
    fs::path backups;
};

class DataGuard {
public:
    explicit DataGuard(DataFiles files) : files_(std::move(files)) {}

    fs::path backup_path(const SnapshotId& id) const;

    std::expected<std::optional<DataInfo>, std::string> inspect() const;

    // A consistent copy of the live database at backups/<id>.db. Done when an
    // intact copy with that name already exists.
    bool snapshot(const SnapshotId& id, std::string& error) const;

    // Keeps the live database (with its WAL) as backups/<preserve_live_as>.db,
    // then puts backups/<from>.db in its place, or leaves no database when
    // `from` is nullopt. Done when the live database already has from's
    // content, or is gone.
    std::expected<void, RestoreError> restore(const std::optional<SnapshotId>& from,
                                              const SnapshotId& preserve_live_as) const;

    bool intact(const SnapshotId& id) const;

    // Intact backups, newest first.
    std::vector<Backup> list() const;

    void prune(const SupervisorState& committed) const;

private:
    DataFiles files_;
};

} // namespace parties::update::launcher
