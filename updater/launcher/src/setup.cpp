#include "setup.h"

#include "app_process.h"
#include "build_store.h"
#include "data_guard.h"
#include "file_io.h"
#include "install_plan.h"
#include "legacy_bridge.h"
#include "log.h"
#include "setup_payload.h"
#include "shell_integration.h"
#include "state_store.h"
#include "testing.h"

#include <parties/crypto.h>
#include <parties/update/install_layout.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <charconv>
#include <format>
#include <functional>
#include <thread>

#include <windows.h>
#include <commctrl.h>
#include <objbase.h>

namespace parties::update::launcher {

namespace {

enum ExitCode : int { kDone = 0, kFailed = 1, kCancelled = 2 };

constexpr DWORD kLauncherQuitTimeoutMs = 60'000;
// The legacy updater calls ExitProcess right after starting the carrier.
constexpr std::chrono::milliseconds kPortableExitTimeout{30'000};

struct SetupOptions {
    enum class Mode { Install, Uninstall, Pack } mode = Mode::Install;
    InstallPaths install;
    DataPaths data;
    bool data_root_given = false;
    bool silent = false;
    bool launch = true;
    bool remove_data = false;
    std::wstring object_prefix = L"Parties";
    fs::path pack_dir, pack_out;
    HANDLE wait_for = nullptr;
    std::vector<std::wstring> args;
    bool carrier = false;
    bool handed_over = false;
    LegacyHints legacy;
    std::chrono::milliseconds portable_exit_timeout = kPortableExitTimeout;
};

struct Outcome {
    int code = kDone;
    std::string message;
    std::string note;
};

Outcome failed(std::string message) { return {kFailed, std::move(message)}; }

bool same_name(const fs::path& file, const wchar_t* name) { return _wcsicmp(file.filename().c_str(), name) == 0; }

bool inside(const fs::path& path, const fs::path& dir) {
    std::wstring p = path.lexically_normal().wstring(), d = dir.lexically_normal().wstring();
    if (!d.empty() && d.back() != L'\\') d += L'\\';
    return p.size() > d.size() && _wcsnicmp(p.c_str(), d.c_str(), d.size()) == 0;
}

void report(const std::string& text) {
    log("{}", text);
    HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    if (!err || err == INVALID_HANDLE_VALUE) return;
    std::string line = text + "\n";
    DWORD written = 0;
    WriteFile(err, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
}

bool is_handoff_flag(const std::wstring& a) { return a == L"--update-cleanup" || a == L"--update-replace"; }

std::optional<SetupOptions> parse(const std::vector<std::wstring>& args, std::string& error) {
    SetupOptions o;
    o.args = args;
    const fs::path self = self_exe();
    std::error_code ec;
    o.legacy.working_dir = fs::current_path(ec);
    o.legacy.self_dir = self.parent_path();
    o.carrier = same_name(self, L"parties_client.exe") || std::ranges::any_of(args, is_handoff_flag);
    std::optional<fs::path> install_root, data_root;
    auto absolute = [&](std::wstring_view v, const char* what) -> std::optional<fs::path> {
        fs::path p(v);
        if (p.is_absolute()) return p.lexically_normal();
        error = std::string(what) + " must be an absolute path";
        return std::nullopt;
    };
    for (size_t i = 0; i < args.size(); ++i) {
        const std::wstring& a = args[i];
        auto value = [&](std::wstring_view flag) -> std::optional<std::wstring_view> {
            if (!a.starts_with(flag)) return std::nullopt;
            return std::wstring_view(a).substr(flag.size());
        };
        if (a == L"--uninstall") o.mode = SetupOptions::Mode::Uninstall;
        else if (a == L"--silent") o.silent = true;
        else if (a == L"--no-launch") o.launch = false;
        else if (a == L"--remove-data") o.remove_data = true;
        else if (auto v = value(L"--pack=")) {
            o.mode = SetupOptions::Mode::Pack;
            auto dir = absolute(*v, "--pack");
            if (!dir) return std::nullopt;
            o.pack_dir = *dir;
        } else if (auto v = value(L"--out=")) {
            auto out = absolute(*v, "--out");
            if (!out) return std::nullopt;
            o.pack_out = *out;
        } else if (auto v = value(L"--install-root=")) {
            if (!(install_root = absolute(*v, "--install-root"))) return std::nullopt;
        } else if (auto v = value(L"--data-root=")) {
            if (!(data_root = absolute(*v, "--data-root"))) return std::nullopt;
        } else if (auto v = value(L"--wait-for=")) {
            uint64_t handle = 0;
            std::string digits(v->begin(), v->end());
            std::from_chars(digits.data(), digits.data() + digits.size(), handle);
            o.wait_for = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(handle));
        } else if (auto v = value(L"--test-namespace=")) {
            if (testing::enabled() && !v->empty() && v->find(L'\\') == std::wstring_view::npos) o.object_prefix = *v;
        } else if (is_handoff_flag(a)) {
            if (i + 1 == args.size()) {
                error = utf8(fs::path(a)) + " needs a path";
                return std::nullopt;
            }
            const auto kind = a == L"--update-cleanup" ? OldBinary::Kind::Cleanup : OldBinary::Kind::Replace;
            o.legacy.old = OldBinary{fs::absolute(args[++i], ec).lexically_normal(), kind};
            o.handed_over = true;
        } else if (auto v = value(L"--test-portable-wait-ms=")) {
            uint64_t ms = 0;
            std::string digits(v->begin(), v->end());
            if (testing::enabled() && std::from_chars(digits.data(), digits.data() + digits.size(), ms).ec == std::errc{})
                o.portable_exit_timeout = std::chrono::milliseconds(ms);
        } else if (o.carrier) {
            log("ignoring {}", utf8(fs::path(a)));  // old shortcuts may carry options of the portable app
        } else {
            error = "unknown argument " + utf8(fs::path(a));
            return std::nullopt;
        }
    }
    // A handoff that was cut short leaves the old binary beside the carrier.
    const fs::path leftover = o.legacy.self_dir / "parties_client.old.exe";
    if (o.carrier && !o.legacy.old && fs::exists(leftover, ec)) o.legacy.old = OldBinary{leftover};
    if (o.mode == SetupOptions::Mode::Pack && o.pack_out.empty()) {
        error = "--pack needs --out=<setup exe>";
        return std::nullopt;
    }
    o.install.root = install_root.value_or(default_install_root());
    o.data.root = data_root.value_or(default_data_root());
    o.data_root_given = data_root.has_value();
    if (o.install.root.empty() || o.data.root.empty()) {
        error = "the per-user application folders are unknown";
        return std::nullopt;
    }
    return o;
}

class Mutex {
public:
    Mutex() = default;
    Mutex(Mutex&& o) noexcept : h_(std::exchange(o.h_, nullptr)) {}
    Mutex& operator=(Mutex&&) = delete;
    ~Mutex() {
        if (!h_) return;
        ReleaseMutex(h_);
        CloseHandle(h_);
    }

    // nullopt: another setup or uninstall of this install is running.
    static std::optional<Mutex> setup(const SetupOptions& o) {
        Mutex m;
        m.h_ = CreateMutexW(nullptr, TRUE, (L"Local\\" + o.object_prefix + L".Setup").c_str());
        if (!m.h_ || GetLastError() == ERROR_ALREADY_EXISTS) return std::nullopt;
        return m;
    }

    // Holds Local\<prefix>.Launcher.Running so that no launcher runs while
    // setup changes the install: a running one is asked to quit, which
    // closes the app through its normal path first, and one started later
    // finds the mutex taken and exits.
    static std::optional<Mutex> launcher(const SetupOptions& o) {
        Mutex m;
        m.h_ = CreateMutexW(nullptr, FALSE, (L"Local\\" + o.object_prefix + L".Launcher.Running").c_str());
        if (!m.h_) return std::nullopt;
        DWORD r = WaitForSingleObject(m.h_, 0);
        if (r == WAIT_TIMEOUT) {
            log("Parties is running; asking its launcher to quit");
            HANDLE quit = CreateEventW(nullptr, FALSE, FALSE, (L"Local\\" + o.object_prefix + L".Launcher.Quit").c_str());
            if (quit) SetEvent(quit);
            r = WaitForSingleObject(m.h_, kLauncherQuitTimeoutMs);
            // Closing it before any new launcher can start keeps a stale signal from stopping that one.
            if (quit) CloseHandle(quit);
        }
        if (r != WAIT_OBJECT_0 && r != WAIT_ABANDONED) {
            CloseHandle(std::exchange(m.h_, nullptr));
            return std::nullopt;
        }
        return m;
    }

private:
    HANDLE h_ = nullptr;
};

constexpr const char* kStillRunning = "Parties is still running. Close it and try again.";

bool ensure_sealed(const BuildStore& builds, const Payload& payload, std::string& error) {
    const fs::path dir = builds.dir_of(payload.entry.id());
    std::error_code ec;
    if (fs::exists(dir, ec)) {
        if (builds.adopt(dir)) return true;
        log("repairing {}", utf8(dir));
        if (!remove_tree(dir, error)) return false;
    }
    auto source = make_payload_source(payload);
    std::atomic<bool> cancel{false};
    auto sealed = builds.seal(payload.entry, *source, nullptr, cancel);
    if (!sealed) error = sealed.error();
    return sealed.has_value();
}

bool ensure_stub(const InstallPaths& install, std::span<const uint8_t> stub, bool replace, std::string& error) {
    const fs::path file = install.root / kStubFile;
    auto current = digest_file(file);
    if (current && !replace) return true;
    auto wanted = digest_bytes(stub);
    if (current && wanted && *current == *wanted) return true;
    log("writing {}", utf8(file));
    return durable_replace(file, {reinterpret_cast<const char*>(stub.data()), stub.size()}, error);
}

// A restore whose backup is gone can never finish, and the launcher would
// stop at it on every start. Repair gives up on it the way the launcher does
// when the restore fails without changing the data.
LoadResult drop_unfinishable_restore(LoadResult loaded, const DataGuard& data, const BuildStore& builds,
                                     StateStore& store) {
    auto* state = std::get_if<SupervisorState>(&loaded);
    auto* restoring = state ? std::get_if<Restoring>(&state->operation) : nullptr;
    if (!restoring || !restoring->from || data.intact(restoring->from->id)) return loaded;
    log("the backup {} that a restore needs is missing or damaged; leaving the data as it is", restoring->from->id.stem);
    Step step_out = step(*state, RestoreFailed{}, {});
    std::string error;
    for (const Effect& effect : step_out.effects) {
        if (const auto* commit = std::get_if<Commit>(&effect)) {
            if (!store.commit(commit->state, error)) {
                log("cannot save the install state: {}", error);
                return loaded;
            }
        } else if (const auto* doomed = std::get_if<DeleteBuild>(&effect)) {
            if (!builds.remove(doomed->build, step_out.next, error)) log("cannot delete: {}", error);
        }
    }
    return step_out.next;
}

// The plan's state change, executed the way the launcher executes effects.
std::expected<std::optional<BuildId>, std::string> apply_state(const InstallPlan& plan, const LoadResult& loaded,
                                                               const BuildStore& builds, StateStore& store,
                                                               const BuildId& bundled) {
    std::string error;
    if (std::holds_alternative<Adopt>(plan.state)) {
        auto record = builds.adopt(builds.dir_of(bundled));
        if (!record) return std::unexpected(record.error());
        if (!store.commit(adopted_state(*record), error)) return std::unexpected(error);
        log("adopted {} as the good build", bundled.dir_name());
        return std::optional(bundled);
    }
    if (const auto* stage = std::get_if<Stage>(&plan.state)) {
        for (const Effect& effect : stage->step.effects) {
            if (const auto* commit = std::get_if<Commit>(&effect)) {
                if (!store.commit(commit->state, error)) return std::unexpected(error);
            } else if (const auto* doomed = std::get_if<DeleteBuild>(&effect)) {
                if (!builds.remove(doomed->build, stage->step.next, error)) log("cannot delete: {}", error);
            }
        }
        log("staged {}; {} stays good until it is proven", bundled.dir_name(), stage->step.next.good.dir_name());
        return std::optional(stage->step.next.good);
    }
    if (const auto* state = std::get_if<SupervisorState>(&loaded)) return std::optional(state->good);
    return std::optional<BuildId>();
}

ChooseIdentity identity_chooser(const SetupOptions& o);

Outcome install(const SetupOptions& o, const Payload& payload) {
    auto lock = Mutex::setup(o);
    if (!lock) return failed("Another Parties setup is running. Wait for it to finish.");
    auto exclusive = Mutex::launcher(o);
    if (!exclusive) return failed(kStillRunning);

    // Before any shortcut changes: the Start menu Parties.lnk may be the only
    // pointer to a portable copy, and before the first start the data root
    // must already hold its identity.
    testing::before_effect();
    const LegacyData legacy = move_legacy_data(o.data, legacy_dirs(o.legacy), identity_chooser(o));
    if (!legacy.error.empty())
        return failed("Parties could not copy your data, so nothing was installed: " + legacy.error);

    StateStore store(o.install, utf8(o.data.data()));
    BuildStore builds(o.install);
    const LoadResult loaded = drop_unfinishable_restore(store.load(), DataGuard(o.data), builds, store);
    const BuildRecord bundled = BuildRecord::from_entry(payload.entry, kWindowsClient);
    const InstallPlan plan = plan_install(loaded, bundled);
    std::string error;
    std::error_code ec;
    fs::create_directories(o.install.builds(), ec);
    if (ec) return failed("Parties cannot create " + utf8(o.install.builds()) + ": " + ec.message());
    testing::before_effect();
    if (plan.seal_bundled && !ensure_sealed(builds, payload, error)) return failed("Parties cannot be installed: " + error);
    testing::before_effect();
    if (!ensure_stub(o.install, payload.stub, plan.replace_stub, error)) return failed("Parties cannot be installed: " + error);
    testing::before_effect();
    auto good = apply_state(plan, loaded, builds, store, bundled.id);
    if (!good) return failed("Parties cannot save its install state: " + good.error());
    testing::before_effect();
    if (*good && !write_uninstall_entry(o.install, **good, error)) return failed(error);
    testing::before_effect();
    if (!write_shortcuts(o.install, error)) return failed(error);

    const std::string version = payload.entry.version.to_string();
    if (std::holds_alternative<Adopt>(plan.state)) return {kDone, "Parties " + version + " is installed.", legacy.note};
    if (std::holds_alternative<Stage>(plan.state))
        return {kDone, "Parties " + version + " is ready and starts the next time you open Parties.", legacy.note};
    return {kDone, "Parties is repaired. Your data was not changed.", legacy.note};
}

bool launcher_running(const SetupOptions& o) {
    HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, (L"Local\\" + o.object_prefix + L".Launcher.Running").c_str());
    if (!m) return false;
    CloseHandle(m);
    return true;
}

void launch_installed(const SetupOptions& o) {
    std::vector<std::wstring> args{L"--install-root=" + o.install.root.wstring()};
    if (testing::enabled()) {
        args.push_back(L"--test-namespace=" + o.object_prefix);
        if (o.data_root_given) args.push_back(L"--data-root=" + o.data.root.wstring());
    }
    const fs::path stub = o.install.root / kStubFile;
    std::wstring cmd = quote_argument(stub.wstring());
    for (const auto& a : args) cmd += L" " + quote_argument(a);
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(stub.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, o.install.root.c_str(), &si,
                        &pi)) {
        log("cannot start {}: {}", utf8(stub), last_error_text());
        return;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
}

std::string utc_stamp() {
    return std::format("{:%Y%m%d-%H%M%S}", std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
}

Outcome uninstall(const SetupOptions& o) {
    auto lock = Mutex::setup(o);
    if (!lock) return failed("Another Parties setup is running. Wait for it to finish.");
    auto exclusive = Mutex::launcher(o);
    if (!exclusive) return failed(kStillRunning);

    std::string error;
    std::error_code ec;
    if (fs::exists(o.data.db(), ec)) {
        DataGuard data(o.data);
        if (!data.snapshot({"pre-uninstall-" + utc_stamp()}, error))
            return failed("Parties could not back up your data, so nothing was removed: " + error);
    }
    if (!remove_tree(o.install.root, error)) {
        log("{}", error);
        return failed("Parties could not be removed because some of its files are in use. Close Parties and try again.");
    }
    remove_shortcuts(o.install);
    remove_uninstall_entry(o.install);
    log("removed {}", utf8(o.install.root));

    const std::string data_root = utf8(o.data.root);
    if (!o.remove_data || !fs::exists(o.data.root, ec))
        return {kDone, "Parties was removed. Your identity, servers and settings are still in " + data_root +
                           ", so installing Parties again brings them back."};
    if (!recycle(o.data.root, error)) {
        log("{}", error);
        return {kDone, "Parties was removed, but its data could not be moved to the Recycle Bin, so it is still in " +
                           data_root + "."};
    }
    return {kDone, "Parties was removed, and its data was moved to the Recycle Bin. You can restore it from there."};
}

// The uninstaller cannot delete the build directory it runs from, so the
// copy in a build directory hands over to a temporary copy and exits.
std::optional<int> hand_over_to_temporary_copy(const SetupOptions& o) {
    const fs::path self = self_exe();
    if (o.wait_for) {
        WaitForSingleObject(o.wait_for, 30'000);
        CloseHandle(o.wait_for);
        return std::nullopt;
    }
    if (!inside(self, o.install.root)) return std::nullopt;
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) /
                         std::format("Parties-uninstall-{}-{}", GetCurrentProcessId(), GetTickCount64());
    const fs::path copy = dir / kWindowsClient.launcher_file;
    fs::create_directories(dir, ec);
    HANDLE me = nullptr;
    if (ec || !CopyFileW(self.c_str(), copy.c_str(), FALSE) ||
        !DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &me, SYNCHRONIZE, TRUE, 0)) {
        report("cannot prepare the uninstaller: " + last_error_text());
        return kFailed;
    }
    std::wstring cmd = quote_argument(copy.wstring());
    for (const auto& a : o.args) cmd += L" " + quote_argument(a);
    cmd += L" --wait-for=" + std::to_wstring(reinterpret_cast<uintptr_t>(me));
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(copy.c_str(), cmd.data(), nullptr, nullptr, TRUE, 0, nullptr, dir.c_str(), &si, &pi)) {
        report("cannot start the uninstaller: " + last_error_text());
        return kFailed;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return kDone;
}

// The temporary copy cannot delete itself, so a shell removes its folder once it has exited.
void remove_temporary_copy() {
    const fs::path dir = self_exe().parent_path();
    std::error_code ec;
    if (!dir.filename().wstring().starts_with(L"Parties-uninstall-") ||
        !fs::equivalent(dir.parent_path(), fs::temp_directory_path(ec), ec))
        return;
    wchar_t system[MAX_PATH]{};
    GetSystemDirectoryW(system, MAX_PATH);
    const fs::path cmd_exe = fs::path(system) / L"cmd.exe";
    std::wstring cmd = L"cmd.exe /d /c ping -n 2 127.0.0.1 >nul & rmdir /s /q " + quote_argument(dir.wstring());
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(cmd_exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                       fs::temp_directory_path(ec).c_str(), &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

int show(const wchar_t* instruction, const std::wstring& content, PCWSTR icon,
         const std::vector<TASKDIALOG_BUTTON>& buttons = {}, const wchar_t* checkbox = nullptr,
         bool* checked = nullptr) {
    TASKDIALOGCONFIG cfg{sizeof cfg};
    cfg.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION;
    cfg.pszWindowTitle = L"Parties";
    cfg.pszMainIcon = icon;
    cfg.pszMainInstruction = instruction;
    cfg.pszContent = content.c_str();
    cfg.pButtons = buttons.data();
    cfg.cButtons = static_cast<UINT>(buttons.size());
    cfg.dwCommonButtons = buttons.empty() ? TDCBF_OK_BUTTON : TDCBF_CANCEL_BUTTON;
    cfg.pszVerificationText = checkbox;
    int pressed = IDCANCEL;
    BOOL verified = FALSE;
    if (FAILED(TaskDialogIndirect(&cfg, &pressed, nullptr, checkbox ? &verified : nullptr))) return IDCANCEL;
    if (checked) *checked = verified != FALSE;
    return pressed;
}

constexpr int kKeepLive = 101;
constexpr int kUseLegacy = 102;

IdentityChoice ask_identity(const DataPaths& data, const client::IdentityConflict& c) {
    const std::wstring legacy = widen(public_key_fingerprint(c.legacy));
    const std::wstring live = c.live ? widen(public_key_fingerprint(*c.live)) : std::wstring();
    const std::wstring installed = c.live ? L"The installed Parties uses the identity with fingerprint " + live + L"."
                                          : std::wstring(L"The installed Parties has no identity yet.");
    const std::wstring content =
        L"People you talk to know you by your identity.\n\n" + installed + L"\n\nThe portable copy in " +
        c.legacy_db.parent_path().wstring() + L" uses the identity with fingerprint " + legacy +
        L".\n\nThe data you do not choose stays saved as a backup in " + data.backups().wstring() + L".";
    const std::wstring keep = c.live ? L"Keep the installed identity\n" + live : std::wstring(L"Keep the installed data");
    const std::wstring use = L"Use the portable copy's identity, with its servers and settings\n" + legacy;
    const TASKDIALOG_BUTTON buttons[] = {{kKeepLive, keep.c_str()}, {kUseLegacy, use.c_str()}};
    TASKDIALOGCONFIG cfg{sizeof cfg};
    cfg.dwFlags = TDF_USE_COMMAND_LINKS;
    cfg.pszWindowTitle = L"Parties";
    cfg.pszMainIcon = TD_WARNING_ICON;
    cfg.pszMainInstruction = L"Which identity should Parties use?";
    cfg.pszContent = content.c_str();
    cfg.pButtons = buttons;
    cfg.cButtons = static_cast<UINT>(std::size(buttons));
    cfg.nDefaultButton = kKeepLive;
    int pressed = kKeepLive;
    if (FAILED(TaskDialogIndirect(&cfg, &pressed, nullptr, nullptr))) return IdentityChoice::KeepLive;
    return pressed == kUseLegacy ? IdentityChoice::UseLegacy : IdentityChoice::KeepLive;
}

ChooseIdentity identity_chooser(const SetupOptions& o) {
    if (auto scripted = testing::identity_choice())
        return [legacy = *scripted == "legacy"](const client::IdentityConflict&) {
            return legacy ? IdentityChoice::UseLegacy : IdentityChoice::KeepLive;
        };
    if (o.silent) return {};
    return [data = o.data](const client::IdentityConflict& c) { return ask_identity(data, c); };
}

// Runs `work` on a worker thread behind a marquee progress dialog.
Outcome with_progress(const wchar_t* instruction, const std::function<Outcome()>& work) {
    struct Shared {
        std::atomic<bool> done{false};
        Outcome outcome;
    } shared;
    std::thread worker([&] {
        HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        shared.outcome = work();
        if (SUCCEEDED(com)) CoUninitialize();
        shared.done = true;
    });
    TASKDIALOGCONFIG cfg{sizeof cfg};
    cfg.dwFlags = TDF_SHOW_MARQUEE_PROGRESS_BAR | TDF_CALLBACK_TIMER;
    cfg.dwCommonButtons = TDCBF_CLOSE_BUTTON;
    cfg.pszWindowTitle = L"Parties";
    cfg.pszMainInstruction = instruction;
    cfg.pszContent = L"This takes a few seconds.";
    cfg.lpCallbackData = reinterpret_cast<LONG_PTR>(&shared);
    cfg.pfCallback = [](HWND hwnd, UINT msg, WPARAM, LPARAM, LONG_PTR data) -> HRESULT {
        auto* s = reinterpret_cast<Shared*>(data);
        if (msg == TDN_CREATED) {
            SendMessageW(hwnd, TDM_SET_PROGRESS_BAR_MARQUEE, TRUE, 0);
            SendMessageW(hwnd, TDM_ENABLE_BUTTON, IDCLOSE, FALSE);
        } else if (msg == TDN_TIMER && s->done) {
            SendMessageW(hwnd, TDM_CLICK_BUTTON, IDCLOSE, 0);
        } else if (msg == TDN_BUTTON_CLICKED && !s->done) {
            return S_FALSE;
        }
        return S_OK;
    };
    TaskDialogIndirect(&cfg, nullptr, nullptr, nullptr);
    worker.join();
    return shared.outcome;
}

Outcome run_work(const SetupOptions& o, const wchar_t* instruction, const std::function<Outcome()>& work) {
    return o.silent ? work() : with_progress(instruction, work);
}

constexpr int kProceed = 100;

int run_install(const SetupOptions& o, const std::expected<Payload, std::string>& payload) {
    if (!payload) {
        report("setup refused its payload: " + payload.error());
        if (!o.silent)
            show(L"Parties cannot be installed", widen(payload.error()) + L"\n\nDownload the installer again.",
                 TD_ERROR_ICON);
        return kFailed;
    }
    std::error_code ec;
    const bool repair = fs::exists(o.install.supervisor(), ec);
    if (!o.silent) {
        const std::wstring content =
            L"Parties will be installed for your Windows account in " + o.install.root.wstring() +
            L". No administrator permission is needed.\n\nYour identity, servers and settings stay in " +
            o.data.root.wstring() + L" and are not changed.";
        const std::vector<TASKDIALOG_BUTTON> buttons{{kProceed, repair ? L"Repair" : L"Install"}};
        if (show(repair ? L"Repair Parties" : L"Install Parties", content, TD_INFORMATION_ICON, buttons) != kProceed)
            return kCancelled;
    }
    Outcome outcome = run_work(o, repair ? L"Repairing Parties" : L"Installing Parties", [&] { return install(o, *payload); });
    report(outcome.message);
    if (outcome.code != kDone) {
        if (!o.silent) show(L"Parties cannot be installed", widen(outcome.message), TD_ERROR_ICON);
        return outcome.code;
    }
    if (o.launch) launch_installed(o);
    if (!outcome.note.empty()) {
        report(outcome.note);
        if (!o.silent) show(L"Parties is installed", widen(outcome.note), TD_INFORMATION_ICON);
    }
    return kDone;
}

int run_carrier(const SetupOptions& o, const std::expected<Payload, std::string>& payload) {
    if (!payload) {
        report("setup refused its payload: " + payload.error());
        if (!o.silent)
            show(L"Parties cannot be updated", widen(payload.error()) + L"\n\nDownload Parties again.", TD_ERROR_ICON);
        return kFailed;
    }
    if (!o.handed_over && launcher_running(o)) {
        log("Parties is running; bringing it forward");
        launch_installed(o);
        return kDone;
    }
    const bool portable_closed = !o.handed_over || wait_for_portable_exit(o.object_prefix, o.portable_exit_timeout);
    auto work = [&] { return install(o, *payload); };
    Outcome outcome = o.handed_over ? run_work(o, L"Updating Parties", work) : work();
    report(outcome.message);
    if (outcome.code != kDone) {
        std::string message = outcome.message;
        std::error_code ec;
        if (o.legacy.old && fs::exists(o.legacy.old->path, ec))
            message += " Your previous version is still in " + utf8(o.legacy.old->path) + ".";
        if (!o.silent) show(L"Parties could not be updated", widen(message), TD_ERROR_ICON);
        return outcome.code;
    }
    testing::before_effect();
    if (o.legacy.old) retire_old_binary(*o.legacy.old, self_exe());
    testing::before_effect();
    std::string note = outcome.note;
    if (!portable_closed)
        note += (note.empty() ? "" : " ") + std::string("Close the older Parties window, then start Parties from the Start menu.");
    else if (o.launch)
        launch_installed(o);
    if (!note.empty()) {
        report(note);
        if (!o.silent) show(L"Parties is updated", widen(note), TD_INFORMATION_ICON);
    }
    return kDone;
}

int confirm_and_uninstall(SetupOptions o) {
    if (!o.silent) {
        const std::vector<TASKDIALOG_BUTTON> buttons{{kProceed, L"Uninstall"}};
        const std::wstring content = L"Parties closes if it is running. Your identity, servers and settings stay in " +
                                     o.data.root.wstring() + L" unless you choose to remove them.";
        int pressed = show(L"Uninstall Parties?", content, TD_WARNING_ICON, buttons,
                           L"Also move my Parties data to the Recycle Bin", &o.remove_data);
        if (pressed != kProceed) return kCancelled;
    }
    Outcome outcome = run_work(o, L"Uninstalling Parties", [&] { return uninstall(o); });
    report(outcome.message);
    if (!o.silent)
        show(outcome.code == kDone ? L"Parties is uninstalled" : L"Parties could not be uninstalled",
             widen(outcome.message), outcome.code == kDone ? TD_INFORMATION_ICON : TD_ERROR_ICON);
    return outcome.code;
}

int run_uninstall(const SetupOptions& o) {
    if (auto handed_over = hand_over_to_temporary_copy(o)) return *handed_over;
    int code = confirm_and_uninstall(o);
    remove_temporary_copy();
    return code;
}

int run_pack(const SetupOptions& o) {
    std::string error;
    if (!pack_payload(o.pack_dir, o.pack_out, testing::pinned_keys(), error)) {
        report(error);
        return kFailed;
    }
    return kDone;
}

bool asks_for(const std::vector<std::wstring>& args, std::wstring_view flag) {
    for (const std::wstring& a : args) {
        if (a == L"--") return false;
        if (a.starts_with(flag)) return true;
    }
    return false;
}

} // namespace

std::optional<int> run_setup_modes(const std::vector<std::wstring>& args) {
    std::optional<std::expected<Payload, std::string>> payload;
    if (!asks_for(args, L"--uninstall") && !asks_for(args, L"--pack=")) {
        payload = embedded_payload(testing::pinned_keys());
        if (!payload) return std::nullopt;
    }
    std::error_code ec;
    open_log(fs::temp_directory_path(ec) / "PartiesSetup.log");
    std::string error;
    auto options = parse(args, error);
    if (!options) {
        report(error);
        return kFailed;
    }
    HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int code = kFailed;
    switch (options->mode) {
    case SetupOptions::Mode::Pack: code = run_pack(*options); break;
    case SetupOptions::Mode::Uninstall: code = run_uninstall(*options); break;
    case SetupOptions::Mode::Install:
        if (!payload) break;
        code = options->carrier ? run_carrier(*options, *payload) : run_install(*options, *payload);
        break;
    }
    if (SUCCEEDED(com)) CoUninitialize();
    return code;
}

} // namespace parties::update::launcher
