#pragma once

// Pure decisions of the macOS launcher's front door (process-contract.md):
// how a front process ended, judged without being its parent, and what a
// newly connected front is told.

#include <parties/update/probation.h>

#include <cstdint>
#include <optional>
#include <variant>
#include <vector>

namespace parties::update::front_door {

// What the launcher observed of a front before its NOTE_EXIT.
struct FrontOutcome {
    std::optional<int32_t> reported_code;  // the last exiting{code}
    bool killed_by_launcher = false;       // the launcher sent SIGKILL (TerminateApp or the post-exit grace)
    bool session_ending = false;           // NSWorkspaceWillPowerOffNotification seen before the death
    friend bool operator==(const FrontOutcome&, const FrontOutcome&) = default;
};

// The host, not the payload, writes exiting{code}, so a crash or a stray
// exit() can never read as Clean: every unknown is Abnormal, the blamed
// direction. A report stands even when the grace kill followed it.
// ExitKind::Terminated is never produced: Force Quit is a SIGKILL like a crash.
ExitKind classify_exit(const FrontOutcome& outcome);

// What a connected front is doing, as far as routing cares.
enum class FrontState : uint8_t {
    Waiting,     // told nothing yet; the next Spawn goes to it
    Assigned,    // received assign: it is the app session
    Recovering,  // received recovery
    Leaving,     // told to exit, or reported exiting: it owns nothing
};

struct FrontView {
    int32_t pid = 0;
    FrontState state = FrontState::Waiting;
};

struct RouteInput {
    std::vector<FrontView> fronts;  // connected before the newcomer
    bool pending_spawn = false;     // a Spawn waits for a front to deliver to
    bool recovery_pending = false;  // a ShowRecovery waits for a front
    bool newcomer_recover = false;  // front{recover:true}
    bool newcomer_seed_pending = false;
};

struct DeliverPendingSpawn {
    friend bool operator==(const DeliverPendingSpawn&, const DeliverPendingSpawn&) = default;
};
struct DeliverPendingRecovery {
    friend bool operator==(const DeliverPendingRecovery&, const DeliverPendingRecovery&) = default;
};
struct OpenRequestedRecovery {
    friend bool operator==(const OpenRequestedRecovery&, const OpenRequestedRecovery&) = default;
};
// Nothing is pending: the supervisor decides (StartRequested).
struct StartForFront {
    friend bool operator==(const StartForFront&, const StartForFront&) = default;
};
struct SendActivateOther {
    int32_t owner = 0;
    friend bool operator==(const SendActivateOther&, const SendActivateOther&) = default;
};
struct SendQuitForInstall {
    friend bool operator==(const SendQuitForInstall&, const SendQuitForInstall&) = default;
};
using Route = std::variant<DeliverPendingSpawn, DeliverPendingRecovery, OpenRequestedRecovery, StartForFront,
                           SendActivateOther, SendQuitForInstall>;

// A front that is assigned, recovering or waiting owns the session (in that
// order of precedence, then list order), and every newcomer is sent to it.
// Otherwise pending work goes first, so a Spawn whose health deadline is
// already running always gets its front. A newer seed waits until no session
// is in progress.
Route route_front(const RouteInput& input);

} // namespace parties::update::front_door
