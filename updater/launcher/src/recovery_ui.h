#pragma once

// Native TaskDialog recovery. It runs exactly when the app cannot, so it
// depends on nothing but Windows. Choices go back into step() as events.

#include "data_guard.h"

#include <parties/update/probation.h>

#include <functional>
#include <optional>
#include <variant>
#include <vector>

namespace parties::update::launcher {

struct StartAgain {};
// restore: the backup the build starts on, when it cannot read the live data.
struct StartBuild {
    BuildId build;
    std::optional<Snapshot> restore;
};
struct RetryBuild { BuildId build; };
struct RestoreBackup { Snapshot snapshot; };
struct Quit {};
using RecoveryChoice = std::variant<StartAgain, StartBuild, RetryBuild, RestoreBackup, Quit>;

struct RecoveryRequest {
    std::optional<RecoveryReason> why;  // nullopt: opened with --recover
    const SupervisorState& state;
    std::optional<DataInfo> live_db;  // nullopt: no database
    std::vector<Backup> backups;      // newest first
    fs::path data_dir;
    std::function<void()> on_shown;  // the dialog is on screen
};

RecoveryChoice show_recovery(const RecoveryRequest& request);

// The events that carry out a choice, in order.
std::vector<Event> events_for(const RecoveryChoice& choice);

} // namespace parties::update::launcher
