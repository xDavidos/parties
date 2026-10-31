#include "supervisor.h"

#include "app_process.h"
#include "build_store.h"
#include "data_guard.h"
#include "file_io.h"
#include "link_server.h"
#include "log.h"
#include "recovery_ui.h"
#include "state_store.h"
#include "testing.h"
#include "update_worker.h"

#include <parties/update/link.h>
#include <parties/update/probation.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <deque>
#include <memory>

#include <windows.h>
#include <bcrypt.h>
#include <shlobj.h>

namespace parties::update::launcher {

namespace {

using Clock = std::chrono::steady_clock;

constexpr auto kCheckInterval = std::chrono::hours(6);

std::wstring widen(std::string_view s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

fs::path known_folder(REFKNOWNFOLDERID id) {
    PWSTR path = nullptr;
    fs::path out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &path))) out = path;
    CoTaskMemFree(path);
    return out;
}

fs::path self_exe() {
    std::wstring buffer(32768, L'\0');
    DWORD n = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    buffer.resize(n);
    return fs::path(buffer);
}

std::string random_hex(size_t bytes) {
    std::vector<unsigned char> raw(bytes);
    BCryptGenRandom(nullptr, raw.data(), static_cast<ULONG>(raw.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    constexpr char digits[] = "0123456789abcdef";
    std::string out;
    for (unsigned char b : raw) {
        out.push_back(digits[b >> 4]);
        out.push_back(digits[b & 15]);
    }
    return out;
}

// A dialog would block an unattended harness forever.
void tell_user(const std::string& text) {
    log("{}", text);
    if (!testing::enabled()) MessageBoxW(nullptr, widen(text).c_str(), L"Parties", MB_OK | MB_ICONERROR);
}

link::StatusNotice wire_notice(const Notice& n) {
    link::StatusNotice out;
    out.kind = n.kind == Notice::Kind::RolledBack ? link::StatusNotice::Kind::RolledBack
                                                  : link::StatusNotice::Kind::DataRestored;
    out.from = n.from;
    out.to = n.to;
    if (n.preserved) out.preserved = "backups\\" + n.preserved->stem + ".db";
    return out;
}

struct PendingRecovery {
    std::optional<RecoveryReason> why;
};

std::unique_ptr<UpdateSource> make_source(const LauncherOptions& o) {
    if (o.feed_dir) return make_local_dir_source(*o.feed_dir);
    if (o.feed_base) return make_https_source(*o.feed_base, true);
    // A testing launcher talks to no real feed unless told to, and a build
    // that pins no key could verify nothing it fetched.
    if (testing::enabled() || testing::pinned_keys().empty()) return nullptr;
    return make_https_source(std::string(kFeedBase), false);
}

class Supervisor {
public:
    explicit Supervisor(const LauncherOptions& o)
        : options_(o), store_(o.install, utf8(o.data.data())), builds_(o.install), data_(o.data),
          worker_(make_source(o), testing::pinned_keys(), o.data.trust(), BuildStore(o.install)),
          timing_(testing::timing()) {}

    int run();

private:
    struct AppRun {
        BuildId build;
        AppProcess process;
        std::unique_ptr<LinkServer> link;
        Clock::time_point started;
        std::optional<Clock::time_point> healthy_at;
        bool welcomed = false;
        bool deadline_fired = false;
        bool clean_mark_fired = false;
        bool confirm_mark_fired = false;
        bool exit_sent = false;
        std::optional<link::Status> last_sent;
    };

    bool load_state();
    bool adopt_own_build();
    void dispatch(Event event);
    bool execute(const Effect& effect);
    bool spawn(const BuildId& build);
    void on_app_exit();
    void on_link();
    void on_request(const link::Request& request);
    void on_activate();
    void on_worker();
    void on_timers();
    std::optional<Clock::time_point> next_deadline() const;
    void start_check();
    void submit(Job job);
    void pump();
    bool repairing() const;
    void show_pending_recovery();
    void publish_status();
    link::Status status() const;
    std::optional<DataInfo> live_db();
    void signal_boot_ok();
    void write_session(bool clean);
    void end_session();
    Clock::duration units(uint32_t seconds) const { return timing_.unit * seconds; }
    std::wstring object_name(const wchar_t* suffix) const { return L"Local\\" + options_.object_prefix + suffix; }

    static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    const LauncherOptions& options_;
    StateStore store_;
    BuildStore builds_;
    DataGuard data_;
    UpdateWorker worker_;
    AppJob job_;
    testing::Timing timing_;

    SupervisorState state_;  // the last state that reached disk
    std::deque<Event> queue_;
    bool draining_ = false;
    std::optional<AppRun> app_;
    std::optional<PendingRecovery> recovery_;
    std::optional<DataInfo> last_live_db_;
    FeedView feed_view_;
    bool restart_requested_ = false;
    bool quit_ = false;
    bool session_ended_ = false;
    Clock::time_point next_check_ = Clock::time_point::max();
    std::deque<Job> jobs_;
    std::optional<Job> running_job_;

    fs::path self_exe_;
    Sha256 self_sha_;
};

bool Supervisor::adopt_own_build() {
    auto record = builds_.adopt(self_exe_.parent_path());
    if (!record) {
        tell_user("Parties is not installed correctly: " + record.error());
        return false;
    }
    SupervisorState initial = initial_state(*record, Channel::Stable);
    initial.generation = 1;
    std::string error;
    if (!store_.commit(initial, error)) {
        tell_user("Parties cannot save its update state: " + error);
        return false;
    }
    state_ = initial;
    log("adopted {} as the good build", record->id.dir_name());
    return true;
}

bool Supervisor::load_state() {
    LoadResult loaded = store_.load();
    if (auto* s = std::get_if<SupervisorState>(&loaded)) {
        state_ = *s;
        return true;
    }
    if (std::holds_alternative<NewerLauncherState>(loaded)) {
        // The boot stub falls back to its own safe mode when we exit without
        // boot_ok, so leaving now is the safe answer.
        log("supervisor.json was written by a newer launcher; leaving it untouched");
        return false;
    }
    if (std::holds_alternative<UnreadableState>(loaded))
        log("supervisor.json and its .prev are unreadable; rebuilding from the running build");
    return adopt_own_build();
}

std::optional<DataInfo> Supervisor::live_db() {
    if (app_) return last_live_db_;
    auto info = data_.inspect();
    if (!info) {
        log("cannot read the live database: {}", info.error());
        return last_live_db_ = std::nullopt;
    }
    return last_live_db_ = *info;
}

void Supervisor::dispatch(Event event) {
    queue_.push_back(std::move(event));
    if (draining_) return;
    draining_ = true;
    while (!queue_.empty()) {
        Event next = std::move(queue_.front());
        queue_.pop_front();
        Context ctx{live_db(), timing_.policy};
        Step result = step(state_, next, ctx);
        for (const Effect& effect : result.effects) {
            testing::before_effect();
            if (!execute(effect)) break;
        }
    }
    draining_ = false;
    publish_status();
}

bool Supervisor::execute(const Effect& effect) {
    std::string error;
    bool ok = std::visit(
        [&](const auto& e) -> bool {
            using T = std::decay_t<decltype(e)>;
            if constexpr (std::is_same_v<T, TakeSnapshot>) {
                if (app_) return (error = "the app is running", false);
                if (!data_.snapshot(e.id, error)) return false;
                data_.prune(state_);
                return true;
            } else if constexpr (std::is_same_v<T, Commit>) {
                if (!store_.commit(e.state, error)) return false;
                state_ = e.state;
                return true;
            } else if constexpr (std::is_same_v<T, Spawn>) {
                return spawn(e.build);
            } else if constexpr (std::is_same_v<T, TerminateApp>) {
                if (!app_) return true;
                app_->process.terminate();
                log("killed {} after it missed the health deadline", app_->build.dir_name());
                app_.reset();
                queue_.push_back(AppExited{ExitKind::KilledHung});
                return true;
            } else if constexpr (std::is_same_v<T, RestoreSnapshot>) {
                if (app_) return (error = "the app is running", false);
                return data_.restore(e.from, e.preserve_live_as, error);
            } else if constexpr (std::is_same_v<T, DeleteBuild>) {
                // A leftover directory is untracked, so the next start collects it.
                if (!builds_.remove(e.build, state_, error)) log("cannot delete {}: {}", e.build.dir_name(), error);
                return true;
            } else if constexpr (std::is_same_v<T, Redownload>) {
                if (!worker_.available()) log("{} needs to be downloaded again, but there is no update feed", e.build.dir_name());
                else submit(RepairJob{e.build});
                return true;
            } else if constexpr (std::is_same_v<T, ShowRecovery>) {
                recovery_ = PendingRecovery{e.why};
                return true;
            } else {
                signal_boot_ok();
                return true;
            }
        },
        effect);
    if (!ok && !error.empty()) log("effect failed: {}", error);
    return ok;
}

bool Supervisor::spawn(const BuildId& build) {
    const BuildRecord* record = state_.find(build);
    std::optional<ExecLease> lease = record ? builds_.lease_app(*record) : std::nullopt;
    if (!lease) {
        queue_.push_back(BuildCorrupt{build});
        return false;
    }
    std::wstring pipe = L"\\\\.\\pipe\\parties-link-" + std::to_wstring(options_.boot_session) + L"-" +
                        widen(random_hex(16));
    auto link = LinkServer::create(pipe);
    if (!link) {
        log("{}", link.error());
        queue_.push_back(AppExited{ExitKind::Abnormal});
        return false;
    }
    std::vector<std::wstring> args{L"--data-dir=" + options_.data.data().wstring(),
                                   L"--backups-dir=" + options_.data.backups().wstring(),
                                   L"--supervisor-pipe=" + pipe};
    args.insert(args.end(), options_.forwarded.begin(), options_.forwarded.end());
    auto process = AppProcess::spawn(job_, std::move(*lease), args, options_.data.data());
    if (!process) {
        log("{}", process.error());
        queue_.push_back(AppExited{ExitKind::Abnormal});
        return false;
    }
    (*link)->expect_client(process->pid());
    app_.emplace(AppRun{build, std::move(*process), std::move(*link), Clock::now()});
    return true;
}

void Supervisor::on_app_exit() {
    on_link();
    if (!app_) return;
    ExitKind kind = app_->process.exit_kind();
    app_.reset();
    dispatch(AppExited{kind});
    if (restart_requested_ && !app_) {
        restart_requested_ = false;
        dispatch(StartRequested{});
    }
    if (app_) return;
    // Nobody is left to see a check or a downgrade; a repair is what lets the
    // next start succeed, so it goes on.
    std::erase_if(jobs_, [](const Job& j) { return !std::holds_alternative<RepairJob>(j); });
    if (running_job_ && !std::holds_alternative<RepairJob>(*running_job_)) worker_.cancel();
}

void Supervisor::on_link() {
    if (!app_) return;
    for (link::AppMessage& message : app_->link->poll()) {
        if (!app_) return;
        if (auto* hello = std::get_if<link::Hello>(&message)) {
            auto protocol = link::negotiate(*hello, kLinkProtocol);
            if (!protocol) {
                log("app {} cannot talk link protocol {}", hello->version.to_string(), kLinkProtocol);
                continue;
            }
            app_->link->send(link::Welcome{*protocol});
            app_->welcomed = true;
            app_->last_sent.reset();
            publish_status();
        } else if (auto* healthy = std::get_if<link::Healthy>(&message)) {
            if (app_->healthy_at) continue;
            log("app reported healthy (data schema {})", healthy->data_schema);
            app_->healthy_at = Clock::now();
            dispatch(AppHealthy{});
            // The first check of a launcher run waits for the app, so it never
            // competes with startup.
            if (next_check_ == Clock::time_point::max()) start_check();
        } else if (auto* request = std::get_if<link::Request>(&message)) {
            on_request(*request);
        }
    }
}

void Supervisor::on_request(const link::Request& request) {
    std::visit(
        [&](const auto& r) {
            using T = std::decay_t<decltype(r)>;
            if constexpr (std::is_same_v<T, link::RestartToUpdate>) {
                if (!std::holds_alternative<Staged>(state_.activation) || !app_) return;
                log("restarting into the staged build");
                restart_requested_ = true;
                app_->exit_sent = app_->link->send(link::ExitRequested{});
            } else if constexpr (std::is_same_v<T, link::CheckNow>) {
                start_check();
            } else if constexpr (std::is_same_v<T, link::AcknowledgeNotice>) {
                dispatch(NoticeAcknowledged{});
            } else {
                if (r.choice == link::ChannelChoice::DowngradeNow) {
                    submit(DowngradeJob{r.channel});
                    return;
                }
                ChannelIntent intent = r.choice == link::ChannelChoice::Wait
                                           ? ChannelIntent{PinnedUntilAtLeast{state_.good.version}}
                                           : ChannelIntent{Follow{}};
                dispatch(ChannelIntentSet{r.channel, intent});
                start_check();
            }
        },
        request);
}

void Supervisor::on_activate() {
    if (app_) {
        HANDLE e = OpenEventW(EVENT_MODIFY_STATE, FALSE, object_name(L".Client.Activate").c_str());
        if (e) {
            SetEvent(e);
            CloseHandle(e);
        }
        return;
    }
    dispatch(StartRequested{});
}

void Supervisor::on_worker() {
    auto outcome = worker_.take();
    if (!outcome) {
        publish_status();
        return;
    }
    running_job_.reset();
    if (std::holds_alternative<CheckJob>(outcome->job) && outcome->channel != state_.channel) {
        log("dropping a check of the channel the user just left");
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

void Supervisor::start_check() {
    next_check_ = Clock::now() + kCheckInterval;
    submit(CheckJob{});
}

void Supervisor::submit(Job job) {
    if (!worker_.available()) return;
    bool duplicate = std::ranges::find(jobs_, job) != jobs_.end() ||
                     (std::holds_alternative<CheckJob>(job) && running_job_ == job);
    if (!duplicate) jobs_.push_back(std::move(job));
    pump();
}

void Supervisor::pump() {
    if (running_job_ || jobs_.empty() || worker_.busy()) return;
    Job job = jobs_.front();
    std::vector<Backup> backups = std::holds_alternative<RepairJob>(job) ? std::vector<Backup>{} : data_.list();
    if (!worker_.start(JobRequest{job, state_, last_live_db_, std::move(backups)})) return;
    jobs_.pop_front();
    running_job_ = std::move(job);
    publish_status();
}

bool Supervisor::repairing() const {
    auto repair = [](const Job& j) { return std::holds_alternative<RepairJob>(j); };
    return (running_job_ && repair(*running_job_)) || std::ranges::any_of(jobs_, repair);
}

std::optional<Clock::time_point> Supervisor::next_deadline() const {
    std::optional<Clock::time_point> next;
    auto consider = [&](Clock::time_point t) {
        if (!next || t < *next) next = t;
    };
    if (worker_.available()) consider(next_check_);
    if (!app_) return next;
    if (!app_->healthy_at) {
        if (!app_->deadline_fired) consider(app_->started + units(timing_.policy.health_deadline_s));
        return next;
    }
    if (!app_->clean_mark_fired) consider(*app_->healthy_at + units(timing_.policy.confirm_on_clean_exit_after_s));
    if (!app_->confirm_mark_fired) consider(*app_->healthy_at + units(timing_.policy.confirm_uptime_s));
    if (auto exit_after = testing::exit_after_healthy(); exit_after && !app_->exit_sent)
        consider(*app_->healthy_at + *exit_after);
    return next;
}

void Supervisor::on_timers() {
    const auto now = Clock::now();
    if (worker_.available() && now >= next_check_) start_check();
    if (!app_) return;
    if (!app_->healthy_at) {
        if (!app_->deadline_fired && now >= app_->started + units(timing_.policy.health_deadline_s)) {
            app_->deadline_fired = true;
            dispatch(HealthDeadlinePassed{});
        }
        return;
    }
    const Clock::time_point healthy = *app_->healthy_at;
    if (!app_->clean_mark_fired && now >= healthy + units(timing_.policy.confirm_on_clean_exit_after_s)) {
        app_->clean_mark_fired = true;
        dispatch(ConfirmTimerElapsed{timing_.policy.confirm_on_clean_exit_after_s});
        if (!app_) return;
    }
    if (!app_->confirm_mark_fired && now >= healthy + units(timing_.policy.confirm_uptime_s)) {
        app_->confirm_mark_fired = true;
        dispatch(ConfirmTimerElapsed{timing_.policy.confirm_uptime_s});
        if (!app_) return;
    }
    if (auto exit_after = testing::exit_after_healthy(); exit_after && !app_->exit_sent && now >= healthy + *exit_after) {
        log("asking the app to exit (testing)");
        app_->link->send(link::ExitRequested{});
        app_->exit_sent = true;
    }
}

link::Status Supervisor::status() const {
    link::Status s;
    s.channel = state_.channel;
    s.current = app_ ? app_->build.version : state_.good.version;
    if (auto download = worker_.download()) {
        s.phase = link::Phase::Downloading;
        s.percent = download->percent;
        s.available = download->version;
    } else if (worker_.busy()) {
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

void Supervisor::publish_status() {
    if (!app_ || !app_->welcomed) return;
    link::Status s = status();
    if (app_->last_sent == s) return;
    if (app_->link->send(s)) app_->last_sent = s;
}

void Supervisor::show_pending_recovery() {
    PendingRecovery pending = *recovery_;
    recovery_.reset();
    RecoveryChoice choice = show_recovery(
        {pending.why, state_, data_.list(), options_.data.data(), [this] { signal_boot_ok(); }});
    if (std::holds_alternative<Quit>(choice)) {
        quit_ = true;
        return;
    }
    for (Event& e : events_for(choice)) dispatch(std::move(e));
}

void Supervisor::signal_boot_ok() {
    if (options_.boot_ok_event) SetEvent(options_.boot_ok_event);
}

void Supervisor::write_session(bool clean) {
    std::string error;
    if (!store_.write_session({options_.boot_session, self_sha_, clean}, error)) log("{}", error);
}

// Logoff or shutdown: Windows ends the app too, so record its outcome if it
// goes in time, and mark this session clean before we are terminated.
void Supervisor::end_session() {
    if (session_ended_) return;
    log("session is ending");
    if (app_ && WaitForSingleObject(app_->process.handle(), 5000) == WAIT_OBJECT_0) on_app_exit();
    write_session(true);
    session_ended_ = true;
    quit_ = true;
}

LRESULT CALLBACK Supervisor::window_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<Supervisor*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_QUERYENDSESSION) return TRUE;
    if (msg == WM_ENDSESSION && wp && self) {
        self->end_session();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int Supervisor::run() {
    HANDLE running = CreateMutexW(nullptr, TRUE, object_name(L".Launcher.Running").c_str());
    if (running && GetLastError() == ERROR_ALREADY_EXISTS) {
        // The stub counts an exit without boot_ok as a failed boot.
        log("another launcher is running; asking it to come forward");
        if (HANDLE e = OpenEventW(EVENT_MODIFY_STATE, FALSE, object_name(L".Launcher.Activate").c_str())) {
            SetEvent(e);
            CloseHandle(e);
        }
        signal_boot_ok();
        CloseHandle(running);
        return 0;
    }

    self_exe_ = self_exe();
    if (auto digest = digest_file(self_exe_)) self_sha_ = digest->sha256;
    write_session(false);

    HANDLE activate = CreateEventW(nullptr, FALSE, FALSE, object_name(L".Launcher.Activate").c_str());
    WNDCLASSW wc{};
    wc.lpfnWndProc = window_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"PartiesLauncherSession";
    RegisterClassW(&wc);
    // A top-level (not message-only) window, because only those get WM_ENDSESSION.
    HWND window = CreateWindowExW(0, wc.lpszClassName, L"Parties Launcher", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
                                  wc.hInstance, nullptr);
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

    std::error_code ec;
    fs::create_directories(options_.data.data(), ec);
    if (!load_state()) return 1;
    builds_.collect_garbage(state_, self_exe_.parent_path());
    data_.prune(state_);

    dispatch(Resumed{});
    if (options_.recover) recovery_ = PendingRecovery{std::nullopt};
    else dispatch(StartRequested{});

    while (!quit_) {
        // A repair in flight may make the recovery dialog unnecessary.
        if (recovery_ && !app_ && !repairing()) {
            show_pending_recovery();
            continue;
        }
        if (!app_ && !repairing() && !worker_.busy()) break;

        const bool with_app = app_.has_value();
        std::vector<HANDLE> handles{activate, worker_.event()};
        if (with_app) handles.insert(handles.end(), {app_->link->event(), app_->process.handle()});
        DWORD timeout = INFINITE;
        if (auto deadline = next_deadline()) {
            auto wait = std::chrono::ceil<std::chrono::milliseconds>(*deadline - Clock::now()).count();
            timeout = static_cast<DWORD>(std::clamp<long long>(wait, 0, 24ll * 3600 * 1000));
        }
        DWORD r = MsgWaitForMultipleObjectsEx(static_cast<DWORD>(handles.size()), handles.data(), timeout, QS_ALLINPUT,
                                              MWMO_INPUTAVAILABLE);
        if (r == WAIT_OBJECT_0) on_activate();
        else if (r == WAIT_OBJECT_0 + 1) on_worker();
        else if (with_app && r == WAIT_OBJECT_0 + 2) on_link();
        else if (with_app && r == WAIT_OBJECT_0 + 3) on_app_exit();
        else if (r == WAIT_OBJECT_0 + handles.size()) {
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
        on_timers();
    }

    worker_.stop();
    if (!session_ended_) write_session(true);
    log("launcher exiting");
    DestroyWindow(window);
    CloseHandle(activate);
    CloseHandle(running);
    return 0;
}

std::optional<uint64_t> number(std::wstring_view text) {
    std::string narrow(text.begin(), text.end());
    uint64_t value = 0;
    auto [end, ec] = std::from_chars(narrow.data(), narrow.data() + narrow.size(), value);
    if (ec != std::errc{} || end != narrow.data() + narrow.size()) return std::nullopt;
    return value;
}

} // namespace

std::optional<LauncherOptions> parse_options(const std::vector<std::wstring>& args, std::string& error) {
    LauncherOptions o;
    std::optional<fs::path> install_root, data_root;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::wstring& a = args[i];
        auto value_of = [&](std::wstring_view flag) -> std::optional<std::wstring> {
            if (!a.starts_with(flag)) return std::nullopt;
            return a.substr(flag.size());
        };
        auto absolute = [&](const std::wstring& v, const char* what) -> std::optional<fs::path> {
            fs::path p(v);
            if (!p.is_absolute()) {
                error = std::string(what) + " must be an absolute path";
                return std::nullopt;
            }
            return p.lexically_normal();
        };
        if (a == L"--") {
            o.forwarded.assign(args.begin() + static_cast<std::ptrdiff_t>(i) + 1, args.end());
            break;
        }
        if (a == L"--recover") {
            o.recover = true;
        } else if (auto v = value_of(L"--install-root=")) {
            if (!(install_root = absolute(*v, "--install-root"))) return std::nullopt;
        } else if (auto v = value_of(L"--data-root=")) {
            if (!(data_root = absolute(*v, "--data-root"))) return std::nullopt;
        } else if (auto v = value_of(L"--boot-session=")) {
            auto n = number(*v);
            if (!n) return (error = "--boot-session must be a number", std::nullopt);
            o.boot_session = *n;
        } else if (auto v = value_of(L"--boot-ok-event=")) {
            auto n = number(*v);
            if (!n) return (error = "--boot-ok-event must be a handle value", std::nullopt);
            o.boot_ok_event = reinterpret_cast<void*>(static_cast<uintptr_t>(*n));
        } else if (auto v = value_of(L"--test-namespace=")) {
            if (testing::enabled() && !v->empty() && v->find(L'\\') == std::wstring::npos) o.object_prefix = *v;
        } else if (auto v = value_of(L"--feed-dir=")) {
            if (!testing::enabled()) continue;
            if (!(o.feed_dir = absolute(*v, "--feed-dir"))) return std::nullopt;
        } else if (auto v = value_of(L"--feed-base=")) {
            if (!testing::enabled()) continue;
            if (!v->starts_with(L"http://127.0.0.1:") || !v->ends_with(L"/"))
                return (error = "--feed-base must be http://127.0.0.1:<port>/", std::nullopt);
            o.feed_base = utf8(fs::path(*v));
        } else {
            log("ignoring unknown argument {}", utf8(fs::path(a)));
        }
    }
    o.install.root = install_root ? *install_root : known_folder(FOLDERID_UserProgramFiles) / "Parties";
    o.data.root = data_root ? *data_root : known_folder(FOLDERID_LocalAppData) / "Parties";
    if (o.install.root.empty() || o.data.root.empty()) {
        error = "the per-user application folders are unknown";
        return std::nullopt;
    }
    return o;
}

int run_launcher(const LauncherOptions& options) {
    open_log(options.data.logs() / "launcher.log");
    log("launcher starting: install root {}, data root {}, session {}", utf8(options.install.root),
        utf8(options.data.root), options.boot_session);
    Supervisor supervisor(options);
    return supervisor.run();
}

} // namespace parties::update::launcher
