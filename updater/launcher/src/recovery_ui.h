#pragma once

// Native TaskDialog recovery. It runs exactly when the app cannot, so it
// depends on nothing but Windows.

#include "data_guard.h"
#include "supervisor_core.h"

#include <parties/update/probation.h>

#include <functional>
#include <optional>
#include <variant>
#include <vector>

namespace parties::update::launcher {

struct RecoveryRequest {
    std::optional<RecoveryReason> why;  // nullopt: opened with --recover
    const SupervisorState& state;
    std::optional<DataInfo> live_db;  // nullopt: no database
    std::vector<Backup> backups;      // newest first
    fs::path data_dir;
    std::function<void()> on_shown;
};

RecoveryChoice show_recovery(const RecoveryRequest& request);

} // namespace parties::update::launcher
