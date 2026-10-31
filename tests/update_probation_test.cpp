// Offline tests for the supervisor state machine: an explicit transition table,
// then a seeded random walk that checks invariants over every reachable state.

#include <parties/update/probation.h>
#include <parties/update/state_codec.h>

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

SupervisorState revoked(SupervisorState s, const BuildRecord& b, Channel channel = Channel::Stable) {
    s.revoked.of(channel).push_back(b.id.version);
    return s;
}


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

    row({"steady start", steady(), StartRequested{}, kV1, {"Commit", "Spawn 1.0.0.0"},
         [](auto& s) { return running(s) && running(s)->build == A.id && !running(s)->healthy; }});
    row({"steady start on data good cannot open", steady(), StartRequested{}, kV3,
         {"ShowRecovery DataIncompatible", "SignalBootOk"}});
    row({"steady start that migrates snapshots first", steady(B), StartRequested{}, kV1,
         {"TakeSnapshot " + snap("pre", B, 11), "Commit", "Spawn 1.1.0.0"}});
    row({"steady start while running", with_op(steady(), run(A)), StartRequested{}, kV1, {}});
    row({"steady start with no database", steady(), StartRequested{}, std::nullopt, {"Commit", "Spawn 1.0.0.0"}});

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
         {"Commit", "RestoreSnapshot backup-v1 keep " + snap("pre-downgrade", D, 11), "Commit", "Spawn 0.9.0.0"},
         [](auto& s) {
             auto* p = prob(s);
             return p && p->restore_on_rollback && p->pre_snapshot && p->pre_snapshot->data == kV3 &&
                    running(s) && running(s)->build == D.id;
         }});

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
    row({"a downgrade stages a new older build", steady(C), UserDowngrade{D, backup_v1}, kV3, {"Commit"},
         [](auto& s) {
             auto* st = std::get_if<Staged>(&s.activation);
             return st && st->build == D.id && st->restore_first && st->restore_first->id.stem == "backup-v1" &&
                    std::holds_alternative<Unproven>(s.find(D.id)->verdict);
         }});
    row({"a downgrade stages previous good again", with_previous(steady(C), A), UserDowngrade{A, backup_v1}, kV3,
         {"Commit"}, [](auto& s) {
             auto* st = std::get_if<Staged>(&s.activation);
             return st && st->build == A.id && s.previous_good == A.id && s.builds.size() == 2;
         }});
    row({"a downgrade replaces the staged build", staged(B), UserDowngrade{D, std::nullopt}, kV1,
         {"Commit", "DeleteBuild 1.1.0.0"}, [](auto& s) { return std::get<Staged>(s.activation).build == D.id; }});
    row({"a downgrade onto good changes nothing", steady(), UserDowngrade{A, std::nullopt}, kV1, {}});
    row({"no downgrade during probation", probation(B), UserDowngrade{D, std::nullopt}, kV1, {}});
    row({"a downgrade never stages a quarantined build", add(steady(), D, Quarantined{FailureReason::InitFailed}),
         UserDowngrade{D, std::nullopt}, kV1, {}});

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

    row({"candidate corrupt at spawn", with_op(probation(B, pre_b), run(B)), BuildCorrupt{B.id}, kV1,
         {"Commit", "Spawn 1.0.0.0"}, [](auto& s) { return quarantined_for(s, B, FailureReason::Corrupt); }});
    row({"good corrupt at spawn", with_op(steady(), run(A)), BuildCorrupt{A.id}, kV1,
         {"Redownload 1.0.0.0", "Commit", "ShowRecovery Repairing", "SignalBootOk"}});
    row({"previous good corrupt", with_previous(steady(), D), BuildCorrupt{D.id}, kV1, {"Redownload 0.9.0.0"}});
    row({"staged corrupt", staged(B), BuildCorrupt{B.id}, kV1, {"Commit", "DeleteBuild 1.1.0.0"}});
    row({"revoked staged build", staged(B), ManifestSeen{Channel::Stable, {B.id.version}}, kV1,
         {"Commit", "DeleteBuild 1.1.0.0"}, [](auto& s) { return s.revoked.stable == std::vector{B.id.version}; }});
    row({"revoked candidate between runs", probation(B, pre_b), ManifestSeen{Channel::Stable, {B.id.version}}, kV2,
         {"Commit"}, [](auto& s) { return quarantined_for(s, B, FailureReason::Revoked); }});
    row({"revoked candidate while it runs is kept until it exits", with_op(probation(B), run(B, true)),
         ManifestSeen{Channel::Beta, {B.id.version}}, kV2, {"Commit"},
         [](auto& s) { return prob(s) && s.revoked.beta == std::vector{B.id.version} && s.revoked.stable.empty(); }});
    row({"a manifest replaces its channel's revocations", revoked(steady(), B), ManifestSeen{Channel::Stable, {}}, kV1,
         {"Commit"}, [](auto& s) { return !s.revoked.contains(B.id.version); }});
    row({"the same revocations change nothing", revoked(steady(), B), ManifestSeen{Channel::Stable, {B.id.version}},
         kV1, {}});
    row({"a revoked candidate is never confirmed", revoked(with_op(probation(B), run(B, true, 60)), B),
         ConfirmTimerElapsed{600}, kV2, {}});
    row({"a revoked candidate's clean exit rolls back", revoked(with_op(probation(B, pre_b), run(B, true, 60)), B),
         AppExited{ExitKind::Clean}, kV2, {"Commit"},
         [](auto& s) { return steady_on(s, A) && quarantined_for(s, B, FailureReason::Revoked) && !running(s); }});
    row({"a revoked candidate's crash before health starts good", revoked(with_op(probation(B, pre_b), run(B)), B),
         AppExited{ExitKind::Abnormal}, kV2, {"Commit", "Spawn 1.0.0.0"},
         [](auto& s) { return steady_on(s, A) && quarantined_for(s, B, FailureReason::Revoked); }});
    row({"an unobserved run of a revoked candidate rolls back", revoked(with_op(probation(B, pre_b), run(B, true)), B),
         Resumed{}, kV2, {"Commit"},
         [](auto& s) { return steady_on(s, A) && quarantined_for(s, B, FailureReason::Revoked); }});
    row({"a revoked candidate is not restarted", revoked(probation(B, pre_b), B), StartRequested{}, kV2,
         {"Commit", "Spawn 1.0.0.0"},
         [](auto& s) { return steady_on(s, A) && quarantined_for(s, B, FailureReason::Revoked); }});
    row({"a revoked staged build is not started", revoked(staged(B), B, Channel::Beta), StartRequested{}, kV1,
         {"Commit", "DeleteBuild 1.1.0.0", "Commit", "Spawn 1.0.0.0"}});
    row({"a revoked build is not staged", revoked(steady(), B), BuildSealed{B, std::nullopt}, kV1, {}});
    row({"a revoked build is not retried", revoked(add(steady(), B, Quarantined{FailureReason::Revoked}), B),
         UserRetry{B.id}, kV1, {}});
    row({"no rollback to a revoked previous good", revoked(with_previous(steady(), D), D),
         UserRollback{D.id, std::nullopt}, kV1, {}});
    row({"no downgrade to a revoked build", revoked(with_previous(steady(), D), D), UserDowngrade{D, std::nullopt},
         kV1, {}});
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

// The world is what the shell would observe: the database (schema info plus a
// content id, so two copies with equal schema are still told apart), the
// backups directory, the app process, and the committed state on disk.

struct Data {
    DataInfo info;
    uint64_t content = 0;
};

struct World {
    SupervisorState disk;
    std::optional<Data> live;
    std::map<std::string, Data> snapshots;
    std::optional<BuildId> app;
    bool app_opened_db = false;
    bool app_refused_db = false;
    bool app_healthy = false;
    bool app_killed = false;
    std::optional<BuildId> lease_failed;
    uint64_t next_content = 1;
};

struct Stats {
    uint64_t sequences = 0;
    uint64_t events = 0;
    uint64_t kills = 0;
    uint64_t checks = 0;
    uint64_t truncations = 0;
    uint64_t probation_exits = 0;
    uint64_t rollbacks_with_restore = 0;
    std::unordered_set<std::string> states;
};

struct Walk {
    std::mt19937_64 rng;
    Stats stats;
    std::vector<std::string> trace;
    std::string violation;

    bool check(bool cond, const std::string& what) {
        ++stats.checks;
        if (!cond && violation.empty()) violation = what;
        return cond;
    }

    size_t pick(size_t n) { return std::uniform_int_distribution<size_t>(0, n - 1)(rng); }
    bool chance(double p) { return std::bernoulli_distribution(p)(rng); }
};

const BuildRecord* universe[] = {&A, &B, &C, &D, &E};

SupervisorState normalized(SupervisorState s) {
    s.generation = 0;
    return s;
}

const Commit* last_commit(const std::vector<Effect>& effects, size_t end) {
    const Commit* last = nullptr;
    for (size_t i = 0; i < end; ++i)
        if (auto* c = std::get_if<Commit>(&effects[i])) last = c;
    return last;
}

bool has_spawn_or_recovery(const std::vector<Effect>& effects, const BuildId& build) {
    return std::ranges::any_of(effects, [&](const Effect& e) {
        auto* s = std::get_if<Spawn>(&e);
        return (s && s->build == build) || std::holds_alternative<ShowRecovery>(e);
    });
}

std::string event_name(const Event& e) {
    return std::visit(
        [](const auto& x) -> std::string {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, StartRequested>) return "StartRequested";
            else if constexpr (std::is_same_v<T, Resumed>) return "Resumed";
            else if constexpr (std::is_same_v<T, AppHealthy>) return "AppHealthy";
            else if constexpr (std::is_same_v<T, AppExited>) return "AppExited" + std::to_string(int(x.kind));
            else if constexpr (std::is_same_v<T, HealthDeadlinePassed>) return "HealthDeadlinePassed";
            else if constexpr (std::is_same_v<T, ConfirmTimerElapsed>) return "ConfirmTimer" + std::to_string(x.healthy_uptime_s);
            else if constexpr (std::is_same_v<T, BuildSealed>) return "BuildSealed " + x.record.id.version.to_string() + (x.restore_first ? " restore " + x.restore_first->id.stem : "");
            else if constexpr (std::is_same_v<T, UserDowngrade>) return "UserDowngrade " + x.record.id.version.to_string() + (x.restore_first ? " restore " + x.restore_first->id.stem : "");
            else if constexpr (std::is_same_v<T, UserRollback>) return "UserRollback " + x.target.version.to_string() + (x.restore ? " restore " + x.restore->id.stem : "");
            else if constexpr (std::is_same_v<T, UserRetry>) return "UserRetry " + x.target.version.to_string();
            else if constexpr (std::is_same_v<T, UserRestore>) return "UserRestore " + x.from.id.stem;
            else if constexpr (std::is_same_v<T, BuildCorrupt>) return "BuildCorrupt " + x.build.version.to_string();
            else if constexpr (std::is_same_v<T, ManifestSeen>) return "ManifestSeen";
            else if constexpr (std::is_same_v<T, ChannelIntentSet>) return "ChannelIntentSet";
            else return "NoticeAcknowledged";
        },
        e);
}

// User actions and store reports the shell would deliver again after a crash.
bool retriable(const Event& e) {
    return !std::holds_alternative<Resumed>(e) && !std::holds_alternative<AppHealthy>(e) &&
           !std::holds_alternative<AppExited>(e) && !std::holds_alternative<HealthDeadlinePassed>(e) &&
           !std::holds_alternative<ConfirmTimerElapsed>(e);
}


void check_state(Walk& w, const SupervisorState& s) {
    const BuildRecord* good = s.find(s.good);
    w.check(good && std::holds_alternative<Proven>(good->verdict), "good is a proven record");
    for (size_t i = 0; i < s.builds.size(); ++i)
        for (size_t j = i + 1; j < s.builds.size(); ++j)
            w.check(!(s.builds[i].id == s.builds[j].id), "build records are unique");
    if (s.previous_good) w.check(*s.previous_good != s.good && s.find(*s.previous_good), "previous_good is a distinct record");
    if (auto* st = std::get_if<Staged>(&s.activation)) {
        const BuildRecord* r = s.find(st->build);
        w.check(r && st->build != s.good && !r->quarantined(), "staged is a distinct, eligible record");
        w.check(!s.revoked.contains(st->build.version), "a revoked build is never staged");
    }
    if (auto* p = std::get_if<Probation>(&s.activation)) {
        w.check(p->candidate != s.good, "good is never the candidate");
        w.check(p->fallback == s.good, "fallback is good during probation");
        const BuildRecord* r = s.find(p->candidate);
        w.check(r && !r->quarantined(), "the candidate is an eligible record");
        auto* run = std::get_if<Running>(&s.operation);
        w.check(!s.revoked.contains(p->candidate.version) || (run && run->build == p->candidate),
                "a revoked candidate is only the app that ran when the revocation arrived");
    }
    if (auto* r = std::get_if<Running>(&s.operation)) w.check(s.find(r->build) != nullptr, "running build has a record");
}


void check_step(Walk& w, const SupervisorState& before, const Event& event, const Context& ctx, const Step& out) {
    const auto& fx = out.effects;
    const Commit* last = last_commit(fx, fx.size());
    w.check(last ? last->state == out.next : out.next == before, "next is the last committed state");
    uint64_t gen = before.generation;
    const SupervisorState* committed = &before;
    for (const Effect& e : fx) {
        if (auto* c = std::get_if<Commit>(&e)) {
            w.check(c->state.generation == ++gen, "each commit advances the generation by one");
            check_state(w, c->state);
            committed = &c->state;
        } else if (auto* sp = std::get_if<Spawn>(&e)) {
            auto* r = std::get_if<Running>(&committed->operation);
            w.check(r && r->build == sp->build && !r->healthy, "every spawn is committed as Running first");
            w.check(sp->build == committed->good || !committed->revoked.contains(sp->build.version),
                    "a revoked build is never spawned as a candidate");
        } else if (auto* rs = std::get_if<RestoreSnapshot>(&e)) {
            w.check(!rs->preserve_live_as.stem.empty() && rs->preserve_live_as != rs->from,
                    "every restore preserves the live data under its own name");
            auto* r = std::get_if<Restoring>(&committed->operation);
            w.check(r && r->from.id == rs->from && r->preserve_live_as == rs->preserve_live_as,
                    "every restore is committed as Restoring first");
        } else if (auto* d = std::get_if<DeleteBuild>(&e)) {
            const SupervisorState& s = *committed;
            bool protected_build = d->build == s.good || (s.previous_good && d->build == *s.previous_good);
            if (auto* st = std::get_if<Staged>(&s.activation)) protected_build |= st->build == d->build;
            if (auto* p = std::get_if<Probation>(&s.activation)) protected_build |= p->candidate == d->build;
            if (auto* r = std::get_if<Running>(&s.operation)) protected_build |= r->build == d->build;
            w.check(!protected_build && !s.find(d->build), "never delete a build the state still uses");
        }
    }

    // Leaving probation either confirms or returns to the fallback, and the
    // next start runs the fallback or explains why it cannot.
    if (auto* p = std::get_if<Probation>(&before.activation); p && !std::get_if<Probation>(&out.next.activation)) {
        ++w.stats.probation_exits;
        if (std::ranges::any_of(fx, [](const Effect& e) { return std::holds_alternative<RestoreSnapshot>(e); }))
            ++w.stats.rollbacks_with_restore;
        bool confirmed = out.next.good == p->candidate;
        if (confirmed) {
            w.check(out.next.previous_good == p->fallback, "confirm keeps the old good as previous_good");
        } else {
            const BuildRecord* c = out.next.find(p->candidate);
            w.check(out.next.good == p->fallback && c && c->quarantined(), "rollback quarantines the candidate");
            auto* run = std::get_if<Running>(&before.operation);
            auto* exit = std::get_if<AppExited>(&event);
            bool startup_failure = exit && run && !run->healthy && exit->kind != ExitKind::Clean &&
                                   exit->kind != ExitKind::DataUnavailable;
            if (startup_failure || std::holds_alternative<UserRollback>(event))
                w.check(has_spawn_or_recovery(fx, p->fallback), "a startup failure starts the fallback at once");
            Step resumed = step(out.next, Resumed{}, ctx);
            Step start = step(resumed.next, StartRequested{}, ctx);
            if (!std::holds_alternative<Running>(resumed.next.operation))
                w.check(has_spawn_or_recovery(start.effects, p->fallback), "after a rollback the next start runs the fallback");
        }
    }

    if (out.next.good != before.good)
        w.check(!out.next.revoked.contains(out.next.good.version), "a revoked build never becomes good");

    Step r1 = step(out.next, Resumed{}, ctx);
    Step r2 = step(r1.next, Resumed{}, ctx);
    w.check(r2.effects.empty(), "a second Resumed does nothing");

    // Killed at any effect: Resumed converges to the uninterrupted outcome, or
    // to the state before the event, or (for an event the shell redelivers)
    // reaches the uninterrupted outcome once the event arrives again.
    if (!fx.empty()) {
        SupervisorState done = normalized(r1.next);
        SupervisorState lost = normalized(step(before, Resumed{}, ctx).next);
        const Commit* previous = nullptr;
        for (size_t i = 0; i < fx.size(); ++i) {
            ++w.stats.truncations;
            const Commit* c = last_commit(fx, i);
            // Truncations that share the same last commit leave the same disk.
            if (i > 0 && c == previous) continue;
            previous = c;
            const SupervisorState& on_disk = c ? c->state : before;
            Step recovered = step(on_disk, Resumed{}, ctx);
            SupervisorState got = normalized(recovered.next);
            bool converged = got == done || got == lost;
            if (!converged && retriable(event)) {
                SupervisorState again = normalized(step(step(recovered.next, event, ctx).next, Resumed{}, ctx).next);
                converged = again == done;
            }
            w.check(converged, "kill at effect " + std::to_string(i) + " of " + describe(fx) + " did not converge");
            // A restore that was committed but cut off is finished by Resumed.
            if (c && std::holds_alternative<Restoring>(c->state.operation))
                w.check(!recovered.effects.empty() && std::holds_alternative<RestoreSnapshot>(recovered.effects[0]),
                        "Resumed finishes a committed restore first");
        }
    }
}


void open_db(World& world, const BuildRecord& build) {
    world.app_opened_db = true;
    Compatibility c = world.live ? compatibility(build.data_schema, world.live->info) : Compatibility::MigratesForward;
    if (c == Compatibility::Incompatible) {
        world.app_refused_db = true;
        return;
    }
    DataInfo info = world.live ? world.live->info : DataInfo{build.data_schema, build.data_compat_floor};
    if (c == Compatibility::MigratesForward && world.live)
        info = {build.data_schema, std::max(info.compat_floor, build.data_compat_floor)};
    world.live = Data{info, world.next_content++};
}

void apply(Walk& w, World& world, const Effect& e) {
    if (auto* t = std::get_if<TakeSnapshot>(&e)) {
        if (!w.check(world.live.has_value(), "snapshot only when there is data")) return;
        auto it = world.snapshots.find(t->id.stem);
        w.check(it == world.snapshots.end() || it->second.content == world.live->content,
                "a snapshot name is never reused for different data");
        world.snapshots[t->id.stem] = *world.live;
    } else if (auto* c = std::get_if<Commit>(&e)) {
        w.check(c->state.generation == world.disk.generation + 1, "commits reach disk in order");
        world.disk = c->state;
    } else if (auto* sp = std::get_if<Spawn>(&e)) {
        w.check(!world.app, "never two app processes");
        const BuildRecord* r = world.disk.find(sp->build);
        if (!w.check(r != nullptr, "spawned build has a record")) return;
        if (world.live) {
            Compatibility c = compatibility(r->data_schema, world.live->info);
            w.check(c != Compatibility::Incompatible, "never spawn a build onto data it cannot open");
            if (c == Compatibility::MigratesForward) {
                bool saved = std::ranges::any_of(world.snapshots, [&](const auto& kv) {
                    return kv.second.content == world.live->content;
                });
                w.check(saved, "a copy of the data exists before a build migrates it");
            }
        }
        if (w.chance(0.03)) {
            world.lease_failed = sp->build;
            return;
        }
        world.app = sp->build;
        world.app_opened_db = world.app_refused_db = world.app_healthy = world.app_killed = false;
        if (w.chance(0.7)) open_db(world, *r);
    } else if (std::holds_alternative<TerminateApp>(e)) {
        w.check(world.app.has_value(), "terminate only a running app");
        world.app_killed = true;
    } else if (auto* rs = std::get_if<RestoreSnapshot>(&e)) {
        w.check(!world.app, "restore only while the app is not running");
        auto from = world.snapshots.find(rs->from.stem);
        if (!w.check(from != world.snapshots.end(), "restore from a snapshot that exists")) return;
        bool replay = world.live && world.live->content == from->second.content &&
                      world.snapshots.contains(rs->preserve_live_as.stem);
        if (replay) return;
        w.check(!world.snapshots.contains(rs->preserve_live_as.stem), "a restore never overwrites a preserved copy");
        if (world.live) world.snapshots[rs->preserve_live_as.stem] = *world.live;
        world.live = from->second;
    }
}


std::optional<Snapshot> random_snapshot(Walk& w, const World& world) {
    if (world.snapshots.empty() || w.chance(0.3)) return std::nullopt;
    auto it = world.snapshots.begin();
    std::advance(it, w.pick(world.snapshots.size()));
    return Snapshot{{it->first}, it->second.info};
}

BuildId random_known_id(Walk& w, const SupervisorState& s) {
    if (w.chance(0.15)) return universe[w.pick(5)]->id;
    return s.builds[w.pick(s.builds.size())].id;
}

Event app_event(Walk& w, const World& world) {
    if (world.app_killed) return AppExited{ExitKind::KilledHung};
    if (world.app_refused_db) return AppExited{ExitKind::DataTooNew};
    switch (w.pick(10)) {
    case 0:
    case 1:
    case 2: return AppHealthy{};
    case 3: return HealthDeadlinePassed{};
    case 4: return ConfirmTimerElapsed{uint32_t(std::array{30, 60, 600, 700}[w.pick(4)])};
    default: {
        constexpr ExitKind kinds[] = {ExitKind::Clean, ExitKind::Clean, ExitKind::InitFailed, ExitKind::DataUnavailable,
                                      ExitKind::Abnormal, ExitKind::Abnormal, ExitKind::Terminated};
        return AppExited{kinds[w.pick(std::size(kinds))]};
    }
    }
}

Event other_event(Walk& w, const World& world, const SupervisorState& s) {
    switch (w.pick(16)) {
    case 0:
    case 1:
    case 2:
    case 3:
    case 4: return StartRequested{};
    case 5:
    case 6: {
        const BuildRecord& r = *universe[1 + w.pick(4)];
        std::optional<Snapshot> restore = w.chance(0.2) ? random_snapshot(w, world) : std::nullopt;
        if (w.chance(0.3)) {
            if (s.previous_good && w.chance(0.5)) return UserDowngrade{*s.find(*s.previous_good), restore};
            return UserDowngrade{r, restore};
        }
        return BuildSealed{r, restore};
    }
    case 7: {
        BuildId target = s.previous_good && w.chance(0.7) ? *s.previous_good : random_known_id(w, s);
        if (auto* p = std::get_if<Probation>(&s.activation); p && w.chance(0.5)) target = p->fallback;
        return UserRollback{target, w.chance(0.4) ? random_snapshot(w, world) : std::nullopt};
    }
    case 8: return UserRetry{random_known_id(w, s)};
    case 9: {
        auto snap_choice = random_snapshot(w, world);
        if (snap_choice) return UserRestore{*snap_choice};
        return StartRequested{};
    }
    case 10: {
        BuildId id = random_known_id(w, s);
        if (world.app && *world.app == id) return StartRequested{};
        return BuildCorrupt{id};
    }
    case 11: {
        ManifestSeen m{w.chance(0.5) ? Channel::Stable : Channel::Beta, {}};
        for (const BuildRecord* r : universe)
            if (w.chance(0.2)) m.revoked.push_back(r->id.version);
        return m;
    }
    case 12: {
        Channel c = w.chance(0.5) ? Channel::Stable : Channel::Beta;
        if (w.chance(0.5)) return ChannelIntentSet{c, Follow{}};
        return ChannelIntentSet{c, PinnedUntilAtLeast{universe[w.pick(5)]->id.version}};
    }
    case 13: return NoticeAcknowledged{};
    case 14: return Resumed{};
    default: return w.chance(0.5) ? Event{AppHealthy{}} : Event{AppExited{ExitKind::Abnormal}};
    }
}

Event next_event(Walk& w, const World& world, const SupervisorState& s) {
    if (world.lease_failed) return BuildCorrupt{*world.lease_failed};
    if (world.app && w.chance(0.75)) return app_event(w, world);
    return other_event(w, world, s);
}

// Feeds one event, checks it, and executes its effects, possibly killing the
// launcher partway through.
void feed(Walk& w, World& world, SupervisorState& state, const Event& event, bool allow_kill) {
    ++w.stats.events;
    w.trace.push_back(event_name(event));
    w.stats.states.insert(write_state(normalized(state)));

    // Resumed means the launcher restarted. The shell runs the app in a
    // kill-on-close job, so an app never outlives the launcher that spawned it.
    if (std::holds_alternative<Resumed>(event)) world.app.reset();
    if (std::holds_alternative<BuildCorrupt>(event)) world.lease_failed.reset();
    if (std::holds_alternative<AppExited>(event) && world.app) {
        if (!world.app_opened_db && w.chance(0.5)) {
            if (const BuildRecord* r = world.disk.find(*world.app)) open_db(world, *r);
        }
        world.app.reset();
    }
    if (std::holds_alternative<AppHealthy>(event) && world.app) {
        if (!world.app_opened_db)
            if (const BuildRecord* r = world.disk.find(*world.app)) open_db(world, *r);
        world.app_healthy = true;
    }

    Context context = ctx(world.live ? std::optional(world.live->info) : std::nullopt);
    Step out = step(state, event, context);
    check_step(w, state, event, context, out);

    size_t limit = out.effects.size();
    bool kill = allow_kill && limit > 0 && w.chance(0.08);
    if (kill) limit = w.pick(out.effects.size());
    for (size_t i = 0; i < limit && w.violation.empty(); ++i) apply(w, world, out.effects[i]);
    if (!kill) {
        state = out.next;
        w.check(normalized(state) == normalized(world.disk) || !last_commit(out.effects, out.effects.size()),
                "disk holds the state the step returned");
        return;
    }
    ++w.stats.kills;
    w.trace.push_back("<kill at " + std::to_string(limit) + ">");
    world.app.reset();
    state = world.disk;
    feed(w, world, state, Resumed{}, false);
}

bool test_random_walk() {
    constexpr uint64_t kSequences = 100000;
    Walk w{std::mt19937_64(0x5eed'2026'1031ULL)};
    for (uint64_t seq = 0; seq < kSequences && w.violation.empty(); ++seq) {
        ++w.stats.sequences;
        w.trace.clear();
        World world;
        SupervisorState state = initial_state(A, Channel::Stable);
        world.disk = state;
        if (w.chance(0.9)) world.live = Data{kV1, world.next_content++};
        size_t length = 1 + w.pick(30);
        for (size_t i = 0; i < length && w.violation.empty(); ++i) {
            Event e = next_event(w, world, state);
            feed(w, world, state, e, true);
            check_state(w, state);
        }
    }
    std::cout << "update_probation_test: random walk sequences=" << w.stats.sequences << " events=" << w.stats.events
              << " kills=" << w.stats.kills << " distinct_states=" << w.stats.states.size()
              << " probation_exits=" << w.stats.probation_exits
              << " rollbacks_with_restore=" << w.stats.rollbacks_with_restore
              << " kill_truncations=" << w.stats.truncations << " invariant_checks=" << w.stats.checks << "\n";
    if (!w.violation.empty()) {
        std::string t;
        for (const auto& e : w.trace) t += "\n  " + e;
        return fail("random walk violated: " + w.violation + "\ntrace:" + t);
    }
    return true;
}

} // namespace

int main() {
    bool ok = true;
    ok &= test_table();
    ok &= test_random_walk();
    if (ok) std::cout << "update_probation_test: all passed\n";
    return ok ? 0 : 1;
}
