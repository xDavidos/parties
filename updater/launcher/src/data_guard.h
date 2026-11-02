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

class DataGuard {
public:
    explicit DataGuard(DataPaths paths) : paths_(std::move(paths)) {}

    fs::path backup_path(const SnapshotId& id) const;

    // nullopt: there is no database yet. An error means a database exists but
    // cannot be read.
    std::expected<std::optional<DataInfo>, std::string> inspect() const;

    // A consistent copy of the live database at backups\<id>.db. Done when an
    // intact copy with that name already exists.
    bool snapshot(const SnapshotId& id, std::string& error) const;

    // Keeps the live database (with its WAL) as backups\<preserve_live_as>.db,
    // then puts backups\<from>.db in its place. Done when the live database
    // already has from's content.
    bool restore(const SnapshotId& from, const SnapshotId& preserve_live_as, std::string& error) const;

    // Intact backups, newest first.
    std::vector<Backup> list() const;

    // Deletes the automatic snapshots retention allows, given the committed state.
    void prune(const SupervisorState& committed) const;

private:
    DataPaths paths_;
};

} // namespace parties::update::launcher
