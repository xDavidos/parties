#include <parties/update/front_door_policy.h>

#include <algorithm>
#include <array>

namespace parties::update::front_door {

ExitKind classify_exit(const FrontOutcome& outcome) {
    if (outcome.reported_code) {
        switch (*outcome.reported_code) {
        case 0: return ExitKind::Clean;
        case 2: return ExitKind::InitFailed;
        case 3: return ExitKind::DataUnavailable;
        case 4: return ExitKind::DataTooNew;
        default: return ExitKind::Abnormal;
        }
    }
    if (outcome.killed_by_launcher) return ExitKind::KilledHung;
    return outcome.session_ending ? ExitKind::Clean : ExitKind::Abnormal;
}

Route route_front(const RouteInput& input) {
    constexpr std::array kOwners{FrontState::Assigned, FrontState::Recovering, FrontState::Waiting};
    for (FrontState owner : kOwners) {
        auto it = std::ranges::find(input.fronts, owner, &FrontView::state);
        if (it != input.fronts.end()) return SendActivateOther{it->pid};
    }
    if (input.pending_spawn) return DeliverPendingSpawn{};
    if (input.recovery_pending) return DeliverPendingRecovery{};
    if (input.newcomer_seed_pending) return SendQuitForInstall{};
    if (input.newcomer_recover) return OpenRequestedRecovery{};
    return StartForFront{};
}

} // namespace parties::update::front_door
