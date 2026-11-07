#pragma once

// What a recovery can offer, decided the same on every platform. Each
// platform only draws the choices.

#include "supervisor_core.h"

#include <parties/update/front_door.h>

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

// What the macOS host's alert offers: start, previous, retry:<build dir> and
// restore:<backup stem>, each only when it can be taken.
std::vector<front_door::RecoveryOption> recovery_options(const SupervisorState& state,
                                                         const std::optional<DataInfo>& live_db,
                                                         const std::vector<Backup>& backups);

// The choice an answer to that alert stands for. A bare answer a harness
// scripted (retry, restore) reads as scripted_choice does, and anything not
// on offer quits.
RecoveryChoice choice_for(std::string_view id, const SupervisorState& state, const std::optional<DataInfo>& live_db,
                          const std::vector<Backup>& backups);

} // namespace parties::update::launcher
