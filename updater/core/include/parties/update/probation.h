#pragma once

// The whole "which build runs, when do we roll back" policy as one pure
// transition function. The launcher shell feeds events, executes the effects
// in order, and stops at the first failure; on its next start it feeds
// Resumed, which re-derives whatever was left undone from committed state.

#include <parties/update/supervisor_state.h>

#include <cstdint>
#include <optional>
#include <variant>
#include <vector>

namespace parties::update {

// How the app process ended, classified by the shell from its exit code.
enum class ExitKind : uint8_t {
    Clean,            // 0, or session end
    InitFailed,       // 2
    DataUnavailable,  // 3: the data, not the build, is at fault
    DataTooNew,       // 4: the build refused a database above its floor
    Abnormal,         // crash codes and anything unknown
    Terminated,       // 1: killed from outside (Task Manager)
    KilledHung,       // the shell killed it after TerminateApp
};

// --- Events ---
// The shell feeds Resumed once at launcher start, before anything else.
struct StartRequested { friend bool operator==(const StartRequested&, const StartRequested&) = default; };
struct Resumed { friend bool operator==(const Resumed&, const Resumed&) = default; };
struct AppHealthy { friend bool operator==(const AppHealthy&, const AppHealthy&) = default; };
struct AppExited {
    ExitKind kind;
    friend bool operator==(const AppExited&, const AppExited&) = default;
};
// Policy::health_deadline_s after a spawn without AppHealthy.
struct HealthDeadlinePassed { friend bool operator==(const HealthDeadlinePassed&, const HealthDeadlinePassed&) = default; };
// Fired at confirm_on_clean_exit_after_s and confirm_uptime_s of healthy uptime.
struct ConfirmTimerElapsed {
    uint32_t healthy_uptime_s;
    friend bool operator==(const ConfirmTimerElapsed&, const ConfirmTimerElapsed&) = default;
};
struct BuildSealed {
    BuildRecord record;
    std::optional<Snapshot> restore_first;
    friend bool operator==(const BuildSealed&, const BuildSealed&) = default;
};
// Recovery: "start previous version", optionally on a chosen backup.
struct UserRollback {
    BuildId target;
    std::optional<Snapshot> restore;
    friend bool operator==(const UserRollback&, const UserRollback&) = default;
};
// The user chose an immediate downgrade (set_channel downgrade_now). The shell
// sealed `record`, or found it on disk as previous_good, and picked
// restore_first when the build cannot read the live data.
struct UserDowngrade {
    BuildRecord record;
    std::optional<Snapshot> restore_first;
    friend bool operator==(const UserDowngrade&, const UserDowngrade&) = default;
};
// Recovery: "try the failed build again".
struct UserRetry {
    BuildId target;
    friend bool operator==(const UserRetry&, const UserRetry&) = default;
};
// Recovery: "restore a dated backup" for the current build.
struct UserRestore {
    Snapshot from;
    friend bool operator==(const UserRestore&, const UserRestore&) = default;
};
// A missing or modified file: from the exec lease of a Spawn that therefore
// started nothing, or from a scan of a build that is not running.
struct BuildCorrupt {
    BuildId build;
    friend bool operator==(const BuildCorrupt&, const BuildCorrupt&) = default;
};
// An accepted manifest of `channel`. Its revoked[] replaces the list kept for
// that channel.
struct ManifestSeen {
    Channel channel = Channel::Stable;
    std::vector<Version> revoked;
    friend bool operator==(const ManifestSeen&, const ManifestSeen&) = default;
};
struct ChannelIntentSet {
    Channel channel;
    ChannelIntent intent;
    friend bool operator==(const ChannelIntentSet&, const ChannelIntentSet&) = default;
};
struct NoticeAcknowledged { friend bool operator==(const NoticeAcknowledged&, const NoticeAcknowledged&) = default; };

using Event = std::variant<StartRequested, Resumed, AppHealthy, AppExited, HealthDeadlinePassed,
                           ConfirmTimerElapsed, BuildSealed, UserDowngrade, UserRollback, UserRetry,
                           UserRestore, BuildCorrupt, ManifestSeen, ChannelIntentSet, NoticeAcknowledged>;

struct Policy {
    uint32_t health_deadline_s = 120;
    uint32_t confirm_uptime_s = 600;
    uint32_t confirm_on_clean_exit_after_s = 60;
    uint8_t max_failed_runs_after_healthy = 2;
};

// Facts the shell reads while no app is running.
struct Context {
    std::optional<DataInfo> live_db;  // nullopt: no database yet
    Policy policy{};
};

// --- Effects: ordered, each idempotent. ---
// Write a fresh copy of the live database to backups\<id>.db (tmp + rename).
struct TakeSnapshot {
    SnapshotId id;
    friend bool operator==(const TakeSnapshot&, const TakeSnapshot&) = default;
};
// Durably write supervisor.json, then derive boot-contract.json.
struct Commit {
    SupervisorState state;
    friend bool operator==(const Commit&, const Commit&) = default;
};
// Lease the build's files, check digests, start the app.
struct Spawn {
    BuildId build;
    friend bool operator==(const Spawn&, const Spawn&) = default;
};
// Kill the app, wait for it, and report its exit as ExitKind::KilledHung.
struct TerminateApp { friend bool operator==(const TerminateApp&, const TerminateApp&) = default; };
// Keep the live database as backups\<preserve_live_as>.db, then put `from` in
// place. Done when the live database already has from's content.
struct RestoreSnapshot {
    SnapshotId from;
    SnapshotId preserve_live_as;
    friend bool operator==(const RestoreSnapshot&, const RestoreSnapshot&) = default;
};
// Never good, previous_good, staged or candidate of the last commit.
struct DeleteBuild {
    BuildId build;
    friend bool operator==(const DeleteBuild&, const DeleteBuild&) = default;
};
// Fetch the same digests again; the store reports BuildSealed when done.
struct Redownload {
    BuildId build;
    friend bool operator==(const Redownload&, const Redownload&) = default;
};
enum class RecoveryReason : uint8_t { GoodFailedToStart, DataUnavailable, DataIncompatible, Repairing };
struct ShowRecovery {
    RecoveryReason why;
    friend bool operator==(const ShowRecovery&, const ShowRecovery&) = default;
};
// Tell the boot stub this launcher works. Repeating it is harmless.
struct SignalBootOk { friend bool operator==(const SignalBootOk&, const SignalBootOk&) = default; };

using Effect = std::variant<TakeSnapshot, Commit, Spawn, TerminateApp, RestoreSnapshot, DeleteBuild,
                            Redownload, ShowRecovery, SignalBootOk>;

struct Step {
    SupervisorState next;  // the state of the last Commit, or the input if none
    std::vector<Effect> effects;
};

Step step(const SupervisorState& state, const Event& event, const Context& ctx);

} // namespace parties::update
