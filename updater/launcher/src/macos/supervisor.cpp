#include "supervisor.h"

#include "build_store.h"
#include "data_guard.h"
#include "file_io.h"
#include "front_door.h"
#include "log.h"
#include "recovery_choice.h"
#include "state_store.h"
#include "supervisor_core.h"
#include "test_hooks.h"
#include "update_worker.h"

#include <parties/data_lock.h>
#include <parties/front_door_socket.h>
#include <parties/host_api.h>

#include <fcntl.h>
#include <signal.h>
#include <sys/event.h>
#include <sys/file.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <memory>
#include <thread>

namespace parties::update::launcher::macos {

namespace {

constexpr uintptr_t kWorkerWake = 1;
constexpr Runner kRunner{.launcher_protocol = kLinkProtocol, .host_abi = PARTIES_HOST_ABI};

template <class... F>
struct overloaded : F... {
    using F::operator()...;
};

DataFiles data_files(const fs::path& root) { return {root / "parties.db", root / "backups"}; }

std::unique_ptr<UpdateSource> make_source(const LauncherOptions& o) {
    // The https feed arrives with PR 8; until then only a harness feeds builds.
    if (testing::enabled() && o.feed_dir) return make_local_dir_source(*o.feed_dir);
    return nullptr;
}

// The Unix event loop around SupervisorCore, and its Executor: fronts through
// the front door, data.lock around every touch of the live database, the
// feed on a worker thread, and SIGTERM for the end of the user's session.
class Supervisor final : Executor {
public:
    explicit Supervisor(const LauncherOptions& o)
        : options_(o), store_(o.install, o.data_root.string()), builds_(o.install), data_(data_files(o.data_root)),
          worker_(make_source(o),
                  WorkerConfig{kMacClient, kRunner, testing::pinned_keys(), o.data_root / "update" / "trust.json",
                               [this](const BuildEntry& entry, UpdateSource& source, const Progress& progress,
                                      const std::atomic<bool>& cancel) {
                                   return builds_.seal(entry, FeedFiles{source, progress, cancel});
                               },
                               [this] { wake_loop(); }}),
          boot_ok_fd_(o.boot_ok_fd) {}
    ~Supervisor() {
        worker_.stop();
        if (kq_ >= 0) close(kq_);
    }

    int run();

private:
    // The app session SupervisorCore holds. Its front may not have connected
    // yet: the front door assigns the build when one does.
    struct AppRun final : AppSession {
        explicit AppRun(Supervisor& s) : owner(s) { owner.app_ = this; }
        ~AppRun() override {
            if (owner.app_ == this) owner.app_ = nullptr;
        }

        bool send(const link::LauncherMessage& message) override { return owner.door_->send_link(message); }
        void terminate() override { owner.terminate_app(); }
        void activate() override { owner.door_->activate_app(); }

        Supervisor& owner;
        bool corrupt = false;  // its front reported the build's files corrupt
    };

    Clock::time_point now() const override { return Clock::now(); }
    void before_effect() override { testing::before_effect(); }
    bool commit(const SupervisorState& next, const BuildId&, std::string& error) override {
        return store_.commit(next, error);
    }
    bool take_snapshot(const SnapshotId& id, const SupervisorState& committed, std::string& error) override;
    std::expected<void, RestoreError> restore_snapshot(const std::optional<SnapshotId>& from,
                                                       const SnapshotId& preserve_live_as) override;
    std::expected<std::unique_ptr<AppSession>, SpawnFailure> spawn(const BuildRecord& build) override;
    bool delete_build(const BuildId& build, const SupervisorState& committed, std::string& error) override {
        return builds_.remove(build, committed, error);
    }
    void signal_boot_ok() override;
    std::expected<std::optional<DataInfo>, LiveDbError> inspect_live_db() const override;
    std::vector<Backup> list_backups() const override { return data_.list(); }
    bool feed_available() const override { return worker_.available(); }
    bool job_busy() const override { return worker_.busy(); }
    bool start_job(JobRequest request) override { return worker_.start(std::move(request)); }
    std::optional<Download> download() const override { return worker_.download(); }
    void cancel_job() override { worker_.cancel(); }

    std::optional<DataLock> lock_data() const { return DataLock::try_acquire(options_.data_root); }
    std::optional<SupervisorState> load_state();
    std::optional<SupervisorState> adopt_own_build();
    void terminate_app();
    void on_door(const Event& event);
    void drain_door();
    void show_recovery(RecoveryDue due);
    void wake_loop();
    void write_session(bool clean);
    void end_session();

    const LauncherOptions& options_;
    StateStore store_;
    BuildStore builds_;
    DataGuard data_;
    int kq_ = kqueue();
    UpdateWorker worker_;
    std::unique_ptr<FrontDoor> door_;

    AppRun* app_ = nullptr;
    // A front SupervisorCore already let go of (TerminateApp) that the front
    // door has yet to see die. What it still reports belongs to no session.
    std::optional<int32_t> retired_;
    std::optional<SupervisorCore> core_;
    int boot_ok_fd_;
    bool quit_ = false;
    bool session_ended_ = false;
    Sha256 self_sha_;
};

constexpr const char* kDataBusy = "data.lock is held: an app process still has the data open";

bool Supervisor::take_snapshot(const SnapshotId& id, const SupervisorState& committed, std::string& error) {
    auto lock = lock_data();
    if (!lock) return (error = kDataBusy, false);
    if (!data_.snapshot(id, error)) return false;
    data_.prune(committed);
    return true;
}

std::expected<void, RestoreError> Supervisor::restore_snapshot(const std::optional<SnapshotId>& from,
                                                               const SnapshotId& preserve_live_as) {
    auto lock = lock_data();
    if (!lock) return std::unexpected(RestoreError{kDataBusy, false, true});
    return data_.restore(from, preserve_live_as);
}

std::expected<std::optional<DataInfo>, LiveDbError> Supervisor::inspect_live_db() const {
    auto lock = lock_data();
    if (!lock) return std::unexpected(LiveDbError{kDataBusy, true});
    auto info = data_.inspect();
    if (!info) return std::unexpected(LiveDbError{info.error()});
    return *info;
}

// The front checks the build's digest itself when it loads it, and reports
// corrupt instead of running it, so a damaged build surfaces here as an app
// that ends with corrupt{build}.
std::expected<std::unique_ptr<AppSession>, SpawnFailure> Supervisor::spawn(const BuildRecord& build) {
    const fs::path data = options_.data_root;
    log("starting {}", build.id.dir_name());
    door_->deliver({build_dir(build.id), build.app_sha256, data.string(), (data / "backups").string()});
    return std::make_unique<AppRun>(*this);
}

// "Kills the app and returns once it is gone": until the kernel has reaped the
// front, it may still hold data.lock.
void Supervisor::terminate_app() {
    const std::optional<int32_t> pid = door_->app_pid();
    if (!pid) {
        log("no front connected before the health deadline");
        door_->withdraw();
        return;
    }
    if (!door_->kill_app()) return;
    retired_ = pid;
    for (int i = 0; i < 500 && kill(*pid, 0) == 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
}

void Supervisor::signal_boot_ok() {
    if (boot_ok_fd_ < 0) return;
    const char byte = 1;
    // The booting front may be gone; then nobody waits for this.
    if (write(boot_ok_fd_, &byte, 1) != 1) log("boot_ok was not delivered: {}", std::strerror(errno));
    close(boot_ok_fd_);
    boot_ok_fd_ = -1;
}

void Supervisor::wake_loop() {
    struct kevent change;
    EV_SET(&change, kWorkerWake, EVFILT_USER, 0, NOTE_TRIGGER, 0, nullptr);
    kevent(kq_, &change, 1, nullptr, 0, nullptr);
}

void Supervisor::write_session(bool clean) {
    std::string error;
    if (!store_.write_session({options_.boot_session, self_sha_, clean}, error)) log("{}", error);
}

std::optional<SupervisorState> Supervisor::adopt_own_build() {
    auto record = builds_.adopt(options_.build_dir);
    if (!record) {
        log("Parties is not installed correctly: {}", record.error());
        return std::nullopt;
    }
    SupervisorState initial = adopted_state(*record);
    std::string error;
    if (!store_.commit(initial, error)) {
        log("Parties cannot save its update state: {}", error);
        return std::nullopt;
    }
    log("adopted {} as the good build", record->id.dir_name());
    return initial;
}

std::optional<SupervisorState> Supervisor::load_state() {
    LoadResult loaded = store_.load();
    if (auto* s = std::get_if<SupervisorState>(&loaded)) return *s;
    if (std::holds_alternative<NewerLauncherState>(loaded)) {
        log("supervisor.json was written by a newer launcher; leaving it untouched");
        return std::nullopt;
    }
    if (std::holds_alternative<UnreadableState>(loaded))
        log("supervisor.json and its .prev are unreadable; rebuilding from the running build");
    return adopt_own_build();
}

void Supervisor::on_door(const Event& event) {
    auto retired = [&](int32_t pid) { return retired_ && *retired_ == pid; };
    std::visit(overloaded{
                   [&](const FrontWaiting& e) {
                       log("front {} is waiting{}", e.pid, e.recover ? " (asked for recovery)" : "");
                       core_->on_activate();
                   },
                   [&](const FrontLoaded& e) {
                       if (!retired(e.pid)) log("app {} loaded {}", e.pid, e.build);
                   },
                   [&](const FrontCorrupt& e) {
                       log("app {} found {} corrupt", e.pid, e.build);
                       if (app_ && !retired(e.pid)) app_->corrupt = true;
                   },
                   [&](const LinkLine& e) {
                       if (retired(e.pid) || !app_) return;
                       if (auto message = link::parse_app_message(e.line)) core_->on_link({*message});
                   },
                   [&](const FrontExited& e) {
                       if (retired(e.pid)) {
                           retired_.reset();
                           return;
                       }
                       if (!e.was_app || !app_) return;
                       log("app exited: {}", exit_kind_name(e.kind));
                       if (app_->corrupt) core_->on_app_corrupt();
                       else core_->on_app_exited(e.kind);
                   },
                   [&](const QuitForInstallSent&) {
                       log("a newer seed is waiting to be installed; leaving");
                       core_->on_quit();
                   },
               },
               event);
}

void Supervisor::drain_door() {
    while (auto event = door_->next(std::chrono::milliseconds(0))) on_door(*event);
}

void Supervisor::show_recovery(RecoveryDue due) {
    log("recovery shown: {}", recovery_reason_name(due.why));
    RecoveryChoice choice = Quit{};
    if (auto answer = testing::scripted_recovery()) {
        signal_boot_ok();
        choice = scripted_choice(core_->state(), due.live_db, due.backups, *answer);
        log("recovery answered by script: {}", *answer);
    } else {
        // The host's recovery alert arrives with the boot role.
        log("recovery needs the host's alert, which this build cannot show yet; leaving");
    }
    if (std::holds_alternative<Quit>(choice)) {
        quit_ = true;
        return;
    }
    core_->on_recovery_choice(choice);
}

// Logout, restart or shutdown. The front ends with the session too, so its
// end is recorded if it comes within 5 s, and this session is marked clean.
void Supervisor::end_session() {
    if (session_ended_) return;
    log("session is ending");
    door_->session_ending();
    const auto until = Clock::now() + std::chrono::seconds(5);
    while (app_ && Clock::now() < until) {
        auto left = std::chrono::ceil<std::chrono::milliseconds>(until - Clock::now());
        if (auto event = door_->next(left)) on_door(*event);
    }
    core_->cancel_jobs();
    write_session(true);
    session_ended_ = true;
    quit_ = true;
}

int Supervisor::run() {
    if (!take_launcher_lock(options_.install, std::chrono::milliseconds(0))) {
        // A booting front connects to the launcher that holds it.
        log("another launcher is running");
        signal_boot_ok();
        return 0;
    }
    if (auto digest = digest_file(options_.build_dir / kMacClient.launcher_file)) self_sha_ = digest->sha256;
    write_session(false);

    std::vector<std::string> front_arguments;
    if (testing::enabled()) {
        front_arguments.push_back("--install-root=" + options_.install.root.string());
        if (!options_.test_namespace.empty()) front_arguments.push_back("--test-namespace=" + options_.test_namespace);
        if (options_.log_dir) front_arguments.push_back("--log-dir=" + options_.log_dir->string());
    }
    door_ = FrontDoor::listen({front_door::socket_path(options_.test_namespace), options_.host_bundle.string(),
                               std::move(front_arguments)});
    if (!door_) {
        log("cannot open the front door");
        return 1;
    }

    std::error_code ec;
    fs::create_directories(options_.data_root, ec);
    std::optional<SupervisorState> state = load_state();
    if (!state) return 1;
    builds_.collect_garbage(*state, options_.build_dir);
    data_.prune(*state);

    struct kevent changes[3];
    EV_SET(&changes[0], uintptr_t(door_->fd()), EVFILT_READ, EV_ADD, 0, 0, nullptr);
    EV_SET(&changes[1], kWorkerWake, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, nullptr);
    signal(SIGTERM, SIG_IGN);
    EV_SET(&changes[2], SIGTERM, EVFILT_SIGNAL, EV_ADD, 0, 0, nullptr);
    if (kq_ < 0 || kevent(kq_, changes, 3, nullptr, 0, nullptr) != 0) {
        log("cannot set up the event loop: {}", std::strerror(errno));
        return 1;
    }

    const testing::Timing timing = testing::timing();
    core_.emplace(static_cast<Executor&>(*this), std::move(*state),
                  CoreConfig{timing.unit, timing.policy, testing::exit_after_healthy(), testing::job_cap()});
    core_->start(options_.recover);

    while (!quit_) {
        if (auto due = core_->take_recovery()) {
            show_recovery(std::move(*due));
            continue;
        }
        if (core_->idle()) break;

        timespec until{};
        const timespec* timeout = nullptr;
        if (auto deadline = core_->next_deadline()) {
            auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(*deadline - Clock::now()).count();
            ns = std::clamp<long long>(ns, 0, 24ll * 3600 * 1'000'000'000);
            until = {time_t(ns / 1'000'000'000), long(ns % 1'000'000'000)};
            timeout = &until;
        }
        struct kevent events[8];
        int n = kevent(kq_, nullptr, 0, events, 8, timeout);
        if (n < 0 && errno != EINTR) {
            log("kevent: {}", std::strerror(errno));
            break;
        }
        for (int i = 0; i < n && !quit_; ++i) {
            if (events[i].filter == EVFILT_READ) drain_door();
            else if (events[i].filter == EVFILT_USER) core_->on_worker(worker_.take());
            else if (events[i].filter == EVFILT_SIGNAL) end_session();
        }
        core_->on_timers();
    }

    worker_.stop();
    if (!session_ended_) write_session(true);
    log("launcher exiting");
    return 0;
}

} // namespace

bool take_launcher_lock(const InstallPaths& install, std::chrono::milliseconds wait) {
    std::error_code ec;
    fs::create_directories(install.state(), ec);
    int fd = open((install.state() / "launcher.lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    const auto until = std::chrono::steady_clock::now() + wait;
    while (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        if (std::chrono::steady_clock::now() >= until) {
            close(fd);
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return true;
}

int run_launcher(const LauncherOptions& options) {
    log("launcher starting: install root {}, data root {}, session {}", options.install.root.string(),
        options.data_root.string(), options.boot_session);
    Supervisor supervisor(options);
    return supervisor.run();
}

} // namespace parties::update::launcher::macos
