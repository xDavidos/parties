#include <parties/update/probation.h>

#include <algorithm>

namespace parties::update {

namespace {

// One transition in progress. Every state change reaches disk through a commit
// before any effect that depends on it.
class Tx {
public:
    Tx(const SupervisorState& state, const Context& context)
        : s(state), ctx(context), committed_(state) {}

    SupervisorState s;
    const Context& ctx;

    uint64_t next_generation() const { return s.generation + 1; }
    void commit() {
        ++s.generation;
        committed_ = s;
        effects_.push_back(Commit{s});
    }
    void settle() {
        if (s != committed_) commit();
    }
    void emit(Effect e) { effects_.push_back(std::move(e)); }
    Step done() && { return {std::move(s), std::move(effects_)}; }

private:
    SupervisorState committed_;
    std::vector<Effect> effects_;
};

BuildRecord* record(SupervisorState& s, const BuildId& id) {
    auto it = std::ranges::find(s.builds, id, &BuildRecord::id);
    return it == s.builds.end() ? nullptr : &*it;
}

Compatibility compat_of(const SupervisorState& s, const BuildId& id, const std::optional<DataInfo>& data) {
    const BuildRecord* r = s.find(id);
    if (!r) return Compatibility::Incompatible;
    return data ? compatibility(r->data_schema, *data) : Compatibility::ReadsAsIs;
}

// Names carry the generation of the commit that records them, so a replay
// after a crash reuses the name and a later rollback never reuses it.
SnapshotId snapshot_name(std::string_view kind, const BuildId& id, uint64_t generation) {
    return {std::string(kind) + "-" + id.dir_name() + "-g" + std::to_string(generation)};
}

bool revoked(const SupervisorState& s, const BuildId& id) { return s.revoked.contains(id.version); }

Probation* probation(SupervisorState& s) { return std::get_if<Probation>(&s.activation); }
Running* running(SupervisorState& s) { return std::get_if<Running>(&s.operation); }
bool idle(const SupervisorState& s) { return std::holds_alternative<Idle>(s.operation); }

void show(Tx& tx, RecoveryReason why) {
    tx.emit(ShowRecovery{why});
    tx.emit(SignalBootOk{});
}

void spawn(Tx& tx, const BuildId& build) {
    tx.s.operation = Running{build};
    tx.commit();
    tx.emit(Spawn{build});
}

// Emits the restore of a committed Restoring; the caller commits after it.
DataInfo finish_restore(Tx& tx) {
    Restoring r = std::get<Restoring>(tx.s.operation);
    tx.emit(RestoreSnapshot{r.from.id, r.preserve_live_as});
    tx.s.operation = Idle{};
    return r.from.data;
}

void begin_restore(Tx& tx, const Snapshot& from, SnapshotId preserve_live_as) {
    tx.s.operation = Restoring{from, std::move(preserve_live_as)};
    tx.commit();
}

// Returns the build to delete. A retried previous_good keeps its record.
std::optional<BuildId> unstage(Tx& tx) {
    auto* staged = std::get_if<Staged>(&tx.s.activation);
    if (!staged) return std::nullopt;
    BuildId id = staged->build;
    tx.s.activation = Steady{};
    if (tx.s.previous_good == id) return std::nullopt;
    std::erase_if(tx.s.builds, [&](const BuildRecord& r) { return r.id == id; });
    return id;
}

void start_good(Tx& tx, const std::optional<DataInfo>& data) {
    Compatibility c = compat_of(tx.s, tx.s.good, data);
    if (c == Compatibility::Incompatible) {
        tx.settle();
        return show(tx, RecoveryReason::DataIncompatible);
    }
    if (c == Compatibility::MigratesForward)
        tx.emit(TakeSnapshot{snapshot_name("pre", tx.s.good, tx.next_generation())});
    spawn(tx, tx.s.good);
}

// Leaves Probation for good. Restores the pre-candidate data when good cannot
// read what the candidate left, or when the candidate ran on restored data.
void rollback(Tx& tx, FailureReason reason, bool start_fallback, std::optional<DataInfo> data) {
    Probation p = std::get<Probation>(tx.s.activation);
    if (BuildRecord* c = record(tx.s, p.candidate)) c->verdict = Quarantined{reason};
    tx.s.activation = Steady{};
    tx.s.operation = Idle{};
    Notice notice{Notice::Kind::RolledBack, p.candidate.version, p.fallback.version, std::nullopt};
    bool restore = p.restore_on_rollback ||
                   compat_of(tx.s, p.fallback, data) == Compatibility::Incompatible;
    if (restore && p.pre_snapshot) {
        notice.preserved = snapshot_name("rolled-back", p.candidate, tx.next_generation());
        tx.s.notice = notice;
        begin_restore(tx, *p.pre_snapshot, *notice.preserved);
        data = finish_restore(tx);
    } else {
        tx.s.notice = notice;
    }
    if (start_fallback) start_good(tx, data);
    else tx.settle();
}

void rollback(Tx& tx, FailureReason reason, bool start_fallback) {
    rollback(tx, reason, start_fallback, tx.ctx.live_db);
}

void confirm(Tx& tx) {
    Probation p = std::get<Probation>(tx.s.activation);
    if (BuildRecord* c = record(tx.s, p.candidate)) c->verdict = Proven{};
    BuildId old_good = tx.s.good;
    // Kept on disk for the boot stub's launcher fallback, but never run again.
    if (revoked(tx.s, old_good)) record(tx.s, old_good)->verdict = Quarantined{FailureReason::Revoked};
    tx.s.previous_good = old_good;
    tx.s.good = p.candidate;
    tx.s.activation = Steady{};
    std::vector<BuildId> dropped;
    std::erase_if(tx.s.builds, [&](const BuildRecord& r) {
        bool keep = r.id == tx.s.good || r.id == old_good ||
                    (r.quarantined() && r.id.version > tx.s.good.version);
        if (!keep) dropped.push_back(r.id);
        return !keep;
    });
    tx.commit();
    for (const BuildId& id : dropped) tx.emit(DeleteBuild{id});
}

// A candidate run after health ended badly, or a run's outcome was never seen.
void strike(Tx& tx, FailureReason reason) {
    Probation& p = std::get<Probation>(tx.s.activation);
    if (p.failed_runs < 255) ++p.failed_runs;
    tx.s.operation = Idle{};
    if (p.failed_runs >= tx.ctx.policy.max_failed_runs_after_healthy) return rollback(tx, reason, false);
    tx.commit();
}

void start_staged(Tx& tx, Staged staged, const std::optional<DataInfo>& data) {
    const BuildId c = staged.build;
    std::optional<DataInfo> candidate_data = staged.restore_first ? std::optional(staged.restore_first->data) : data;
    if (revoked(tx.s, c) || compat_of(tx.s, c, candidate_data) == Compatibility::Incompatible) {
        std::optional<BuildId> dropped = unstage(tx);
        tx.commit();
        if (dropped) tx.emit(DeleteBuild{*dropped});
        return start_good(tx, data);
    }
    // Over a restore, the live copy may be the only one of data newer than the
    // backup, so it must not carry the automatic "pre" name retention prunes.
    SnapshotId pre_id = snapshot_name(staged.restore_first ? "pre-downgrade" : "pre", c, tx.next_generation());
    std::optional<Snapshot> pre;
    if (data) pre = Snapshot{pre_id, *data};
    Probation p{c, tx.s.good, pre, 0, false};
    if (staged.restore_first) {
        // The preserved live copy is the pre-candidate snapshot good returns to.
        p.restore_on_rollback = pre.has_value();
        tx.s.activation = p;
        begin_restore(tx, *staged.restore_first, pre_id);
        finish_restore(tx);
    } else {
        if (pre) tx.emit(TakeSnapshot{pre->id});
        tx.s.activation = p;
    }
    spawn(tx, c);
}

void restart_candidate(Tx& tx, const std::optional<DataInfo>& data) {
    Probation& p = std::get<Probation>(tx.s.activation);
    if (revoked(tx.s, p.candidate)) return rollback(tx, FailureReason::Revoked, true, data);
    bool migrates = data && compat_of(tx.s, p.candidate, data) == Compatibility::MigratesForward;
    if (migrates && !(p.pre_snapshot && p.pre_snapshot->data == *data)) {
        Snapshot snap{snapshot_name("pre", p.candidate, tx.next_generation()), *data};
        tx.emit(TakeSnapshot{snap.id});
        if (!p.pre_snapshot) p.pre_snapshot = snap;
    }
    spawn(tx, p.candidate);
}

FailureReason before_health_reason(ExitKind kind) {
    switch (kind) {
    case ExitKind::InitFailed: return FailureReason::InitFailed;
    case ExitKind::DataTooNew: return FailureReason::DataTooNew;
    case ExitKind::KilledHung: return FailureReason::HungBeforeHealthy;
    case ExitKind::Terminated: return FailureReason::TerminatedBeforeHealthy;
    default: return FailureReason::CrashedBeforeHealthy;
    }
}

void candidate_exited(Tx& tx, const Running& run, ExitKind kind) {
    tx.s.operation = Idle{};
    if (revoked(tx.s, run.build)) {
        bool startup_failure = !run.healthy && kind != ExitKind::Clean && kind != ExitKind::DataUnavailable;
        rollback(tx, FailureReason::Revoked, startup_failure);
        if (kind == ExitKind::DataUnavailable) show(tx, RecoveryReason::DataUnavailable);
        return;
    }
    switch (kind) {
    case ExitKind::Clean:
        if (run.healthy && run.healthy_uptime_s >= tx.ctx.policy.confirm_on_clean_exit_after_s)
            return confirm(tx);
        return tx.commit();
    case ExitKind::DataUnavailable:
        tx.commit();
        return show(tx, RecoveryReason::DataUnavailable);
    case ExitKind::Terminated:
        if (run.healthy) return tx.commit();
        return rollback(tx, FailureReason::TerminatedBeforeHealthy, true);
    case ExitKind::InitFailed:
    case ExitKind::DataTooNew:
    case ExitKind::Abnormal:
    case ExitKind::KilledHung:
        if (run.healthy) return strike(tx, FailureReason::CrashedRepeatedlyAfterHealthy);
        return rollback(tx, before_health_reason(kind), true);
    }
}

void good_exited(Tx& tx, const Running& run, ExitKind kind) {
    tx.s.operation = Idle{};
    tx.commit();
    switch (kind) {
    case ExitKind::DataUnavailable: return show(tx, RecoveryReason::DataUnavailable);
    case ExitKind::DataTooNew: return show(tx, RecoveryReason::DataIncompatible);
    case ExitKind::InitFailed:
    case ExitKind::Abnormal:
    case ExitKind::KilledHung:
        if (!run.healthy) show(tx, RecoveryReason::GoodFailedToStart);
        return;
    case ExitKind::Clean:
    case ExitKind::Terminated: return;
    }
}

bool running_candidate(SupervisorState& s) {
    Probation* p = probation(s);
    Running* r = running(s);
    return p && r && r->build == p->candidate;
}

// --- Event handlers ---

void handle(Tx& tx, const StartRequested&) {
    if (running(tx.s)) return;
    std::optional<DataInfo> data = tx.ctx.live_db;
    if (std::holds_alternative<Restoring>(tx.s.operation)) {
        data = finish_restore(tx);
        tx.commit();
    }
    if (std::holds_alternative<Steady>(tx.s.activation)) return start_good(tx, data);
    if (auto* staged = std::get_if<Staged>(&tx.s.activation)) return start_staged(tx, *staged, data);
    restart_candidate(tx, data);
}

void handle(Tx& tx, const Resumed&) {
    if (std::holds_alternative<Restoring>(tx.s.operation)) {
        finish_restore(tx);
        return tx.commit();
    }
    if (!running(tx.s)) return;
    if (running_candidate(tx.s)) {
        if (revoked(tx.s, running(tx.s)->build)) {
            tx.s.operation = Idle{};
            return rollback(tx, FailureReason::Revoked, false);
        }
        return strike(tx, FailureReason::UnknownOutcomeRepeated);
    }
    tx.s.operation = Idle{};
    tx.commit();
}

void handle(Tx& tx, const AppHealthy&) {
    Running* r = running(tx.s);
    if (!r) return;
    if (!r->healthy) {
        r->healthy = true;
        tx.commit();
    }
    tx.emit(SignalBootOk{});
}

void handle(Tx& tx, const AppExited& e) {
    Running* r = running(tx.s);
    if (!r) return;
    Running run = *r;
    if (running_candidate(tx.s)) return candidate_exited(tx, run, e.kind);
    good_exited(tx, run, e.kind);
}

void handle(Tx& tx, const HealthDeadlinePassed&) {
    if (running_candidate(tx.s) && !running(tx.s)->healthy) tx.emit(TerminateApp{});
}

void handle(Tx& tx, const ConfirmTimerElapsed& e) {
    if (!running_candidate(tx.s)) return;
    Running* r = running(tx.s);
    if (!r->healthy || e.healthy_uptime_s <= r->healthy_uptime_s || revoked(tx.s, r->build)) return;
    r->healthy_uptime_s = e.healthy_uptime_s;
    if (r->healthy_uptime_s >= tx.ctx.policy.confirm_uptime_s) return confirm(tx);
    tx.commit();
}

// Stages `rec` in place of whatever was staged, adding its record if it is new.
void stage(Tx& tx, const BuildRecord& rec, const std::optional<Snapshot>& restore_first) {
    std::optional<BuildId> replaced = unstage(tx);
    if (!tx.s.find(rec.id)) {
        BuildRecord fresh = rec;
        fresh.verdict = Unproven{};
        tx.s.builds.push_back(fresh);
    }
    tx.s.activation = Staged{rec.id, restore_first};
    tx.commit();
    if (replaced) tx.emit(DeleteBuild{*replaced});
}

void handle(Tx& tx, const BuildSealed& e) {
    if (tx.s.find(e.record.id) || probation(tx.s) || revoked(tx.s, e.record.id)) return;
    stage(tx, e.record, e.restore_first);
}

// Unlike BuildSealed, a known previous_good is staged again: it is the build a
// user most often switches back to, and its directory is still intact.
void handle(Tx& tx, const UserDowngrade& e) {
    if (probation(tx.s) || e.record.id == tx.s.good || revoked(tx.s, e.record.id)) return;
    const BuildRecord* known = tx.s.find(e.record.id);
    if (known && (tx.s.previous_good != e.record.id || known->quarantined())) return;
    stage(tx, e.record, e.restore_first);
}

void handle(Tx& tx, const UserRollback& e) {
    if (!idle(tx.s)) return;
    if (Probation* p = probation(tx.s)) {
        if (e.target == p->fallback) rollback(tx, FailureReason::UserRolledBack, true);
        return;
    }
    if (!tx.s.previous_good || e.target != *tx.s.previous_good || revoked(tx.s, e.target)) return;
    const BuildRecord* target = tx.s.find(e.target);
    if (!target || !std::holds_alternative<Proven>(target->verdict)) return;
    std::optional<DataInfo> data = e.restore ? std::optional(e.restore->data) : tx.ctx.live_db;
    if (compat_of(tx.s, e.target, data) == Compatibility::Incompatible)
        return show(tx, RecoveryReason::DataIncompatible);

    std::optional<BuildId> unstaged = unstage(tx);
    BuildId old_good = tx.s.good;
    record(tx.s, old_good)->verdict = Quarantined{FailureReason::UserRolledBack};
    tx.s.good = e.target;
    tx.s.previous_good = old_good;
    Notice notice{Notice::Kind::RolledBack, old_good.version, e.target.version, std::nullopt};
    if (e.restore) {
        notice.preserved = snapshot_name("pre-rollback", old_good, tx.next_generation());
        tx.s.notice = notice;
        begin_restore(tx, *e.restore, *notice.preserved);
        data = finish_restore(tx);
    } else {
        tx.s.notice = notice;
    }
    start_good(tx, data);
    if (unstaged) tx.emit(DeleteBuild{*unstaged});
}

void handle(Tx& tx, const UserRetry& e) {
    if (!idle(tx.s) || probation(tx.s)) return;
    const BuildRecord* target = tx.s.find(e.target);
    if (!target || !target->quarantined() || revoked(tx.s, e.target)) return;
    std::optional<BuildId> replaced = unstage(tx);
    record(tx.s, e.target)->verdict = Unproven{};
    tx.s.activation = Staged{e.target, std::nullopt};
    tx.commit();
    if (replaced) tx.emit(DeleteBuild{*replaced});
}

void handle(Tx& tx, const UserRestore& e) {
    if (!idle(tx.s) || probation(tx.s)) return;
    if (compat_of(tx.s, tx.s.good, e.from.data) == Compatibility::Incompatible)
        return show(tx, RecoveryReason::DataIncompatible);
    SnapshotId keep = snapshot_name("pre-restore", tx.s.good, tx.next_generation());
    tx.s.notice = Notice{Notice::Kind::DataRestored, tx.s.good.version, tx.s.good.version, keep};
    begin_restore(tx, e.from, keep);
    finish_restore(tx);
    tx.commit();
}

void handle(Tx& tx, const BuildCorrupt& e) {
    if (std::holds_alternative<Restoring>(tx.s.operation)) return;
    Running* r = running(tx.s);
    bool spawn_failed = r && r->build == e.build;
    bool other_running = r && !spawn_failed;
    if (Probation* p = probation(tx.s); p && e.build == p->candidate) {
        if (other_running) return;
        tx.s.operation = Idle{};
        return rollback(tx, FailureReason::Corrupt, spawn_failed);
    }
    if (auto* staged = std::get_if<Staged>(&tx.s.activation); staged && staged->build == e.build) {
        std::optional<BuildId> dropped = unstage(tx);
        tx.commit();
        if (dropped) tx.emit(DeleteBuild{*dropped});
        if (tx.s.previous_good == e.build) tx.emit(Redownload{e.build});
        return;
    }
    if (e.build == tx.s.good) {
        tx.emit(Redownload{e.build});
        if (other_running) return;
        if (spawn_failed) {
            tx.s.operation = Idle{};
            tx.commit();
        }
        return show(tx, RecoveryReason::Repairing);
    }
    if (tx.s.previous_good && e.build == *tx.s.previous_good) tx.emit(Redownload{e.build});
}

// A candidate revoked while it runs is never confirmed; its exit rolls back.
void handle(Tx& tx, const ManifestSeen& e) {
    const bool good_was_revoked = revoked(tx.s, tx.s.good);
    std::vector<Version> list = e.revoked;
    std::ranges::sort(list);
    tx.s.revoked.of(e.channel) = std::move(list);
    if (!good_was_revoked && revoked(tx.s, tx.s.good) && !tx.s.notice)
        tx.s.notice = Notice{Notice::Kind::Withdrawn, tx.s.good.version, tx.s.good.version, std::nullopt};
    if (auto* staged = std::get_if<Staged>(&tx.s.activation); staged && revoked(tx.s, staged->build)) {
        std::optional<BuildId> dropped = unstage(tx);
        tx.commit();
        if (dropped) tx.emit(DeleteBuild{*dropped});
        return;
    }
    if (Probation* p = probation(tx.s); p && idle(tx.s) && revoked(tx.s, p->candidate))
        rollback(tx, FailureReason::Revoked, false);
}

void handle(Tx& tx, const LeaveRevoked& e) {
    if (probation(tx.s) || !revoked(tx.s, tx.s.good) || e.record.id == tx.s.good || revoked(tx.s, e.record.id)) return;
    if (const BuildRecord* known = tx.s.find(e.record.id); known && known->quarantined()) return;
    if (auto* staged = std::get_if<Staged>(&tx.s.activation); staged && staged->build == e.record.id) return;
    stage(tx, e.record, std::nullopt);
}

void handle(Tx& tx, const ChannelIntentSet& e) {
    if (e.channel == tx.s.channel && e.intent == tx.s.intent) return;
    bool switched = e.channel != tx.s.channel;
    tx.s.channel = e.channel;
    tx.s.intent = e.intent;
    std::optional<BuildId> unstaged = switched ? unstage(tx) : std::nullopt;
    tx.commit();
    if (unstaged) tx.emit(DeleteBuild{*unstaged});
}

// A Restoring under Probation is the candidate's restore_first: without it
// the candidate cannot run, so it goes back to being only a record.
void handle(Tx& tx, const RestoreFailed&) {
    if (!std::holds_alternative<Restoring>(tx.s.operation)) return;
    tx.s.operation = Idle{};
    std::optional<BuildId> dropped;
    if (Probation* p = probation(tx.s)) {
        BuildId candidate = p->candidate;
        tx.s.activation = Steady{};
        if (tx.s.previous_good != candidate) {
            std::erase_if(tx.s.builds, [&](const BuildRecord& r) { return r.id == candidate; });
            dropped = candidate;
        }
    }
    tx.s.notice = Notice{Notice::Kind::RestoreFailed, tx.s.good.version, tx.s.good.version, std::nullopt};
    tx.commit();
    if (dropped) tx.emit(DeleteBuild{*dropped});
}

void handle(Tx& tx, const NoticeAcknowledged&) {
    if (!tx.s.notice) return;
    tx.s.notice.reset();
    tx.commit();
}

} // namespace

Step step(const SupervisorState& state, const Event& event, const Context& ctx) {
    Tx tx(state, ctx);
    std::visit([&](const auto& e) { handle(tx, e); }, event);
    tx.settle();
    return std::move(tx).done();
}

} // namespace parties::update
