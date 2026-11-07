// SupervisorCore against a scripted Executor. Each scenario feeds the core the
// stimuli a platform loop would (start, link messages, exits, timers, worker
// outcomes, recovery choices) and checks that the effects the executor carried
// out are exactly what step() returns for the events those stimuli mean, in
// order, cut at the first failure.

#include "supervisor_core.h"

#include <parties/update/probation.h>

#include <algorithm>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

using namespace parties::update;
using namespace parties::update::launcher;

namespace parties::update::launcher {
void log_line(std::string_view text) { std::cerr << "  log: " << text << "\n"; }
} // namespace parties::update::launcher

namespace {

Sha256 sha(char fill) { return *Sha256::from_hex(std::string(64, fill)); }

BuildRecord rec(const char* version, uint32_t schema, char fill) {
    BuildRecord r;
    r.id = {*Version::parse(version), sha(fill)};
    r.data_schema = schema;
    r.launcher_sha256 = sha(fill);
    r.app_sha256 = sha(char(fill + 1));
    return r;
}

const BuildRecord A = rec("1.0.0.0", 1, 'a');
const BuildRecord B = rec("1.1.0.0", 2, 'b');
constexpr DataInfo kV1{1, 1};
const Policy kPolicy{};
constexpr std::chrono::milliseconds kUnit{1000};
constexpr auto kJobCap = std::chrono::minutes(15);

SupervisorState steady() {
    SupervisorState s = initial_state(A, Channel::Stable);
    s.builds[0].verdict = Proven{};
    s.generation = 10;
    return s;
}

SupervisorState staged() {
    SupervisorState s = steady();
    s.builds.push_back(B);
    s.activation = Staged{B.id, std::nullopt};
    return s;
}

std::string describe(const std::optional<Effect>& e) {
    if (!e) return "(no executor call)";
    return std::visit(
        [](const auto& x) -> std::string {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, TakeSnapshot>) return "TakeSnapshot " + x.id.stem;
            else if constexpr (std::is_same_v<T, Commit>) return "Commit gen " + std::to_string(x.state.generation);
            else if constexpr (std::is_same_v<T, Spawn>) return "Spawn " + x.build.version.to_string();
            else if constexpr (std::is_same_v<T, TerminateApp>) return "TerminateApp";
            else if constexpr (std::is_same_v<T, RestoreSnapshot>)
                return "RestoreSnapshot " + (x.from ? x.from->stem : "nothing") + " keep " + x.preserve_live_as.stem;
            else if constexpr (std::is_same_v<T, DeleteBuild>) return "DeleteBuild " + x.build.version.to_string();
            else if constexpr (std::is_same_v<T, Redownload>) return "Redownload " + x.build.version.to_string();
            else if constexpr (std::is_same_v<T, ShowRecovery>) return "ShowRecovery";
            else return "SignalBootOk";
        },
        *e);
}

class Mock;

class MockSession final : public AppSession {
public:
    explicit MockSession(Mock& mock);
    ~MockSession() override;
    bool send(const link::LauncherMessage& message) override;
    void terminate() override;
    void activate() override;

private:
    Mock& mock_;
};

// One slot per before_effect(); the effect's executor call, if it makes one, fills it.
class Mock final : public Executor {
public:
    Clock::time_point clock = Clock::time_point{} + std::chrono::hours(1);
    std::optional<DataInfo> live = kV1;
    bool feed = true;
    bool link_up = true;
    bool worker_busy = false;
    std::optional<Download> progress;
    std::optional<size_t> fail_slot;  // the effect in this slot fails
    std::optional<SpawnFailure> spawn_failure;
    std::optional<RestoreError> restore_failure;

    std::vector<std::optional<Effect>> slots;
    std::vector<JobRequest> jobs;
    std::vector<link::LauncherMessage> sent;
    SupervisorState disk;
    int cancels = 0;
    int activations = 0;
    bool session_alive = false;
    bool stray_call = false;

    Clock::time_point now() const override { return clock; }
    void before_effect() override { slots.emplace_back(); }

    bool commit(const SupervisorState& next, const BuildId&, std::string& error) override {
        if (!carry(Commit{next}, error)) return false;
        disk = next;
        return true;
    }
    bool take_snapshot(const SnapshotId& id, const SupervisorState&, std::string& error) override {
        return carry(TakeSnapshot{id}, error);
    }
    std::expected<void, RestoreError> restore_snapshot(const std::optional<SnapshotId>& from,
                                                       const SnapshotId& preserve_live_as) override {
        std::string error;
        if (!carry(RestoreSnapshot{from, preserve_live_as}, error)) return std::unexpected(RestoreError{error, true});
        if (restore_failure) return std::unexpected(*std::exchange(restore_failure, std::nullopt));
        return {};
    }
    std::expected<std::unique_ptr<AppSession>, SpawnFailure> spawn(const BuildRecord& build) override {
        std::string error;
        if (!carry(Spawn{build.id}, error)) return std::unexpected(SpawnFailure::StartFailed);
        if (spawn_failure) return std::unexpected(*std::exchange(spawn_failure, std::nullopt));
        return std::make_unique<MockSession>(*this);
    }
    bool delete_build(const BuildId& build, const SupervisorState&, std::string& error) override {
        return carry(DeleteBuild{build}, error);
    }
    void signal_boot_ok() override {
        std::string ignored;
        carry(SignalBootOk{}, ignored);
    }
    std::expected<std::optional<DataInfo>, std::string> inspect_live_db() const override { return live; }
    std::vector<Backup> list_backups() const override { return {}; }
    bool feed_available() const override { return feed; }
    bool job_busy() const override { return worker_busy; }
    bool start_job(JobRequest request) override {
        if (auto* repair = std::get_if<RepairJob>(&request.job)) {
            std::string ignored;
            carry(Redownload{repair->build}, ignored);
        }
        jobs.push_back(std::move(request));
        worker_busy = true;
        return true;
    }
    std::optional<Download> download() const override { return progress; }
    void cancel_job() override { ++cancels; }

    void record_terminate() {
        std::string ignored;
        carry(TerminateApp{}, ignored);
    }

    template <class T>
    size_t count_sent() const {
        size_t n = 0;
        for (const auto& m : sent) n += std::holds_alternative<T>(m);
        return n;
    }

private:
    bool carry(Effect effect, std::string& error) {
        if (slots.empty() || slots.back()) {
            std::cerr << "  executor call outside an effect slot: " << describe(effect) << "\n";
            stray_call = true;
            return true;
        }
        slots.back() = std::move(effect);
        if (fail_slot && *fail_slot == slots.size() - 1) {
            error = "injected failure";
            return false;
        }
        return true;
    }
};

MockSession::MockSession(Mock& mock) : mock_(mock) { mock_.session_alive = true; }
MockSession::~MockSession() { mock_.session_alive = false; }
bool MockSession::send(const link::LauncherMessage& message) {
    if (!mock_.link_up) return false;
    mock_.sent.push_back(message);
    return true;
}
void MockSession::terminate() { mock_.record_terminate(); }
void MockSession::activate() { ++mock_.activations; }

using Fails = bool (*)(const Effect&);

// What the core must have done: step() folded over the events the stimuli
// mean, each batch cut after the effect that fails.
class Oracle {
public:
    explicit Oracle(SupervisorState state) : state_(std::move(state)) {}

    void expect(const Event& event, const Mock& mock, Fails failing = nullptr) {
        Step result = step(state_, event, Context{mock.live, kPolicy});
        for (const Effect& effect : result.effects) {
            const size_t slot = expected_.size();
            if (auto* show = std::get_if<ShowRecovery>(&effect)) recoveries_.push_back(show->why);
            expected_.push_back(std::holds_alternative<ShowRecovery>(effect) ? std::nullopt : std::optional(effect));
            const bool fails = (mock.fail_slot && *mock.fail_slot == slot) || (failing && failing(effect));
            if (auto* commit = std::get_if<Commit>(&effect); commit && !fails) state_ = commit->state;
            if (fails) break;
        }
    }

    const SupervisorState& state() const { return state_; }
    const std::vector<std::optional<Effect>>& expected() const { return expected_; }
    // ShowRecovery makes no executor call, so its reasons are kept apart.
    const std::vector<RecoveryReason>& recoveries() const { return recoveries_; }

private:
    SupervisorState state_;
    std::vector<std::optional<Effect>> expected_;
    std::vector<RecoveryReason> recoveries_;
};

class Scenario {
public:
    explicit Scenario(std::string name) : name_(std::move(name)) {}

    bool check(bool ok, const std::string& what) {
        if (!ok) {
            std::cerr << name_ << ": " << what << "\n";
            ok_ = false;
        }
        return ok;
    }

    void same_effects(const Mock& mock, const Oracle& oracle, const SupervisorCore& core) {
        const auto& got = mock.slots;
        const auto& want = oracle.expected();
        bool same = got == want;
        if (!same) {
            std::cerr << name_ << ": the executor's effects differ from step()\n";
            for (size_t i = 0; i < std::max(got.size(), want.size()); ++i)
                std::cerr << "  " << i << ": got " << (i < got.size() ? describe(got[i]) : "-") << ", step "
                          << (i < want.size() ? describe(want[i]) : "-") << "\n";
            ok_ = false;
        }
        check(!mock.stray_call, "the executor was called outside an effect");
        check(core.state() == oracle.state(), "the core's state is not the last state step() committed");
        check(mock.disk == oracle.state() || mock.disk == SupervisorState{},
              "the state on disk is not the last committed state");
    }

    bool finish() {
        std::cout << (ok_ ? "PASS " : "FAIL ") << name_ << "\n";
        return ok_;
    }

private:
    std::string name_;
    bool ok_ = true;
};

template <class T>
bool has_effect(const std::vector<std::optional<Effect>>& slots) {
    for (const auto& s : slots)
        if (s && std::holds_alternative<T>(*s)) return true;
    return false;
}

CoreConfig config() { return CoreConfig{kUnit, kPolicy, std::nullopt, std::nullopt}; }

link::Hello hello() { return link::Hello{kLinkProtocol, kMinLauncherLink, A.id.version}; }

bool start_healthy_confirm_exit() {
    Scenario t("start, healthy, confirm timers, clean exit, job cap");
    Mock mock;
    Oracle oracle(steady());
    SupervisorCore core(mock, steady(), config());

    core.start(false);
    oracle.expect(Resumed{}, mock);
    oracle.expect(StartRequested{}, mock);
    t.same_effects(mock, oracle, core);
    t.check(mock.session_alive, "StartRequested on a steady install did not start the app");
    const Clock::time_point started = mock.clock;
    t.check(core.next_deadline() == started + kUnit * kPolicy.health_deadline_s,
            "the next deadline is not the health deadline");

    core.on_link({hello()});
    t.check(mock.sent.size() == 2 && std::holds_alternative<link::Welcome>(mock.sent[0]) &&
                std::holds_alternative<link::Status>(mock.sent[1]),
            "hello was not answered with welcome, then status");
    core.on_link({hello()});
    t.check(mock.count_sent<link::Status>() == 2, "a second hello did not resend the status");

    mock.clock += std::chrono::seconds(5);
    const Clock::time_point healthy = mock.clock;
    core.on_link({link::Healthy{1}});
    oracle.expect(AppHealthy{}, mock);
    t.same_effects(mock, oracle, core);
    t.check(mock.jobs.size() == 1 && std::holds_alternative<CheckJob>(mock.jobs[0].job),
            "the first healthy report did not start a check");
    core.on_link({link::Healthy{1}});
    t.same_effects(mock, oracle, core);

    const size_t statuses = mock.count_sent<link::Status>();
    core.on_worker(std::nullopt);
    t.check(mock.count_sent<link::Status>() == statuses, "an unchanged status was sent again");
    mock.progress = Download{B.id.version, 40};
    core.on_worker(std::nullopt);
    t.check(mock.count_sent<link::Status>() == statuses + 1 &&
                std::get<link::Status>(mock.sent.back()).phase == link::Phase::Downloading &&
                std::get<link::Status>(mock.sent.back()).percent == 40,
            "download progress did not reach the app");
    mock.progress.reset();

    mock.clock = healthy + kUnit * kPolicy.confirm_on_clean_exit_after_s;
    core.on_timers();
    oracle.expect(ConfirmTimerElapsed{kPolicy.confirm_on_clean_exit_after_s}, mock);
    t.same_effects(mock, oracle, core);
    core.on_timers();
    t.same_effects(mock, oracle, core);

    mock.clock = healthy + kUnit * kPolicy.confirm_uptime_s;
    core.on_timers();
    oracle.expect(ConfirmTimerElapsed{kPolicy.confirm_uptime_s}, mock);
    t.same_effects(mock, oracle, core);

    core.on_app_exited(ExitKind::Clean);
    oracle.expect(AppExited{ExitKind::Clean}, mock);
    t.same_effects(mock, oracle, core);
    t.check(!mock.session_alive, "the exited app's session was kept");
    t.check(!core.idle(), "the launcher would leave with a check in flight");
    t.check(core.next_deadline() == mock.clock + kJobCap, "the job cap after exit is not 15 minutes");

    mock.clock += kJobCap - std::chrono::milliseconds(1);
    core.on_timers();
    t.check(mock.cancels == 0, "the check was cancelled before the cap");
    mock.clock += std::chrono::milliseconds(1);
    core.on_timers();
    t.check(mock.cancels == 1, "the check outlived the cap after the app exited");

    mock.worker_busy = false;
    core.on_worker(JobOutcome{CheckJob{}, Channel::Stable, false, {}, FeedView{}});
    t.same_effects(mock, oracle, core);
    t.check(core.idle(), "nothing is left, yet the launcher would stay");
    return t.finish();
}

bool candidate_crash_rolls_back() {
    Scenario t("a staged candidate crashes before health and good runs again");
    Mock mock;
    Oracle oracle(staged());
    SupervisorCore core(mock, staged(), config());

    core.start(false);
    oracle.expect(Resumed{}, mock);
    oracle.expect(StartRequested{}, mock);
    t.same_effects(mock, oracle, core);
    t.check(std::holds_alternative<Probation>(core.state().activation), "the staged build did not go on probation");

    core.on_app_exited(ExitKind::Abnormal);
    oracle.expect(AppExited{ExitKind::Abnormal}, mock);
    t.same_effects(mock, oracle, core);
    t.check(core.state().good == A.id && mock.session_alive, "good is not running after the candidate crashed");
    return t.finish();
}

bool hang_is_killed_at_the_deadline() {
    Scenario t("a candidate that never reports healthy is killed at the deadline");
    Mock mock;
    mock.feed = false;
    Oracle oracle(staged());
    SupervisorCore core(mock, staged(), config());

    core.start(false);
    oracle.expect(Resumed{}, mock);
    oracle.expect(StartRequested{}, mock);
    const Clock::time_point deadline = mock.clock + kUnit * kPolicy.health_deadline_s;

    mock.clock = deadline - std::chrono::milliseconds(1);
    core.on_timers();
    t.same_effects(mock, oracle, core);

    mock.clock = deadline;
    core.on_timers();
    oracle.expect(HealthDeadlinePassed{}, mock);
    oracle.expect(AppExited{ExitKind::KilledHung}, mock);
    t.same_effects(mock, oracle, core);
    t.check(has_effect<TerminateApp>(mock.slots), "the hung app was never terminated");
    t.check(core.state().good == A.id && mock.session_alive, "good is not running after the candidate hung");
    return t.finish();
}

bool a_failed_effect_ends_the_batch() {
    Scenario t("a failed commit stops the batch before the spawn");
    Mock mock;
    Oracle oracle(steady());
    SupervisorCore core(mock, steady(), config());

    Step resumed = step(steady(), Resumed{}, Context{mock.live, kPolicy});
    Step start = step(resumed.next, StartRequested{}, Context{mock.live, kPolicy});
    size_t commit_at = 0;
    while (commit_at < start.effects.size() && !std::holds_alternative<Commit>(start.effects[commit_at])) ++commit_at;
    if (!t.check(commit_at + 1 < start.effects.size() && std::holds_alternative<Spawn>(start.effects.back()),
                 "StartRequested no longer commits before it spawns"))
        return t.finish();
    mock.fail_slot = resumed.effects.size() + commit_at;

    core.start(false);
    oracle.expect(Resumed{}, mock);
    oracle.expect(StartRequested{}, mock);
    t.same_effects(mock, oracle, core);
    t.check(!mock.session_alive && !has_effect<Spawn>(mock.slots), "the app started after its commit failed");
    t.check(core.idle(), "the launcher would stay with nothing running");
    return t.finish();
}

bool corrupt_build_is_redownloaded() {
    Scenario t("a spawn that finds the build corrupt redownloads it and starts again");
    Mock mock;
    mock.spawn_failure = SpawnFailure::BuildCorrupt;
    Oracle oracle(steady());
    SupervisorCore core(mock, steady(), config());

    core.start(false);
    oracle.expect(Resumed{}, mock);
    oracle.expect(StartRequested{}, mock, [](const Effect& e) { return std::holds_alternative<Spawn>(e); });
    oracle.expect(BuildCorrupt{A.id}, mock);
    t.same_effects(mock, oracle, core);
    t.check(oracle.recoveries() == std::vector{RecoveryReason::Repairing},
            "a corrupt good build no longer shows the repairing recovery");
    t.check(!mock.session_alive, "an app runs from a corrupt build");
    t.check(mock.jobs.size() == 1 && std::holds_alternative<RepairJob>(mock.jobs[0].job),
            "the corrupt build was not queued for repair");
    t.check(!core.take_recovery(), "recovery was shown while the repair runs");

    mock.worker_busy = false;
    BuildRecord sealed = A;
    sealed.verdict = Proven{};
    core.on_worker(JobOutcome{RepairJob{A.id}, Channel::Stable, true, {BuildSealed{sealed, std::nullopt}}, FeedView{}});
    oracle.expect(BuildSealed{sealed, std::nullopt}, mock);
    oracle.expect(StartRequested{}, mock);
    t.same_effects(mock, oracle, core);
    t.check(mock.session_alive, "the repaired build did not start");
    return t.finish();
}

bool restore_that_cannot_start_falls_back_to_good() {
    Scenario t("a restore that leaves the data untouched reports it and starts good");
    SupervisorState restoring = steady();
    restoring.operation = Restoring{Snapshot{SnapshotId{"pre-1.1.0.0"}, kV1}, SnapshotId{"live-1"}};
    Mock mock;
    mock.restore_failure = RestoreError{"unreadable backup", false};
    Oracle oracle(restoring);
    SupervisorCore core(mock, restoring, config());

    core.start(false);
    oracle.expect(Resumed{}, mock, [](const Effect& e) { return std::holds_alternative<RestoreSnapshot>(e); });
    oracle.expect(RestoreFailed{}, mock);
    oracle.expect(StartRequested{}, mock);
    oracle.expect(StartRequested{}, mock);
    t.same_effects(mock, oracle, core);
    t.check(mock.session_alive, "good did not start after the restore failed");
    return t.finish();
}

bool restart_to_update_applies_the_staged_build() {
    Scenario t("restart to update asks the app to exit, then starts the staged build");
    Mock mock;
    Oracle oracle(steady());
    SupervisorCore core(mock, steady(), config());

    core.start(false);
    oracle.expect(Resumed{}, mock);
    oracle.expect(StartRequested{}, mock);
    core.on_link({hello(), link::Healthy{1}});
    oracle.expect(AppHealthy{}, mock);

    core.on_link({link::Request{link::RestartToUpdate{}}});
    t.check(mock.count_sent<link::ExitRequested>() == 0, "restart to update ran with nothing staged");

    mock.worker_busy = false;
    core.on_worker(JobOutcome{CheckJob{}, Channel::Stable, true, {BuildSealed{B, std::nullopt}}, FeedView{}});
    oracle.expect(BuildSealed{B, std::nullopt}, mock);
    t.same_effects(mock, oracle, core);
    t.check(std::holds_alternative<Staged>(core.state().activation), "the sealed build was not staged");
    t.check(std::get<link::Status>(mock.sent.back()).phase == link::Phase::ReadyToRestart,
            "the app was not told a restart is ready");

    core.on_link({link::Request{link::RestartToUpdate{}}});
    t.check(mock.count_sent<link::ExitRequested>() == 1, "restart to update did not ask the app to exit");
    core.on_app_exited(ExitKind::Clean);
    oracle.expect(AppExited{ExitKind::Clean}, mock);
    oracle.expect(StartRequested{}, mock);
    t.same_effects(mock, oracle, core);
    t.check(std::holds_alternative<Probation>(core.state().activation) && mock.session_alive,
            "the staged build did not start on probation");
    return t.finish();
}

bool quit_cancels_jobs_and_never_restarts() {
    Scenario t("quit cancels jobs, asks the app to exit once, and starts nothing after it");
    Mock mock;
    Oracle oracle(steady());
    SupervisorCore core(mock, steady(), config());

    core.start(false);
    oracle.expect(Resumed{}, mock);
    oracle.expect(StartRequested{}, mock);
    mock.link_up = false;
    core.on_quit();
    t.check(mock.cancels == 1, "quit did not cancel the feed job");
    mock.link_up = true;
    core.on_link({hello()});
    t.check(mock.count_sent<link::ExitRequested>() == 1, "the app that said hello during quit was not asked to exit");
    core.on_quit();
    t.check(mock.count_sent<link::ExitRequested>() == 1, "exit was requested twice");
    core.on_activate();
    t.check(mock.activations == 0, "a quitting launcher brought the app forward");

    core.on_app_exited(ExitKind::Clean);
    oracle.expect(AppExited{ExitKind::Clean}, mock);
    t.same_effects(mock, oracle, core);
    t.check(!mock.session_alive && core.idle(), "something runs after quit");
    return t.finish();
}

bool activate_brings_forward_or_starts() {
    Scenario t("starting Parties again brings the app forward, or starts it");
    Mock mock;
    mock.feed = false;
    Oracle oracle(steady());
    SupervisorCore core(mock, steady(), config());

    core.start(false);
    oracle.expect(Resumed{}, mock);
    oracle.expect(StartRequested{}, mock);
    core.on_link({hello(), link::Healthy{1}});
    oracle.expect(AppHealthy{}, mock);
    core.on_activate();
    t.check(mock.activations == 1, "the running app was not brought forward");
    t.same_effects(mock, oracle, core);

    core.on_app_exited(ExitKind::Clean);
    oracle.expect(AppExited{ExitKind::Clean}, mock);
    core.on_activate();
    oracle.expect(StartRequested{}, mock);
    t.same_effects(mock, oracle, core);
    t.check(mock.session_alive, "activate with no app did not start it");
    return t.finish();
}

bool recovery_waits_then_maps_choices() {
    Scenario t("recovery waits for the app to exit, shows once, and a choice becomes its events");
    Mock mock;
    mock.feed = false;
    Oracle oracle(steady());
    SupervisorCore core(mock, steady(), config());

    core.start(true);
    oracle.expect(Resumed{}, mock);
    core.on_activate();
    oracle.expect(StartRequested{}, mock);
    t.same_effects(mock, oracle, core);
    t.check(!core.take_recovery(), "recovery was offered while the app runs");
    core.on_link({hello(), link::Healthy{1}});
    oracle.expect(AppHealthy{}, mock);
    core.on_app_exited(ExitKind::Clean);
    oracle.expect(AppExited{ExitKind::Clean}, mock);
    t.same_effects(mock, oracle, core);

    auto due = core.take_recovery();
    t.check(due && !due->why && due->live_db == kV1, "--recover did not offer recovery with the live data");
    t.check(!core.take_recovery(), "recovery was offered twice");

    core.on_recovery_choice(RetryBuild{A.id});
    oracle.expect(UserRetry{A.id}, mock);
    oracle.expect(StartRequested{}, mock);
    t.same_effects(mock, oracle, core);

    t.check(events_for(StartAgain{}) == std::vector<Event>{StartRequested{}}, "start again");
    t.check(events_for(StartBuild{B.id, std::nullopt}) == std::vector<Event>{UserRollback{B.id, std::nullopt}},
            "start another build");
    Snapshot backup{SnapshotId{"b"}, kV1};
    t.check(events_for(RestoreBackup{backup}) == std::vector<Event>{UserRestore{backup}, StartRequested{}},
            "restore a backup");
    t.check(events_for(Quit{}).empty(), "quit");
    return t.finish();
}

} // namespace

int main() {
    bool ok = true;
    ok &= start_healthy_confirm_exit();
    ok &= candidate_crash_rolls_back();
    ok &= hang_is_killed_at_the_deadline();
    ok &= a_failed_effect_ends_the_batch();
    ok &= corrupt_build_is_redownloaded();
    ok &= restore_that_cannot_start_falls_back_to_good();
    ok &= restart_to_update_applies_the_staged_build();
    ok &= quit_cancels_jobs_and_never_restarts();
    ok &= activate_brings_forward_or_starts();
    ok &= recovery_waits_then_maps_choices();
    std::cout << (ok ? "update_supervisor_core_test: all scenarios pass\n" : "update_supervisor_core_test: FAILED\n");
    return ok ? 0 : 1;
}
