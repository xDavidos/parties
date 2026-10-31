// Offline tests for the supervisor state machine: an explicit transition table,
// then a seeded random walk that checks invariants over every reachable state.

#include <parties/update/probation.h>

#include <algorithm>
#include <functional>
#include <iostream>
#include <map>
#include <random>
#include <set>
#include <string>
#include <unordered_set>

using namespace parties::update;

namespace {

bool fail(const std::string& msg) {
    std::cerr << "update_probation_test: " << msg << "\n";
    return false;
}

Sha256 sha(char fill) { return *Sha256::from_hex(std::string(64, fill)); }

BuildRecord rec(const char* version, uint32_t schema, uint32_t floor, char fill) {
    BuildRecord r;
    r.id = {*Version::parse(version), sha(fill)};
    r.data_schema = schema;
    r.data_compat_floor = floor;
    r.launcher_sha256 = sha(fill);
    r.app_sha256 = sha(char(fill + 1));
    return r;
}

// A: installed. B: additive migration (A still reads B's data). C: breaking
// change (raises the floor, A and B can no longer read it). D: older than A.
// E: newer than C, same schema.
const BuildRecord A = rec("1.0.0.0", 1, 1, 'a');
const BuildRecord B = rec("1.1.0.0", 2, 1, 'b');
const BuildRecord C = rec("1.2.0.0", 3, 3, 'c');
const BuildRecord D = rec("0.9.0.0", 1, 1, 'd');
const BuildRecord E = rec("1.3.0.0", 3, 3, 'e');

constexpr DataInfo kV1{1, 1};
constexpr DataInfo kV2{2, 1};
constexpr DataInfo kV3{3, 3};

Context ctx(std::optional<DataInfo> live) { return {live, Policy{}}; }

BuildRecord with_verdict(BuildRecord r, Verdict v) {
    r.verdict = v;
    return r;
}

// --- State builders ---

SupervisorState steady(const BuildRecord& good = A) {
    SupervisorState s = initial_state(good, Channel::Stable);
    s.generation = 10;
    return s;
}

SupervisorState add(SupervisorState s, const BuildRecord& r, Verdict v = Unproven{}) {
    s.builds.push_back(with_verdict(r, v));
    return s;
}

SupervisorState with_previous(SupervisorState s, const BuildRecord& prev) {
    s = add(std::move(s), prev, Proven{});
    s.previous_good = prev.id;
    return s;
}

SupervisorState staged(const BuildRecord& c, std::optional<Snapshot> restore = std::nullopt,
                       const BuildRecord& good = A) {
    SupervisorState s = add(steady(good), c);
    s.activation = Staged{c.id, restore};
    return s;
}

SupervisorState probation(const BuildRecord& c, std::optional<Snapshot> pre = std::nullopt, uint8_t failed = 0,
                          const BuildRecord& good = A) {
    SupervisorState s = add(steady(good), c);
    s.activation = Probation{c.id, good.id, pre, failed, false};
    return s;
}

SupervisorState with_op(SupervisorState s, Operation op) {
    s.operation = op;
    return s;
}

Running run(const BuildRecord& b, bool healthy = false, uint32_t uptime = 0) { return {b.id, healthy, uptime}; }

// --- Effects as readable strings ---

std::string describe(const Effect& e) {
    return std::visit(
        [](const auto& x) -> std::string {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, TakeSnapshot>) return "TakeSnapshot " + x.id.stem;
            else if constexpr (std::is_same_v<T, Commit>) return "Commit";
            else if constexpr (std::is_same_v<T, Spawn>) return "Spawn " + x.build.version.to_string();
            else if constexpr (std::is_same_v<T, TerminateApp>) return "TerminateApp";
            else if constexpr (std::is_same_v<T, RestoreSnapshot>)
                return "RestoreSnapshot " + x.from.stem + " keep " + x.preserve_live_as.stem;
            else if constexpr (std::is_same_v<T, DeleteBuild>) return "DeleteBuild " + x.build.version.to_string();
            else if constexpr (std::is_same_v<T, Redownload>) return "Redownload " + x.build.version.to_string();
            else if constexpr (std::is_same_v<T, ShowRecovery>) {
                const char* names[] = {"GoodFailedToStart", "DataUnavailable", "DataIncompatible", "Repairing"};
                return std::string("ShowRecovery ") + names[int(x.why)];
            } else return "SignalBootOk";
        },
        e);
}

std::string describe(const std::vector<Effect>& effects) {
    std::string out = "[";
    for (size_t i = 0; i < effects.size(); ++i) out += (i ? ", " : "") + describe(effects[i]);
    return out + "]";
}

std::string snap(const char* kind, const BuildRecord& b, uint64_t gen) {
    return std::string(kind) + "-" + b.id.dir_name() + "-g" + std::to_string(gen);
}

const Probation* prob(const SupervisorState& s) { return std::get_if<Probation>(&s.activation); }
const Running* running(const SupervisorState& s) { return std::get_if<Running>(&s.operation); }
bool quarantined_for(const SupervisorState& s, const BuildRecord& b, FailureReason why) {
    const BuildRecord* r = s.find(b.id);
    auto* q = r ? std::get_if<Quarantined>(&r->verdict) : nullptr;
    return q && q->reason == why;
}
bool steady_on(const SupervisorState& s, const BuildRecord& good) {
    return std::holds_alternative<Steady>(s.activation) && s.good == good.id;
}

struct Row {
    std::string name;
    SupervisorState state;
    Event event;
    std::optional<DataInfo> live;
    std::vector<std::string> effects;
    std::function<bool(const SupervisorState&)> next_ok = [](const SupervisorState&) { return true; };
};

std::vector<Row> table() {
    const Snapshot pre_b{{"pre-b"}, kV1};
    const Snapshot pre_c{{"pre-c"}, kV1};
    const Snapshot backup_v1{{"backup-v1"}, kV1};
    const Snapshot backup_v3{{"backup-v3"}, kV3};
    std::vector<Row> rows;
    auto row = [&](Row r) { rows.push_back(std::move(r)); };

    // Steady: starting good.
    row({"steady start", steady(), StartRequested{}, kV1, {"Commit", "Spawn 1.0.0.0"},
         [](auto& s) { return running(s) && running(s)->build == A.id && !running(s)->healthy; }});
    row({"steady start on data good cannot open", steady(), StartRequested{}, kV3,
         {"ShowRecovery DataIncompatible", "SignalBootOk"}});
    row({"steady start that migrates snapshots first", steady(B), StartRequested{}, kV1,
         {"TakeSnapshot " + snap("pre", B, 11), "Commit", "Spawn 1.1.0.0"}});
    row({"steady start while running", with_op(steady(), run(A)), StartRequested{}, kV1, {}});
    row({"steady start with no database", steady(), StartRequested{}, std::nullopt, {"Commit", "Spawn 1.0.0.0"}});

    // Staged: the first candidate launch.
    row({"staged start", staged(B), StartRequested{}, kV1,
         {"TakeSnapshot " + snap("pre", B, 11), "Commit", "Spawn 1.1.0.0"}, [](auto& s) {
             auto* p = prob(s);
             return p && p->candidate == B.id && p->fallback == A.id && p->pre_snapshot &&
                    p->pre_snapshot->id.stem == snap("pre", B, 11) && p->pre_snapshot->data == kV1 &&
                    p->failed_runs == 0 && running(s) && running(s)->build == B.id;
         }});
    row({"staged start with no database", staged(B), StartRequested{}, std::nullopt, {"Commit", "Spawn 1.1.0.0"},
         [](auto& s) { return prob(s) && !prob(s)->pre_snapshot; }});
    row({"staged build that cannot open the data is dropped", staged(D, std::nullopt, C), StartRequested{}, kV3,
         {"Commit", "DeleteBuild 0.9.0.0", "Commit", "Spawn 1.2.0.0"},
         [](auto& s) { return steady_on(s, C) && !s.find(D.id); }});
    row({"staged downgrade on a chosen backup", staged(D, backup_v1, C), StartRequested{}, kV3,
         {"Commit", "RestoreSnapshot backup-v1 keep " + snap("pre", D, 11), "Commit", "Spawn 0.9.0.0"},
         [](auto& s) {
             auto* p = prob(s);
             return p && p->restore_on_rollback && p->pre_snapshot && p->pre_snapshot->data == kV3 &&
                    running(s) && running(s)->build == D.id;
         }});

    // Probation: health and confirmation.
    row({"first health", with_op(probation(B), run(B)), AppHealthy{}, kV2, {"Commit", "SignalBootOk"},
         [](auto& s) { return running(s)->healthy; }});
    row({"repeated health", with_op(probation(B), run(B, true)), AppHealthy{}, kV2, {"SignalBootOk"}});
    row({"health of good", with_op(steady(), run(A)), AppHealthy{}, kV1, {"Commit", "SignalBootOk"}});
    row({"confirm after 10 minutes", with_op(with_previous(probation(B, pre_b), D), run(B, true, 60)),
         ConfirmTimerElapsed{600}, kV2, {"Commit", "DeleteBuild 0.9.0.0"}, [](auto& s) {
             return steady_on(s, B) && s.previous_good == A.id && std::holds_alternative<Proven>(s.find(B.id)->verdict) &&
                    running(s) && running(s)->build == B.id && !s.find(D.id);
         }});
    row({"uptime tick below the confirm bar", with_op(probation(B), run(B, true)), ConfirmTimerElapsed{60}, kV2,
         {"Commit"}, [](auto& s) { return prob(s) && running(s)->healthy_uptime_s == 60; }});
    row({"stale uptime tick", with_op(probation(B), run(B, true, 60)), ConfirmTimerElapsed{30}, kV2, {}});
    row({"uptime tick before health", with_op(probation(B), run(B)), ConfirmTimerElapsed{600}, kV2, {}});
    row({"clean exit after 60 s healthy confirms", with_op(probation(B), run(B, true, 60)), AppExited{ExitKind::Clean},
         kV2, {"Commit"}, [](auto& s) { return steady_on(s, B) && std::holds_alternative<Idle>(s.operation); }});
    row({"clean exit after 30 s healthy is neutral", with_op(probation(B), run(B, true, 30)),
         AppExited{ExitKind::Clean}, kV2, {"Commit"},
         [](auto& s) { return prob(s) && prob(s)->failed_runs == 0 && std::holds_alternative<Idle>(s.operation); }});
    row({"clean exit before health is neutral", with_op(probation(B), run(B)), AppExited{ExitKind::Clean}, kV2,
         {"Commit"}, [](auto& s) { return prob(s) && prob(s)->failed_runs == 0; }});

    // Probation: failures before health roll back at once and start good.
    row({"crash before health, data still readable", with_op(probation(B, pre_b), run(B)),
         AppExited{ExitKind::Abnormal}, kV2, {"Commit", "Spawn 1.0.0.0"}, [](auto& s) {
             return steady_on(s, A) && quarantined_for(s, B, FailureReason::CrashedBeforeHealthy) && s.notice &&
                    !s.notice->preserved && running(s) && running(s)->build == A.id;
         }});
    row({"crash before health after a breaking migration restores",
         with_op(probation(C, pre_c), run(C)), AppExited{ExitKind::Abnormal}, kV3,
         {"Commit", "RestoreSnapshot pre-c keep " + snap("rolled-back", C, 11), "Commit", "Spawn 1.0.0.0"},
         [](auto& s) {
             return steady_on(s, A) && s.notice && s.notice->preserved &&
                    s.notice->preserved->stem == snap("rolled-back", C, 11);
         }});
    row({"crash before health, breaking migration, no snapshot", with_op(probation(C), run(C)),
         AppExited{ExitKind::Abnormal}, kV3, {"Commit", "ShowRecovery DataIncompatible", "SignalBootOk"},
         [](auto& s) { return steady_on(s, A) && quarantined_for(s, C, FailureReason::CrashedBeforeHealthy); }});
    row({"init failure", with_op(probation(B, pre_b), run(B)), AppExited{ExitKind::InitFailed}, kV1,
         {"Commit", "Spawn 1.0.0.0"}, [](auto& s) { return quarantined_for(s, B, FailureReason::InitFailed); }});
    row({"hang killed by the deadline", with_op(probation(B, pre_b), run(B)), AppExited{ExitKind::KilledHung}, kV1,
         {"Commit", "Spawn 1.0.0.0"}, [](auto& s) { return quarantined_for(s, B, FailureReason::HungBeforeHealthy); }});
    row({"killed from outside before health", with_op(probation(B, pre_b), run(B)), AppExited{ExitKind::Terminated},
         kV1, {"Commit", "Spawn 1.0.0.0"},
         [](auto& s) { return quarantined_for(s, B, FailureReason::TerminatedBeforeHealthy); }});
    row({"candidate refuses too-new data", with_op(probation(B, pre_b), run(B)), AppExited{ExitKind::DataTooNew}, kV1,
         {"Commit", "Spawn 1.0.0.0"}, [](auto& s) { return quarantined_for(s, B, FailureReason::DataTooNew); }});
    row({"data unavailable never blames the build", with_op(probation(B, pre_b), run(B)),
         AppExited{ExitKind::DataUnavailable}, kV1, {"Commit", "ShowRecovery DataUnavailable", "SignalBootOk"},
         [](auto& s) { return prob(s) && prob(s)->failed_runs == 0 && !s.find(B.id)->quarantined(); }});

    // Probation: failures after health count; the second rolls back without relaunching.
    row({"killed from outside after health is neutral", with_op(probation(B), run(B, true)),
         AppExited{ExitKind::Terminated}, kV2, {"Commit"}, [](auto& s) { return prob(s) && prob(s)->failed_runs == 0; }});
    row({"first crash after health", with_op(probation(B), run(B, true)), AppExited{ExitKind::Abnormal}, kV2,
         {"Commit"}, [](auto& s) { return prob(s) && prob(s)->failed_runs == 1 && std::holds_alternative<Idle>(s.operation); }});
    row({"second crash after health rolls back", with_op(probation(B, pre_b, 1), run(B, true)),
         AppExited{ExitKind::Abnormal}, kV2, {"Commit"}, [](auto& s) {
             return steady_on(s, A) && quarantined_for(s, B, FailureReason::CrashedRepeatedlyAfterHealthy) &&
                    std::holds_alternative<Idle>(s.operation);
         }});
    row({"hang deadline before health", with_op(probation(B), run(B)), HealthDeadlinePassed{}, kV1, {"TerminateApp"}});
    row({"hang deadline after health", with_op(probation(B), run(B, true)), HealthDeadlinePassed{}, kV1, {}});
    row({"hang deadline on good", with_op(steady(), run(A)), HealthDeadlinePassed{}, kV1, {}});
    row({"next start of the candidate", probation(B, pre_b, 1), StartRequested{}, kV2, {"Commit", "Spawn 1.1.0.0"}});
    row({"next start of the candidate before it migrated", probation(B, pre_b), StartRequested{}, kV1,
         {"Commit", "Spawn 1.1.0.0"}});

    // Resumed: reconcile after the launcher died.
    row({"resumed with an unobserved candidate run", with_op(probation(B), run(B, true)), Resumed{}, kV2, {"Commit"},
         [](auto& s) { return prob(s) && prob(s)->failed_runs == 1 && std::holds_alternative<Idle>(s.operation); }});
    row({"second unobserved candidate run rolls back", with_op(probation(B, pre_b, 1), run(B)), Resumed{}, kV2,
         {"Commit"}, [](auto& s) { return steady_on(s, A) && quarantined_for(s, B, FailureReason::UnknownOutcomeRepeated); }});
    row({"resumed with an unobserved good run", with_op(steady(), run(A)), Resumed{}, kV1, {"Commit"},
         [](auto& s) { return std::holds_alternative<Idle>(s.operation); }});
    row({"resumed when idle", steady(), Resumed{}, kV1, {}});
    row({"resumed mid-restore finishes it", with_op(steady(), Restoring{backup_v1, {"keep"}}), Resumed{}, kV3,
         {"RestoreSnapshot backup-v1 keep keep", "Commit"}, [](auto& s) { return std::holds_alternative<Idle>(s.operation); }});
    row({"start mid-restore finishes it first", with_op(steady(), Restoring{backup_v1, {"keep"}}), StartRequested{}, kV3,
         {"RestoreSnapshot backup-v1 keep keep", "Commit", "Commit", "Spawn 1.0.0.0"}});

    // Good itself failing.
    row({"good crashes before health", with_op(steady(), run(A)), AppExited{ExitKind::Abnormal}, kV1,
         {"Commit", "ShowRecovery GoodFailedToStart", "SignalBootOk"}, [](auto& s) {
             return steady_on(s, A) && std::holds_alternative<Proven>(s.find(A.id)->verdict);
         }});
    row({"good crashes after health", with_op(steady(), run(A, true)), AppExited{ExitKind::Abnormal}, kV1, {"Commit"}});
    row({"good cannot open the data", with_op(steady(), run(A)), AppExited{ExitKind::DataUnavailable}, kV1,
         {"Commit", "ShowRecovery DataUnavailable", "SignalBootOk"}});
    row({"good refuses too-new data", with_op(steady(), run(A)), AppExited{ExitKind::DataTooNew}, kV1,
         {"Commit", "ShowRecovery DataIncompatible", "SignalBootOk"}});
    row({"exit with nothing running", steady(), AppExited{ExitKind::Abnormal}, kV1, {}});

    // Staging.
    row({"seal in steady stages", steady(), BuildSealed{B, std::nullopt}, kV1, {"Commit"}, [](auto& s) {
            auto* st = std::get_if<Staged>(&s.activation);
            return st && st->build == B.id && s.find(B.id);
        }});
    row({"a newer seal replaces the staged build", staged(B), BuildSealed{C, std::nullopt}, kV1,
         {"Commit", "DeleteBuild 1.1.0.0"}, [](auto& s) { return !s.find(B.id) && std::get<Staged>(s.activation).build == C.id; }});
    row({"a quarantined build is never re-staged", add(steady(), B, Quarantined{FailureReason::InitFailed}),
         BuildSealed{B, std::nullopt}, kV1, {}});
    row({"no staging during probation", probation(B), BuildSealed{C, std::nullopt}, kV1, {}});
    row({"sealing good again (repair) changes nothing", steady(), BuildSealed{A, std::nullopt}, kV1, {}});

    // Recovery actions.
    row({"user rollback during probation", probation(B, pre_b), UserRollback{A.id, std::nullopt}, kV2,
         {"Commit", "Spawn 1.0.0.0"}, [](auto& s) { return quarantined_for(s, B, FailureReason::UserRolledBack); }});
    row({"user rollback to previous good", with_previous(steady(), D), UserRollback{D.id, std::nullopt}, kV1,
         {"Commit", "Spawn 0.9.0.0"}, [](auto& s) {
             return s.good == D.id && s.previous_good == A.id && quarantined_for(s, A, FailureReason::UserRolledBack);
         }});
    row({"user rollback onto data it cannot read", with_previous(steady(C), A), UserRollback{A.id, std::nullopt}, kV3,
         {"ShowRecovery DataIncompatible", "SignalBootOk"}});
    row({"user rollback with a chosen backup", with_previous(steady(C), A), UserRollback{A.id, backup_v1}, kV3,
         {"Commit", "RestoreSnapshot backup-v1 keep " + snap("pre-rollback", C, 11), "Commit", "Spawn 1.0.0.0"},
         [](auto& s) { return s.good == A.id && s.notice && s.notice->preserved; }});
    row({"user rollback to a build that is not previous good", with_previous(steady(), D), UserRollback{B.id, std::nullopt},
         kV1, {}});
    row({"user rollback while running", with_op(with_previous(steady(), D), run(A)), UserRollback{D.id, std::nullopt},
         kV1, {}});
    row({"user retry stages the quarantined build", add(steady(), B, Quarantined{FailureReason::InitFailed}),
         UserRetry{B.id}, kV1, {"Commit"}, [](auto& s) {
             return std::get_if<Staged>(&s.activation) && std::holds_alternative<Unproven>(s.find(B.id)->verdict);
         }});
    row({"user retry of a healthy build", add(steady(), B), UserRetry{B.id}, kV1, {}});
    row({"user restores a backup", steady(), UserRestore{backup_v1}, kV1,
         {"Commit", "RestoreSnapshot backup-v1 keep " + snap("pre-restore", A, 11), "Commit"},
         [](auto& s) { return s.notice && s.notice->kind == Notice::Kind::DataRestored; }});
    row({"user restores a backup good cannot read", steady(), UserRestore{backup_v3}, kV1,
         {"ShowRecovery DataIncompatible", "SignalBootOk"}});
    row({"no restore during probation", probation(B), UserRestore{backup_v1}, kV1, {}});

    // Corruption, revocation, channel, notices.
    row({"candidate corrupt at spawn", with_op(probation(B, pre_b), run(B)), BuildCorrupt{B.id}, kV1,
         {"Commit", "Spawn 1.0.0.0"}, [](auto& s) { return quarantined_for(s, B, FailureReason::Corrupt); }});
    row({"good corrupt at spawn", with_op(steady(), run(A)), BuildCorrupt{A.id}, kV1,
         {"Redownload 1.0.0.0", "Commit", "ShowRecovery Repairing", "SignalBootOk"}});
    row({"previous good corrupt", with_previous(steady(), D), BuildCorrupt{D.id}, kV1, {"Redownload 0.9.0.0"}});
    row({"staged corrupt", staged(B), BuildCorrupt{B.id}, kV1, {"Commit", "DeleteBuild 1.1.0.0"}});
    row({"revoked staged build", staged(B), ManifestSeen{{B.id.version}}, kV1, {"Commit", "DeleteBuild 1.1.0.0"}});
    row({"revoked candidate between runs", probation(B, pre_b), ManifestSeen{{B.id.version}}, kV2, {"Commit"},
         [](auto& s) { return quarantined_for(s, B, FailureReason::Revoked); }});
    row({"revoked candidate while it runs waits", with_op(probation(B), run(B)), ManifestSeen{{B.id.version}}, kV2, {}});
    row({"channel switch drops the staged build", staged(B), ChannelIntentSet{Channel::Beta, Follow{}}, kV1,
         {"Commit", "DeleteBuild 1.1.0.0"}, [](auto& s) { return s.channel == Channel::Beta && !s.find(B.id); }});
    row({"pin lift keeps the staged build", staged(B), ChannelIntentSet{Channel::Stable, PinnedUntilAtLeast{B.id.version}},
         kV1, {"Commit"}, [](auto& s) { return std::get_if<Staged>(&s.activation); }});
    row({"same channel and intent", steady(), ChannelIntentSet{Channel::Stable, Follow{}}, kV1, {}});
    {
        SupervisorState s = steady();
        s.notice = Notice{Notice::Kind::RolledBack, B.id.version, A.id.version, std::nullopt};
        row({"acknowledge notice", s, NoticeAcknowledged{}, kV1, {"Commit"}, [](auto& n) { return !n.notice; }});
    }
    row({"acknowledge with no notice", steady(), NoticeAcknowledged{}, kV1, {}});
    return rows;
}

bool test_table() {
    bool ok = true;
    for (const Row& r : table()) {
        Step out = step(r.state, r.event, ctx(r.live));
        std::vector<std::string> got;
        for (const auto& e : out.effects) got.push_back(describe(e));
        if (got != r.effects) {
            std::string want = "[";
            for (size_t i = 0; i < r.effects.size(); ++i) want += (i ? ", " : "") + r.effects[i];
            ok &= fail(r.name + ": effects " + describe(out.effects) + ", want " + want + "]");
            continue;
        }
        if (!r.next_ok(out.next)) ok &= fail(r.name + ": wrong next state");
        const Commit* last = nullptr;
        for (const auto& e : out.effects)
            if (auto* c = std::get_if<Commit>(&e)) last = c;
        if (last ? !(last->state == out.next) : !(out.next == r.state))
            ok &= fail(r.name + ": next is not the last committed state");
    }
    return ok;
}

} // namespace

int main() {
    bool ok = true;
    ok &= test_table();
    if (ok) std::cout << "update_probation_test: all passed\n";
    return ok ? 0 : 1;
}
