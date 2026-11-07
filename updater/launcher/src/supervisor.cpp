#include "supervisor.h"

#include "app_process.h"
#include "build_store.h"
#include "data_guard.h"
#include "file_io.h"
#include "link_server.h"
#include "log.h"
#include "recovery_ui.h"
#include "shell_integration.h"
#include "state_store.h"
#include "supervisor_core.h"
#include "testing.h"
#include "update_worker.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <memory>

#include <windows.h>
#include <bcrypt.h>

namespace parties::update::launcher {

namespace {

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

// Windows has no host to outgrow.
constexpr Runner kRunner{.launcher_protocol = kLinkProtocol, .host_abi = 0};

std::unique_ptr<UpdateSource> make_source(const LauncherOptions& o) {
    if (o.feed_dir) return make_local_dir_source(*o.feed_dir);
    if (o.feed_base) return make_https_source(*o.feed_base, true);
    // A testing launcher talks to no real feed unless told to, and a build
    // that pins no key could verify nothing it fetched.
    if (testing::enabled() || testing::pinned_keys().empty()) return nullptr;
    return make_https_source(std::string(kFeedBase), false);
}

// The Win32 event loop around SupervisorCore, and its Executor: a job object,
// a named pipe per app, WinHTTP or a local feed, the Apps entry, the boot
// stub's event, and WM_ENDSESSION.
class Supervisor final : Executor {
public:
    explicit Supervisor(const LauncherOptions& o)
        : options_(o), store_(o.install, utf8(o.data.data())), builds_(o.install),
          data_({o.data.db(), o.data.backups()}),
          worker_(make_source(o),
                  WorkerConfig{kWindowsClient, kRunner, testing::pinned_keys(), o.data.trust(),
                               [builds = builds_](const BuildEntry& entry, UpdateSource& source,
                                                  const Progress& progress, const std::atomic<bool>& cancel) {
                                   return builds.seal(entry, source, progress, cancel);
                               },
                               [event = worker_event_] { SetEvent(event); }}) {}
    ~Supervisor() {
        worker_.stop();
        CloseHandle(worker_event_);
    }

    int run();

private:
    // One app in the job object, with its link. SupervisorCore owns it; app_
    // points at it for the wait handles while it exists.
    struct AppRun final : AppSession {
        AppRun(Supervisor& s, AppProcess p, std::unique_ptr<LinkServer> l)
            : owner(s), process(std::move(p)), link(std::move(l)) {
            owner.app_ = this;
        }
        ~AppRun() override {
            if (owner.app_ == this) owner.app_ = nullptr;
        }

        bool send(const link::LauncherMessage& message) override { return link->send(message); }
        void terminate() override { process.terminate(); }
        void activate() override {
            HANDLE e = OpenEventW(EVENT_MODIFY_STATE, FALSE, owner.object_name(L".Client.Activate").c_str());
            if (e) {
                SetEvent(e);
                CloseHandle(e);
            }
        }

        Supervisor& owner;
        AppProcess process;
        std::unique_ptr<LinkServer> link;
    };

    Clock::time_point now() const override { return Clock::now(); }
    void before_effect() override { testing::before_effect(); }
    bool commit(const SupervisorState& next, const BuildId& previous_good, std::string& error) override;
    bool take_snapshot(const SnapshotId& id, const SupervisorState& committed, std::string& error) override;
    std::expected<void, RestoreError> restore_snapshot(const std::optional<SnapshotId>& from,
                                                       const SnapshotId& preserve_live_as) override {
        return data_.restore(from, preserve_live_as);
    }
    std::expected<std::unique_ptr<AppSession>, SpawnFailure> spawn(const BuildRecord& build) override;
    bool delete_build(const BuildId& build, const SupervisorState& committed, std::string& error) override {
        return builds_.remove(build, committed, error);
    }
    void signal_boot_ok() override;
    std::expected<std::optional<DataInfo>, std::string> inspect_live_db() const override { return data_.inspect(); }
    std::vector<Backup> list_backups() const override { return data_.list(); }
    bool feed_available() const override { return worker_.available(); }
    bool job_busy() const override { return worker_.busy(); }
    bool start_job(JobRequest request) override { return worker_.start(std::move(request)); }
    std::optional<Download> download() const override { return worker_.download(); }
    void cancel_job() override { worker_.cancel(); }

    std::optional<SupervisorState> load_state();
    std::optional<SupervisorState> adopt_own_build();
    void on_app_exit();
    void on_link();
    void show_pending_recovery(RecoveryDue due);
    void write_session(bool clean);
    void end_session();
    std::wstring object_name(const wchar_t* suffix) const { return L"Local\\" + options_.object_prefix + suffix; }

    static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    const LauncherOptions& options_;
    StateStore store_;
    BuildStore builds_;
    DataGuard data_;
    HANDLE worker_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    UpdateWorker worker_;
    AppJob job_;

    AppRun* app_ = nullptr;
    // Declared after job_, so a running app's handles close before the job
    // object that kills it on close.
    std::optional<SupervisorCore> core_;
    bool quit_ = false;
    bool session_ended_ = false;

    fs::path self_exe_;
    Sha256 self_sha_;
};

std::optional<SupervisorState> Supervisor::adopt_own_build() {
    auto record = builds_.adopt(self_exe_.parent_path());
    if (!record) {
        tell_user("Parties is not installed correctly: " + record.error());
        return std::nullopt;
    }
    SupervisorState initial = adopted_state(*record);
    std::string error;
    if (!store_.commit(initial, error)) {
        tell_user("Parties cannot save its update state: " + error);
        return std::nullopt;
    }
    log("adopted {} as the good build", record->id.dir_name());
    return initial;
}

std::optional<SupervisorState> Supervisor::load_state() {
    LoadResult loaded = store_.load();
    if (auto* s = std::get_if<SupervisorState>(&loaded)) return *s;
    if (std::holds_alternative<NewerLauncherState>(loaded)) {
        // The boot stub falls back to its own safe mode when we exit without
        // boot_ok, so leaving now is the safe answer.
        log("supervisor.json was written by a newer launcher; leaving it untouched");
        return std::nullopt;
    }
    if (std::holds_alternative<UnreadableState>(loaded))
        log("supervisor.json and its .prev are unreadable; rebuilding from the running build");
    return adopt_own_build();
}

bool Supervisor::commit(const SupervisorState& next, const BuildId& previous_good, std::string& error) {
    if (!store_.commit(next, error)) return false;
    if (next.good != previous_good) refresh_uninstall_entry(options_.install, next.good);
    return true;
}

bool Supervisor::take_snapshot(const SnapshotId& id, const SupervisorState& committed, std::string& error) {
    if (!data_.snapshot(id, error)) return false;
    data_.prune(committed);
    return true;
}

std::expected<std::unique_ptr<AppSession>, SpawnFailure> Supervisor::spawn(const BuildRecord& build) {
    std::optional<ExecLease> lease = builds_.lease_app(build);
    if (!lease) return std::unexpected(SpawnFailure::BuildCorrupt);
    std::wstring pipe = L"\\\\.\\pipe\\parties-link-" + std::to_wstring(options_.boot_session) + L"-" +
                        widen(random_hex(16));
    auto link = LinkServer::create(pipe);
    if (!link) {
        log("{}", link.error());
        return std::unexpected(SpawnFailure::StartFailed);
    }
    std::vector<std::wstring> args{L"--data-dir=" + options_.data.data().wstring(),
                                   L"--backups-dir=" + options_.data.backups().wstring(),
                                   L"--supervisor-pipe=" + pipe};
    args.insert(args.end(), options_.forwarded.begin(), options_.forwarded.end());
    auto process = AppProcess::spawn(job_, std::move(*lease), args, options_.data.data());
    if (!process) {
        log("{}", process.error());
        return std::unexpected(SpawnFailure::StartFailed);
    }
    (*link)->expect_client(process->pid());
    return std::make_unique<AppRun>(*this, std::move(*process), std::move(*link));
}

void Supervisor::on_app_exit() {
    on_link();
    if (!app_) return;
    core_->on_app_exited(app_->process.exit_kind());
}

void Supervisor::on_link() {
    if (app_) core_->on_link(app_->link->poll());
}

void Supervisor::show_pending_recovery(RecoveryDue due) {
    RecoveryChoice choice = show_recovery({due.why, core_->state(), due.live_db, std::move(due.backups),
                                           options_.data.data(), [this] { signal_boot_ok(); }});
    if (std::holds_alternative<Quit>(choice)) {
        quit_ = true;
        return;
    }
    core_->on_recovery_choice(choice);
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
    // Before the core exists no job has started, so there is nothing to cancel.
    if (core_) core_->cancel_jobs();
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
    HANDLE quit = CreateEventW(nullptr, FALSE, FALSE, object_name(L".Launcher.Quit").c_str());
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
    std::optional<SupervisorState> state = load_state();
    if (!state) return 1;
    builds_.collect_garbage(*state, self_exe_.parent_path());
    data_.prune(*state);
    refresh_uninstall_entry(options_.install, state->good);

    const testing::Timing timing = testing::timing();
    core_.emplace(static_cast<Executor&>(*this), std::move(*state),
                  CoreConfig{timing.unit, timing.policy, testing::exit_after_healthy(), testing::job_cap()});
    core_->start(options_.recover);

    while (!quit_) {
        if (auto due = core_->take_recovery()) {
            show_pending_recovery(std::move(*due));
            continue;
        }
        if (core_->idle()) break;

        const bool with_app = app_ != nullptr;
        std::vector<HANDLE> handles{activate, worker_event_, quit};
        if (with_app) handles.insert(handles.end(), {app_->link->event(), app_->process.handle()});
        DWORD timeout = INFINITE;
        if (auto deadline = core_->next_deadline()) {
            auto wait = std::chrono::ceil<std::chrono::milliseconds>(*deadline - Clock::now()).count();
            timeout = static_cast<DWORD>(std::clamp<long long>(wait, 0, 24ll * 3600 * 1000));
        }
        DWORD r = MsgWaitForMultipleObjectsEx(static_cast<DWORD>(handles.size()), handles.data(), timeout, QS_ALLINPUT,
                                              MWMO_INPUTAVAILABLE);
        if (r == WAIT_OBJECT_0) core_->on_activate();
        else if (r == WAIT_OBJECT_0 + 1) core_->on_worker(worker_.take());
        else if (r == WAIT_OBJECT_0 + 2) {
            log("asked to quit");
            core_->on_quit();
        } else if (with_app && r == WAIT_OBJECT_0 + 3) on_link();
        else if (with_app && r == WAIT_OBJECT_0 + 4) on_app_exit();
        else if (r == WAIT_OBJECT_0 + handles.size()) {
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
        core_->on_timers();
    }

    worker_.stop();
    if (!session_ended_) write_session(true);
    log("launcher exiting");
    DestroyWindow(window);
    CloseHandle(quit);
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
    o.install.root = install_root ? *install_root : default_install_root();
    o.data.root = data_root ? *data_root : default_data_root();
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
