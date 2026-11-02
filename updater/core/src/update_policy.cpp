#include <parties/update/update_policy.h>

#include <algorithm>

namespace parties::update {

namespace {

struct Facts {
    const SupervisorState& state;
    const VerifiedManifest& manifest;
    uint32_t launcher_protocol;
    const std::optional<DataInfo>& live;

    bool quarantined(const BuildEntry& e) const {
        const BuildRecord* r = state.find(e.id());
        return r && r->quarantined();
    }
    bool talks_to_launcher(const BuildEntry& e) const { return e.min_supported_launcher_link <= launcher_protocol; }
    bool reads_data(const BuildEntry& e) const {
        return !live || compatibility(e.data_schema, *live) != Compatibility::Incompatible;
    }
    // Could run here today, ignoring the data.
    bool offerable(const BuildEntry& e) const {
        return !manifest.is_revoked(e.version) && talks_to_launcher(e) && !quarantined(e);
    }
    bool runnable(const BuildEntry& e) const { return offerable(e) && reads_data(e); }

    template <class Pred>
    const BuildEntry* newest(Pred pred) const {
        auto builds = manifest.builds();
        auto it = std::ranges::find_if(builds, pred);
        return it == builds.end() ? nullptr : &*it;
    }

    DataEffect data_effect(const BuildEntry& e) const {
        if (!live) return DataEffect::ReadsAsIs;
        switch (compatibility(e.data_schema, *live)) {
        case Compatibility::MigratesForward: return DataEffect::MigratesForward;
        case Compatibility::ReadsAsIs: return DataEffect::ReadsAsIs;
        case Compatibility::Incompatible: return DataEffect::NeedsRestore;
        }
        return DataEffect::NeedsRestore;
    }

    bool already_staged(const BuildEntry& e) const {
        auto* staged = std::get_if<Staged>(&state.activation);
        return staged && staged->build == e.id();
    }

    UpdatePlan stage(const BuildEntry& e, StageReason reason) const {
        if (already_staged(e)) return NothingToDo{};
        return StageBuild{e, reason};
    }
};

} // namespace

UpdateDecision plan_update(const SupervisorState& state, const VerifiedManifest& manifest,
                           uint32_t launcher_protocol, const std::optional<DataInfo>& live_db) {
    Facts f{state, manifest, launcher_protocol, live_db};
    if (std::holds_alternative<Probation>(state.activation) || std::holds_alternative<Restoring>(state.operation))
        return {Blocked{BlockReason::Busy, std::nullopt}};

    const Version& installed = state.good.version;
    const BuildEntry* newest_runnable = f.newest([&](const BuildEntry& e) { return f.runnable(e); });

    // Moving off a revoked build overrides a pin: the pin only guards against
    // downgrading a working build. A move that would restore a backup is only
    // offered, because it sets aside the user's newest data.
    if (manifest.is_revoked(installed)) {
        auto other = [&](const BuildEntry& e) { return f.offerable(e) && e.id() != state.good; };
        if (const BuildEntry* target = f.newest([&](const BuildEntry& e) { return other(e) && f.reads_data(e); }))
            return {f.stage(*target, StageReason::LeaveRevoked)};
        if (const BuildEntry* restoring = f.newest(other); restoring && !f.already_staged(*restoring))
            return {OfferDowngrade{{*restoring, f.data_effect(*restoring)}}};
        return {Blocked{BlockReason::NoCompatibleBuild, std::nullopt}};
    }

    std::optional<ChannelIntent> new_intent;
    if (auto* pin = std::get_if<PinnedUntilAtLeast>(&state.intent)) {
        if (!newest_runnable || newest_runnable->version < pin->floor) {
            const BuildEntry* top = f.newest([&](const BuildEntry& e) { return !manifest.is_revoked(e.version); });
            ReportPinned report{pin->floor, top ? std::optional(top->version) : std::nullopt, std::nullopt};
            const BuildEntry* older = f.newest([&](const BuildEntry& e) { return f.offerable(e); });
            if (older && older->version < installed) report.downgrade = DowngradeOffer{*older, f.data_effect(*older)};
            return {report};
        }
        new_intent = Follow{};
    }

    if (newest_runnable && newest_runnable->version > installed)
        return {f.stage(*newest_runnable, StageReason::Newer), new_intent};

    const BuildEntry* waiting = f.newest([&](const BuildEntry& e) {
        return e.version > installed && !manifest.is_revoked(e.version) && !f.quarantined(e) && f.reads_data(e) &&
               !f.talks_to_launcher(e);
    });
    if (waiting) return {Blocked{BlockReason::LauncherTooOld, waiting->version}, new_intent};

    const BuildEntry* top = f.newest([&](const BuildEntry& e) { return f.offerable(e); });
    if (top && top->version < installed && !f.already_staged(*top))
        return {OfferDowngrade{{*top, f.data_effect(*top)}}, new_intent};
    return {NothingToDo{}, new_intent};
}

ChannelSwitchPlan plan_channel_switch(const SupervisorState& state, const VerifiedManifest& target,
                                      uint32_t launcher_protocol, const std::optional<DataInfo>& live_db) {
    Facts f{state, target, launcher_protocol, live_db};
    const Version& installed = state.good.version;
    const BuildEntry* top = f.newest([&](const BuildEntry& e) { return f.offerable(e); });
    if (top && top->version >= installed) return FollowNow{};
    StayPinned pinned{installed, std::nullopt};
    if (top) pinned.downgrade = DowngradeOffer{*top, f.data_effect(*top)};
    return pinned;
}

ChannelIntent intent_for(const ChannelSwitchPlan& plan) {
    if (auto* pinned = std::get_if<StayPinned>(&plan)) return PinnedUntilAtLeast{pinned->until};
    return Follow{};
}

ChannelIntent wait_intent(const SupervisorState& state) { return PinnedUntilAtLeast{state.good.version}; }

std::optional<Snapshot> restore_point(uint32_t data_schema, std::span<const Snapshot> newest_first) {
    auto it = std::ranges::find_if(newest_first, [&](const Snapshot& s) {
        return compatibility(data_schema, s.data) != Compatibility::Incompatible;
    });
    if (it == newest_first.end()) return std::nullopt;
    return *it;
}

} // namespace parties::update
