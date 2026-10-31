#pragma once

// Native TaskDialog recovery. It runs exactly when the app cannot, so it
// depends on nothing but Windows.

#include "data_guard.h"

#include <parties/update/probation.h>

#include <functional>
#include <optional>
#include <variant>
#include <vector>

namespace parties::update::launcher {

struct StartAgain {};
struct StartBuild { BuildId build; };
struct RetryBuild { BuildId build; };
struct RestoreBackup { Snapshot snapshot; };
struct Quit {};
using RecoveryChoice = std::variant<StartAgain, StartBuild, RetryBuild, RestoreBackup, Quit>;

struct RecoveryRequest {
    std::optional<RecoveryReason> why;  // nullopt: opened with --recover
    const SupervisorState& state;
    std::vector<Backup> backups;
    fs::path data_dir;
    std::function<void()> on_shown;
};

RecoveryChoice show_recovery(const RecoveryRequest& request);

std::vector<Event> events_for(const RecoveryChoice& choice);

} // namespace parties::update::launcher
