#include "supervisor_core.h"

#include "log.h"

#include <parties/update/update_policy.h>

#include <algorithm>

namespace parties::update::launcher {

namespace {

constexpr auto kCheckInterval = std::chrono::hours(6);
// How long a check or download may keep the launcher running after the app
// exits: a full build over a slow link, but not a hidden process all day.
constexpr auto kJobCapAfterExit = std::chrono::minutes(15);
// How often, and how long, a decision waits for data another process holds.
constexpr auto kDataRetry = std::chrono::milliseconds(250);
constexpr auto kDataWaitCap = std::chrono::minutes(2);

link::StatusNotice wire_notice(const Notice& n) {
    link::StatusNotice out;
    switch (n.kind) {
    case Notice::Kind::RolledBack: out.kind = link::StatusNotice::Kind::RolledBack; break;
    case Notice::Kind::DataRestored: out.kind = link::StatusNotice::Kind::DataRestored; break;
    case Notice::Kind::Withdrawn: out.kind = link::StatusNotice::Kind::Withdrawn; break;
    case Notice::Kind::RestoreFailed: out.kind = link::StatusNotice::Kind::RestoreFailed; break;
    }
    out.from = n.from;
    out.to = n.to;
    if (n.preserved) out.preserved = "backups\\" + n.preserved->stem + ".db";
    return out;
}

} // namespace

std::vector<Event> events_for(const RecoveryChoice& choice) {
    return std::visit(
        [](const auto& c) -> std::vector<Event> {
            using T = std::decay_t<decltype(c)>;
            if constexpr (std::is_same_v<T, StartAgain>) return {StartRequested{}};
            else if constexpr (std::is_same_v<T, StartBuild>) return {UserRollback{c.build, c.restore}};
            else if constexpr (std::is_same_v<T, RetryBuild>) return {UserRetry{c.build}, StartRequested{}};
            else if constexpr (std::is_same_v<T, RestoreBackup>) return {UserRestore{c.snapshot}, StartRequested{}};
            else return {};
        },
        choice);
}

SupervisorCore::SupervisorCore(Executor& executor, SupervisorState committed, CoreConfig config)
    : executor_(executor), config_(config), state_(std::move(committed)) {}

void SupervisorCore::start(bool recover) {
    if (recover) recovery_ = PendingRecovery{std::nullopt};
    dispatch(Resumed{});
    if (!recover) dispatch(StartRequested{});
}

std::optional<DataInfo> SupervisorCore::live_db() {
    if (app_) return last_live_db_;
    auto info = executor_.inspect_live_db();
    if (!info) {
        log("cannot read the live database: {}", info.error().why);
        return last_live_db_ = std::nullopt;
    }
    return last_live_db_ = *info;
}

// Deciding on data another process may still be changing could restore the
// wrong copy, or none, so the queue waits for it. A committed restore needs no
// decision: its effect meets the held data itself and fails like any effect.
bool SupervisorCore::wait_for_data() {
    if (app_ || std::holds_alternative<Restoring>(state_.operation)) return false;
    auto info = executor_.inspect_live_db();
    if (info || !info.error().busy) {
        data_wait_since_.reset();
        return false;
    }
    const auto now = executor_.now();
    if (!data_wait_since_) {
        log("waiting to read the live database: {}", info.error().why);
        data_wait_since_ = now;
    }
    if (now - *data_wait_since_ >= kDataWaitCap) {
        log("gave up waiting for the live database; the next start decides again");
        queue_.clear();
        data_wait_since_.reset();
        return true;
    }
    data_retry_ = now + kDataRetry;
    return true;
}

void SupervisorCore::dispatch(Event event) {
    queue_.push_back(std::move(event));
    drain();
}

void SupervisorCore::drain() {
    if (draining_ || data_retry_) return;
    draining_ = true;
    while (!queue_.empty()) {
        if (wait_for_data()) break;
        Event next = std::move(queue_.front());
        queue_.pop_front();
        Context ctx{live_db(), config_.policy};
        Step result = step(state_, next, ctx);
        for (const Effect& effect : result.effects) {
            executor_.before_effect();
            if (!execute(effect)) break;
        }
    }
    draining_ = false;
    publish_status();
}

bool SupervisorCore::execute(const Effect& effect) {
    std::string error;
    bool ok = std::visit(
        [&](const auto& e) -> bool {
            using T = std::decay_t<decltype(e)>;
            if constexpr (std::is_same_v<T, TakeSnapshot>) {
                if (app_) return (error = "the app is running", false);
                return executor_.take_snapshot(e.id, state_, error);
            } else if constexpr (std::is_same_v<T, Commit>) {
                if (!executor_.commit(e.state, state_.good, error)) return false;
                state_ = e.state;
                return true;
            } else if constexpr (std::is_same_v<T, Spawn>) {
                return spawn(e.build);
            } else if constexpr (std::is_same_v<T, TerminateApp>) {
                if (!app_) return true;
                app_->session->terminate();
                log("killed {} after it missed the health deadline", app_->build.dir_name());
                app_.reset();
                queue_.push_back(AppExited{ExitKind::KilledHung});
                return true;
            } else if constexpr (std::is_same_v<T, RestoreSnapshot>) {
                if (app_) return (error = "the app is running", false);
                auto restored = executor_.restore_snapshot(e.from, e.preserve_live_as);
                if (restored) return true;
                error = restored.error().why;
                // A restore that cannot even start would otherwise stop every
                // launch here; one that changed the live data, or that an app
                // still holding the data kept out, is resumed instead.
                if (!restored.error().live_changed && !restored.error().data_busy) {
                    queue_.push_back(RestoreFailed{});
                    if (!recovery_) queue_.push_back(StartRequested{});
                }
                return false;
            } else if constexpr (std::is_same_v<T, DeleteBuild>) {
                // A leftover directory is untracked, so the next start collects it.
                if (!executor_.delete_build(e.build, state_, error))
                    log("cannot delete {}: {}", e.build.dir_name(), error);
                return true;
            } else if constexpr (std::is_same_v<T, Redownload>) {
                if (!executor_.feed_available())
                    log("{} needs to be downloaded again, but there is no update feed", e.build.dir_name());
                else submit(RepairJob{e.build});
                return true;
            } else if constexpr (std::is_same_v<T, ShowRecovery>) {
                recovery_ = PendingRecovery{e.why};
                return true;
            } else {
                executor_.signal_boot_ok();
                return true;
            }
        },
        effect);
    if (!ok && !error.empty()) log("effect failed: {}", error);
    return ok;
}

bool SupervisorCore::spawn(const BuildId& build) {
    const BuildRecord* record = state_.find(build);
    auto session = record ? executor_.spawn(*record) : std::unexpected(SpawnFailure::BuildCorrupt);
    if (!session) {
        if (session.error() == SpawnFailure::BuildCorrupt) queue_.push_back(BuildCorrupt{build});
        else queue_.push_back(AppExited{ExitKind::Abnormal});
        return false;
    }
    app_.emplace(AppRun{build, std::move(*session), executor_.now()});
    jobs_cut_off_.reset();
    return true;
}

void SupervisorCore::on_app_exited(ExitKind kind) {
    app_.reset();
    dispatch(AppExited{kind});
    after_app();
}

void SupervisorCore::on_app_corrupt() {
    if (!app_) return;
    const BuildId build = app_->build;
    app_.reset();
    dispatch(BuildCorrupt{build});
    after_app();
}

void SupervisorCore::after_app() {
    if (quitting_) {
        on_quit();
        return;
    }
    if (restart_requested_ && !app_) {
        restart_requested_ = false;
        dispatch(StartRequested{});
    }
    if (app_) return;
    // The job in flight finishes, or a user whose sessions are shorter than a
    // download would never get an update. Nobody is left to see the queued
    // checks and downgrades; a queued repair is what lets the next start succeed.
    std::erase_if(jobs_, [](const Job& j) { return !std::holds_alternative<RepairJob>(j); });
    jobs_cut_off_ = executor_.now() + config_.job_cap.value_or(kJobCapAfterExit);
}

void SupervisorCore::on_link(const std::vector<link::AppMessage>& messages) {
    for (const link::AppMessage& message : messages) {
        if (!app_) return;
        if (auto* hello = std::get_if<link::Hello>(&message)) {
            auto protocol = link::negotiate(*hello, kLinkProtocol);
            if (!protocol) {
                log("app {} cannot talk link protocol {}", hello->version.to_string(), kLinkProtocol);
                continue;
            }
            app_->session->send(link::Welcome{*protocol});
            app_->welcomed = true;
            app_->last_sent.reset();
            publish_status();
            if (quitting_) on_quit();
        } else if (auto* healthy = std::get_if<link::Healthy>(&message)) {
            if (app_->healthy_at) continue;
            log("app reported healthy (data schema {})", healthy->data_schema);
            app_->healthy_at = executor_.now();
            dispatch(AppHealthy{});
            // The first check of a launcher run waits for the app, so it never
            // competes with startup.
            if (next_check_ == Clock::time_point::max()) start_check();
        } else if (auto* request = std::get_if<link::Request>(&message)) {
            on_request(*request);
        }
    }
}

void SupervisorCore::on_request(const link::Request& request) {
    std::visit(
        [&](const auto& r) {
            using T = std::decay_t<decltype(r)>;
            if constexpr (std::is_same_v<T, link::RestartToUpdate>) {
                if (!std::holds_alternative<Staged>(state_.activation) || !app_) return;
                log("restarting into the staged build");
                restart_requested_ = true;
                app_->exit_sent = app_->session->send(link::ExitRequested{});
            } else if constexpr (std::is_same_v<T, link::CheckNow>) {
                start_check();
            } else if constexpr (std::is_same_v<T, link::AcknowledgeNotice>) {
                dispatch(NoticeAcknowledged{});
            } else {
                if (r.choice == link::ChannelChoice::DowngradeNow) {
                    submit(DowngradeJob{r.channel, state_.channel, state_.intent});
                    return;
                }
                ChannelIntent intent =
                    r.choice == link::ChannelChoice::Wait ? wait_intent(state_) : ChannelIntent{Follow{}};
                dispatch(ChannelIntentSet{r.channel, intent});
                start_check();
            }
        },
        request);
}

void SupervisorCore::on_quit() {
    quitting_ = true;
    restart_requested_ = false;
    // Setup and uninstall replace the install; a download in flight is not worth waiting for.
    cancel_jobs();
    if (app_ && !app_->exit_sent) app_->exit_sent = app_->session->send(link::ExitRequested{});
}

void SupervisorCore::on_activate() {
    if (quitting_) return;
    if (app_) {
        app_->session->activate();
        return;
    }
    dispatch(StartRequested{});
}

void SupervisorCore::on_worker(std::optional<JobOutcome> outcome) {
    if (!outcome) {
        publish_status();
        return;
    }
    running_job_.reset();
    auto* downgrade = std::get_if<DowngradeJob>(&outcome->job);
    if (std::holds_alternative<CheckJob>(outcome->job) && outcome->channel != state_.channel) {
        log("dropping a check of the channel the user just left");
        if (app_) submit(CheckJob{});
    } else if (downgrade && (downgrade->asked_on != state_.channel || downgrade->asked_with != state_.intent)) {
        log("dropping a switch to {} the user withdrew", channel_name(downgrade->channel));
        if (app_) submit(CheckJob{});
    } else if (std::holds_alternative<RepairJob>(outcome->job)) {
        for (Event& e : outcome->events) dispatch(std::move(e));
        if (outcome->ok && recovery_ && recovery_->why == RecoveryReason::Repairing && !app_ && !repairing()) {
            log("repaired; starting again");
            recovery_.reset();
            dispatch(StartRequested{});
        }
    } else {
        feed_view_ = outcome->view;
        for (Event& e : outcome->events) dispatch(std::move(e));
        if (std::holds_alternative<DowngradeJob>(outcome->job) && app_) submit(CheckJob{});
    }
    pump();
    publish_status();
}

void SupervisorCore::start_check() {
    next_check_ = executor_.now() + kCheckInterval;
    submit(CheckJob{});
}

void SupervisorCore::submit(Job job) {
    if (!executor_.feed_available()) return;
    bool duplicate = std::ranges::find(jobs_, job) != jobs_.end() ||
                     (std::holds_alternative<CheckJob>(job) && running_job_ == job);
    if (!duplicate) jobs_.push_back(std::move(job));
    pump();
}

void SupervisorCore::cancel_jobs() {
    jobs_.clear();
    executor_.cancel_job();
}

void SupervisorCore::pump() {
    if (running_job_ || jobs_.empty() || executor_.job_busy()) return;
    Job job = jobs_.front();
    std::vector<Backup> backups =
        std::holds_alternative<RepairJob>(job) ? std::vector<Backup>{} : executor_.list_backups();
    if (!executor_.start_job(JobRequest{job, state_, last_live_db_, std::move(backups)})) return;
    jobs_.pop_front();
    running_job_ = std::move(job);
    publish_status();
}

bool SupervisorCore::repairing() const {
    auto repair = [](const Job& j) { return std::holds_alternative<RepairJob>(j); };
    return (running_job_ && repair(*running_job_)) || std::ranges::any_of(jobs_, repair);
}

bool SupervisorCore::idle() const {
    // running_job_, not job_busy(): a job that just ended still has an
    // outcome to commit.
    return !app_ && !running_job_ && jobs_.empty() && !data_retry_;
}

std::optional<Clock::time_point> SupervisorCore::next_deadline() const {
    std::optional<Clock::time_point> next;
    auto consider = [&](Clock::time_point t) {
        if (!next || t < *next) next = t;
    };
    if (data_retry_) consider(*data_retry_);
    if (!app_) {
        if (jobs_cut_off_) consider(*jobs_cut_off_);
        return next;
    }
    if (executor_.feed_available()) consider(next_check_);
    if (!app_->healthy_at) {
        if (!app_->deadline_fired) consider(app_->started + units(config_.policy.health_deadline_s));
        return next;
    }
    if (!app_->clean_mark_fired) consider(*app_->healthy_at + units(config_.policy.confirm_on_clean_exit_after_s));
    if (!app_->confirm_mark_fired) consider(*app_->healthy_at + units(config_.policy.confirm_uptime_s));
    if (auto exit_after = config_.exit_after_healthy; exit_after && !app_->exit_sent)
        consider(*app_->healthy_at + *exit_after);
    return next;
}

void SupervisorCore::on_timers() {
    const auto now = executor_.now();
    if (data_retry_ && now >= *data_retry_) {
        data_retry_.reset();
        drain();
    }
    if (!app_) {
        if (jobs_cut_off_ && now >= *jobs_cut_off_) {
            jobs_cut_off_.reset();
            if (running_job_) log("giving up on the update job: the app exited too long ago");
            cancel_jobs();
        }
        return;
    }
    if (executor_.feed_available() && now >= next_check_) start_check();
    if (!app_->healthy_at) {
        if (!app_->deadline_fired && now >= app_->started + units(config_.policy.health_deadline_s)) {
            app_->deadline_fired = true;
            dispatch(HealthDeadlinePassed{});
        }
        return;
    }
    const Clock::time_point healthy = *app_->healthy_at;
    if (!app_->clean_mark_fired && now >= healthy + units(config_.policy.confirm_on_clean_exit_after_s)) {
        app_->clean_mark_fired = true;
        dispatch(ConfirmTimerElapsed{config_.policy.confirm_on_clean_exit_after_s});
        if (!app_) return;
    }
    if (!app_->confirm_mark_fired && now >= healthy + units(config_.policy.confirm_uptime_s)) {
        app_->confirm_mark_fired = true;
        dispatch(ConfirmTimerElapsed{config_.policy.confirm_uptime_s});
        if (!app_) return;
    }
    if (auto exit_after = config_.exit_after_healthy; exit_after && !app_->exit_sent && now >= healthy + *exit_after) {
        log("asking the app to exit (testing)");
        app_->session->send(link::ExitRequested{});
        app_->exit_sent = true;
    }
}

link::Status SupervisorCore::status() const {
    link::Status s;
    s.channel = state_.channel;
    s.current = app_ ? app_->build.version : state_.good.version;
    if (auto download = executor_.download()) {
        s.phase = link::Phase::Downloading;
        s.percent = download->percent;
        s.available = download->version;
    } else if (executor_.job_busy()) {
        s.phase = link::Phase::Checking;
    } else if (auto* staged = std::get_if<Staged>(&state_.activation)) {
        s.phase = link::Phase::ReadyToRestart;
        s.available = staged->build.version;
    } else {
        s.phase = feed_view_.phase;
        s.available = feed_view_.available;
    }
    if (auto* pin = std::get_if<PinnedUntilAtLeast>(&state_.intent)) {
        s.pinned_until = pin->floor;
        if (s.phase == link::Phase::Idle) s.phase = link::Phase::Pinned;
    }
    s.downgrade = feed_view_.downgrade;
    if (state_.notice) s.notice = wire_notice(*state_.notice);
    return s;
}

void SupervisorCore::publish_status() {
    if (!app_ || !app_->welcomed) return;
    link::Status s = status();
    if (app_->last_sent == s) return;
    if (app_->session->send(s)) app_->last_sent = s;
}

std::optional<RecoveryDue> SupervisorCore::take_recovery() {
    // A repair in flight may make the recovery dialog unnecessary.
    if (!recovery_ || app_ || repairing() || quitting_ || data_retry_) return std::nullopt;
    PendingRecovery pending = *recovery_;
    recovery_.reset();
    std::optional<DataInfo> live = live_db();
    return RecoveryDue{pending.why, live, executor_.list_backups()};
}

void SupervisorCore::on_recovery_choice(const RecoveryChoice& choice) {
    for (Event& e : events_for(choice)) dispatch(std::move(e));
}

} // namespace parties::update::launcher
