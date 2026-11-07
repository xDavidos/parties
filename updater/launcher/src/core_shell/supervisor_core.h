#pragma once

// What the launcher does around step(), the same on every platform: it reads
// the live database only while no app runs, runs each batch of effects in
// order and stops at the first failure, answers the app link, queues feed
// jobs and caps them after the app exits, fires the health and confirm
// timers, and publishes the status the app shows. The platform's event loop
// calls the on_* methods; its Executor carries out the effects. Nothing here
// blocks or sleeps.

#include "executor.h"

#include <parties/update/link.h>
#include <parties/update/probation.h>

#include <chrono>
#include <deque>
#include <optional>
#include <variant>
#include <vector>

namespace parties::update::launcher {

struct StartAgain {};
// restore: the backup the build starts on, when it cannot read the live data.
struct StartBuild {
    BuildId build;
    std::optional<Snapshot> restore;
};
struct RetryBuild { BuildId build; };
struct RestoreBackup { Snapshot snapshot; };
struct Quit {};
using RecoveryChoice = std::variant<StartAgain, StartBuild, RetryBuild, RestoreBackup, Quit>;

std::vector<Event> events_for(const RecoveryChoice& choice);

// A recovery the platform shows now, with the facts it offers choices from.
struct RecoveryDue {
    std::optional<RecoveryReason> why;  // nullopt: opened with --recover
    std::optional<DataInfo> live_db;    // nullopt: no database
    std::vector<Backup> backups;        // newest first
};

struct CoreConfig {
    std::chrono::milliseconds unit{1000};  // how long one Policy second lasts
    Policy policy{};
    std::optional<std::chrono::milliseconds> exit_after_healthy;  // ask a healthy app to exit after this long
    std::optional<std::chrono::milliseconds> job_cap;             // replaces the cap on jobs after the app exits
};

class SupervisorCore {
public:
    SupervisorCore(Executor& executor, SupervisorState committed, CoreConfig config);

    // Resumed, then StartRequested unless the user asked for recovery.
    void start(bool recover);

    // The user started Parties again.
    void on_activate();
    // Setup or uninstall asked the launcher to leave.
    void on_quit();
    // Messages that arrived on the running app's link.
    void on_link(const std::vector<link::AppMessage>& messages);
    // The running app's process is gone.
    void on_app_exited(ExitKind kind);
    // The feed worker woke: with a finished job's outcome, or nullopt for progress.
    void on_worker(std::optional<JobOutcome> outcome);
    void on_timers();
    void cancel_jobs();

    std::optional<Clock::time_point> next_deadline() const;

    // A pending recovery, once no app runs, no repair is in flight and no
    // quit is under way. Taking it clears it.
    std::optional<RecoveryDue> take_recovery();
    void on_recovery_choice(const RecoveryChoice& choice);

    // No app, no job in flight and none queued: nothing is left to supervise.
    bool idle() const;
    const SupervisorState& state() const { return state_; }

private:
    struct AppRun {
        BuildId build;
        std::unique_ptr<AppSession> session;
        Clock::time_point started;
        std::optional<Clock::time_point> healthy_at;
        bool welcomed = false;
        bool deadline_fired = false;
        bool clean_mark_fired = false;
        bool confirm_mark_fired = false;
        bool exit_sent = false;
        std::optional<link::Status> last_sent;
    };
    struct PendingRecovery {
        std::optional<RecoveryReason> why;
    };

    void dispatch(Event event);
    bool execute(const Effect& effect);
    bool spawn(const BuildId& build);
    void on_request(const link::Request& request);
    void start_check();
    void submit(Job job);
    void pump();
    bool repairing() const;
    void publish_status();
    link::Status status() const;
    std::optional<DataInfo> live_db();
    Clock::duration units(uint32_t seconds) const { return config_.unit * seconds; }

    Executor& executor_;
    CoreConfig config_;

    SupervisorState state_;  // the last state that reached disk
    std::deque<Event> queue_;
    bool draining_ = false;
    std::optional<AppRun> app_;
    std::optional<PendingRecovery> recovery_;
    std::optional<DataInfo> last_live_db_;
    FeedView feed_view_;
    bool restart_requested_ = false;
    bool quitting_ = false;
    Clock::time_point next_check_ = Clock::time_point::max();
    std::optional<Clock::time_point> jobs_cut_off_;
    std::deque<Job> jobs_;
    std::optional<Job> running_job_;
};

} // namespace parties::update::launcher
