#include "recovery_choice.h"

#include <parties/update/update_policy.h>

namespace parties::update::launcher {

std::optional<StartBuild> start_previous(const SupervisorState& s, const std::optional<DataInfo>& live_db,
                                         const std::vector<Backup>& backups) {
    if (auto* p = std::get_if<Probation>(&s.activation)) return StartBuild{p->fallback, std::nullopt};
    if (!s.previous_good) return std::nullopt;
    const BuildRecord* prev = s.find(*s.previous_good);
    if (!prev || !std::holds_alternative<Proven>(prev->verdict) || s.revoked.contains(prev->id.version)) return std::nullopt;
    if (!live_db || compatibility(prev->data_schema, *live_db) != Compatibility::Incompatible)
        return StartBuild{prev->id, std::nullopt};
    std::vector<Snapshot> newest_first;
    for (const Backup& b : backups) newest_first.push_back({b.id, b.data});
    auto point = restore_point(prev->data_schema, newest_first);
    if (!point) return std::nullopt;
    return StartBuild{prev->id, point};
}

std::vector<BuildId> retryable(const SupervisorState& s) {
    std::vector<BuildId> out;
    if (std::holds_alternative<Probation>(s.activation)) return out;
    for (const BuildRecord& r : s.builds)
        if (r.quarantined() && r.id != s.good && !s.revoked.contains(r.id.version)) out.push_back(r.id);
    return out;
}

bool restorable(const SupervisorState& s) { return !std::holds_alternative<Probation>(s.activation); }

RecoveryChoice scripted_choice(const SupervisorState& s, const std::optional<DataInfo>& live_db,
                               const std::vector<Backup>& backups, std::string_view answer) {
    if (answer == "quit") return Quit{};
    if (answer == "start") return StartAgain{};
    if (answer == "previous") {
        if (auto prev = start_previous(s, live_db, backups)) return *prev;
    }
    if (answer == "retry") {
        auto builds = retryable(s);
        if (!builds.empty()) return RetryBuild{builds.front()};
    }
    if (answer == "restore" && restorable(s) && !backups.empty())
        return RestoreBackup{Snapshot{backups.front().id, backups.front().data}};
    return Quit{};
}

const char* recovery_reason_name(const std::optional<RecoveryReason>& why) {
    if (!why) return "requested";
    switch (*why) {
    case RecoveryReason::GoodFailedToStart: return "good_failed_to_start";
    case RecoveryReason::DataUnavailable: return "data_unavailable";
    case RecoveryReason::DataIncompatible: return "data_incompatible";
    case RecoveryReason::Repairing: return "repairing";
    }
    return "unknown";
}

} // namespace parties::update::launcher
