#pragma once

// The launcher's only access to user data. Owns backups\ and their retention
// (parties/update/retention.h). Every method runs only while no app process
// exists, so the launcher and the app never write the database at the same
// time.

#include "paths.h"

#include <parties/update/supervisor_state.h>

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace parties::update::launcher {

struct Backup {
    SnapshotId id;
    DataInfo data;
    fs::file_time_type written;
};

struct RestoreError {
    std::string why;
    bool live_changed = false;  // false: the live database is exactly as it was
};

class DataGuard {
public:
    explicit DataGuard(DataPaths paths) : paths_(std::move(paths)) {}

    fs::path backup_path(const SnapshotId& id) const;

    std::expected<std::optional<DataInfo>, std::string> inspect() const;

    // A consistent copy of the live database at backups\<id>.db. Done when an
    // intact copy with that name already exists.
    bool snapshot(const SnapshotId& id, std::string& error) const;

    // Keeps the live database (with its WAL) as backups\<preserve_live_as>.db,
    // then puts backups\<from>.db in its place, or leaves no database when
    // `from` is nullopt. Done when the live database already has from's
    // content, or is gone.
    std::expected<void, RestoreError> restore(const std::optional<SnapshotId>& from,
                                              const SnapshotId& preserve_live_as) const;

    bool intact(const SnapshotId& id) const;

    // Intact backups, newest first.
    std::vector<Backup> list() const;

    void prune(const SupervisorState& committed) const;

private:
    DataPaths paths_;
};

} // namespace parties::update::launcher
