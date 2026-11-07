#pragma once

// What a recovery can offer, decided the same on every platform. Each
// platform only draws the choices.

#include "supervisor_core.h"

#include <optional>
#include <string_view>
#include <vector>

namespace parties::update::launcher {

// What "Start the previous version" does, or nullopt when it cannot start it.
// A rollback under probation restores what the candidate changed by itself.
// Otherwise a previous build that cannot read the live data starts on the
// newest backup it can open.
std::optional<StartBuild> start_previous(const SupervisorState& state, const std::optional<DataInfo>& live_db,
                                         const std::vector<Backup>& backups);

// Quarantined builds the user may try again.
std::vector<BuildId> retryable(const SupervisorState& state);

bool restorable(const SupervisorState& state);

// The answer a harness scripted with PARTIES_TEST_RECOVERY: quit, start,
// previous, retry or restore. An answer that is not on offer quits.
RecoveryChoice scripted_choice(const SupervisorState& state, const std::optional<DataInfo>& live_db,
                               const std::vector<Backup>& backups, std::string_view answer);

const char* recovery_reason_name(const std::optional<RecoveryReason>& why);

} // namespace parties::update::launcher
