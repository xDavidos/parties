#include "setup.h"

#include "app_process.h"
#include "build_store.h"
#include "data_guard.h"
#include "file_io.h"
#include "install_plan.h"
#include "log.h"
#include "setup_payload.h"
#include "shell_integration.h"
#include "state_store.h"
#include "testing.h"

#include <parties/update/install_layout.h>

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
    HANDLE wait_for = nullptr;  // set in the temporary uninstaller copy
    std::vector<std::wstring> args;
};

struct Outcome {
    int code = kDone;
    std::string message;
};

Outcome failed(std::string message) { return {kFailed, std::move(message)}; }

std::wstring widen(std::string_view s) { return fs::path(std::u8string(s.begin(), s.end())).wstring(); }

fs::path self_exe() {
    std::wstring buffer(32768, L'\0');
    buffer.resize(GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size())));
    return buffer;
}

bool inside(const fs::path& path, const fs::path& dir) {
    std::wstring p = path.lexically_normal().wstring(), d = dir.lexically_normal().wstring();
    if (!d.empty() && d.back() != L'\\') d += L'\\';
    return p.size() > d.size() && _wcsnicmp(p.c_str(), d.c_str(), d.size()) == 0;
}

// Silent runs report through stderr when the caller redirected it.
void report(const std::string& text) {
    log("{}", text);
    HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    if (!err || err == INVALID_HANDLE_VALUE) return;
    std::string line = text + "\n";
    DWORD written = 0;
    WriteFile(err, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
}

std::optional<SetupOptions> parse(const std::vector<std::wstring>& args, std::string& error) {
    SetupOptions o;
    o.args = args;
    std::optional<fs::path> install_root, data_root;
    auto absolute = [&](std::wstring_view v, const char* what) -> std::optional<fs::path> {
        fs::path p(v);
        if (p.is_absolute()) return p.lexically_normal();
        error = std::string(what) + " must be an absolute path";
        return std::nullopt;
    };
    for (const std::wstring& a : args) {
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
        } else {
            error = "unknown argument " + utf8(fs::path(a));
            return std::nullopt;
        }
    }
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

    // Another setup or uninstall of this install is running.
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

Outcome install(const SetupOptions& o, const Payload& payload) {
    auto lock = Mutex::setup(o);
    if (!lock) return failed("Another Parties setup is running. Wait for it to finish.");
    auto exclusive = Mutex::launcher(o);
    if (!exclusive) return failed(kStillRunning);

    StateStore store(o.install, utf8(o.data.data()));
    BuildStore builds(o.install);
    const LoadResult loaded = store.load();
    const BuildRecord bundled = BuildRecord::from_entry(payload.entry);
    const InstallPlan plan = plan_install(loaded, bundled);
    std::string error;
    std::error_code ec;
    fs::create_directories(o.install.builds(), ec);
    if (ec) return failed("Parties cannot create " + utf8(o.install.builds()) + ": " + ec.message());
    if (plan.seal_bundled && !ensure_sealed(builds, payload, error)) return failed("Parties cannot be installed: " + error);
    if (!ensure_stub(o.install, payload.stub, plan.replace_stub, error)) return failed("Parties cannot be installed: " + error);
    auto good = apply_state(plan, loaded, builds, store, bundled.id);
    if (!good) return failed("Parties cannot save its install state: " + good.error());
    if (*good && !write_uninstall_entry(o.install, **good, error)) return failed(error);
    if (!write_shortcuts(o.install, error)) return failed(error);

    const std::string version = payload.entry.version.to_string();
    if (std::holds_alternative<Adopt>(plan.state)) return {kDone, "Parties " + version + " is installed."};
    if (std::holds_alternative<Stage>(plan.state))
        return {kDone, "Parties " + version + " is ready and starts the next time you open Parties."};
    return {kDone, "Parties is repaired. Your data was not changed."};
}

// Starts the installed app the way the Start menu shortcut does.
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
    const fs::path copy = dir / kLauncherFile;
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

// --- Native UI ---

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

// Only options before "--": what follows belongs to the app.
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
    case SetupOptions::Mode::Install: code = payload ? run_install(*options, *payload) : kFailed; break;
    }
    if (SUCCEEDED(com)) CoUninitialize();
    return code;
}

} // namespace parties::update::launcher
