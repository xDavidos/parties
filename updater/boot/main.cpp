// Parties.exe, the boot stub. Setup installs it and automatic updates never
// replace it, so it stays small and links only what
// tools/update/check_boot_stub.ps1 allows.

#include <parties/update/boot.h>

#include <windows.h>
#include <bcrypt.h>
#include <commctrl.h>
#include <shellapi.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace boot = parties::update::boot;
using parties::update::Sha256;

namespace {

constexpr DWORD kBootWaitMs = 300'000;
constexpr int kMaxReChooses = 2;
constexpr LONGLONG kMaxStateFile = 1 << 20;
constexpr wchar_t kReleasesUrl[] = L"https://github.com/emcifuntik/parties/releases/latest";

enum Button { kSafeMode = 101, kDownload, kDataFolder };

struct Options {
    std::wstring root;  // install root, no trailing separator
    bool recover = false;
    std::vector<std::wstring> forward;
    std::wstring objects = L"Local\\Parties.";
    DWORD boot_wait_ms = kBootWaitMs;
    int forced_button = 0;  // testing builds: the recovery button, chosen without the dialog
    std::vector<std::wstring> launcher_tests;  // testing builds: --test-namespace and --data-root, passed on
};

struct Handle {
    HANDLE h = nullptr;
    Handle() = default;
    explicit Handle(HANDLE v) : h(v == INVALID_HANDLE_VALUE ? nullptr : v) {}
    Handle(Handle&& o) noexcept : h(std::exchange(o.h, nullptr)) {}
    Handle& operator=(Handle&& o) noexcept {
        std::swap(h, o.h);
        return *this;
    }
    ~Handle() {
        if (h) CloseHandle(h);
    }
    explicit operator bool() const { return h != nullptr; }
};

std::wstring widen(std::string_view s) {
    std::wstring w(s.size(), L'\0');
    w.resize(MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), int(w.size())));
    return w;
}

std::wstring full_path(const std::wstring& path) {
    std::wstring out(32768, L'\0');
    out.resize(GetFullPathNameW(path.c_str(), DWORD(out.size()), out.data(), nullptr));
    while (out.size() > 3 && (out.back() == L'\\' || out.back() == L'/')) out.pop_back();
    return out;
}

Options parse_options() {
    Options o;
    std::wstring self(32768, L'\0');
    self.resize(GetModuleFileNameW(nullptr, self.data(), DWORD(self.size())));
    o.root = self.substr(0, self.find_last_of(L'\\'));
    o.recover = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool passthrough = false;
    for (int i = 1; argv && i < argc; ++i) {
        std::wstring_view a = argv[i];
        auto value = [&](std::wstring_view name) {
            return a.starts_with(name) ? std::optional(std::wstring(a.substr(name.size()))) : std::nullopt;
        };
        if (passthrough) o.forward.emplace_back(a);
        else if (a == L"--") passthrough = true;
        else if (a == L"--recover") o.recover = true;
        else if (auto root = value(L"--install-root=")) o.root = *root;
#ifdef PARTIES_UPDATE_TESTING
        else if (auto ns = value(L"--test-namespace=")) {
            o.objects = L"Local\\" + *ns + L".";
            o.launcher_tests.emplace_back(a);
        } else if (a.starts_with(L"--data-root=")) o.launcher_tests.emplace_back(a);
        else if (auto ms = value(L"--test-boot-wait-ms=")) o.boot_wait_ms = wcstoul(ms->c_str(), nullptr, 10);
        else if (auto b = value(L"--test-recovery="))
            o.forced_button = *b == L"safe-mode" ? kSafeMode : IDCLOSE;
#endif
        else o.forward.emplace_back(a);
    }
    LocalFree(argv);
    o.root = full_path(o.root);
    return o;
}

// Quotes one argument so CommandLineToArgvW and the CRT read it back verbatim:
// backslashes are literal unless a quote follows them.
void append_arg(std::wstring& cmd, std::wstring_view arg) {
    if (!cmd.empty()) cmd += L' ';
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring_view::npos) {
        cmd += arg;
        return;
    }
    cmd += L'"';
    size_t slashes = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') {
            ++slashes;
            continue;
        }
        cmd.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        slashes = 0;
        cmd += c;
    }
    cmd.append(slashes * 2, L'\\');
    cmd += L'"';
}

std::optional<std::string> read_small(const std::wstring& path) {
    Handle f(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, OPEN_EXISTING, 0, nullptr));
    LARGE_INTEGER size{};
    if (!f || !GetFileSizeEx(f.h, &size) || size.QuadPart > kMaxStateFile) return std::nullopt;
    std::string text(size_t(size.QuadPart), '\0');
    DWORD got = 0;
    if (!ReadFile(f.h, text.data(), DWORD(text.size()), &got, nullptr) || got != text.size()) return std::nullopt;
    return text;
}

template <class Parse>
auto load(const std::wstring& path, Parse parse) -> decltype(parse(std::string_view{})) {
    auto text = read_small(path);
    if (!text) return std::nullopt;
    return parse(*text);
}

// The only file the stub writes. A failed write is not fatal: starting Parties
// matters more than the bookkeeping.
void persist(const std::wstring& state, const boot::BootRecord& record) {
    std::string text = boot::write_boot_record(record);
    std::wstring path = state + L"\\boot-record.json", tmp = path + L".tmp";
    Handle f(CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!f) return;
    DWORD wrote = 0;
    bool ok = WriteFile(f.h, text.data(), DWORD(text.size()), &wrote, nullptr) && wrote == text.size() &&
              FlushFileBuffers(f.h);
    f = Handle{};
    if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        DeleteFileW(tmp.c_str());
}

// Returns a deny-write handle to the file only if its SHA-256 matches. Keep it
// open across CreateProcess so the bytes that were hashed are the bytes that run.
Handle open_verified(const std::wstring& path, const Sha256& expected) {
    Handle f(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                         FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (!f || !BCRYPT_SUCCESS(BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &hash, nullptr, 0, nullptr, 0, 0)))
        return {};
    std::vector<UCHAR> buf(1 << 16);
    DWORD got = 0;
    bool ok;
    while ((ok = ReadFile(f.h, buf.data(), DWORD(buf.size()), &got, nullptr)) && got > 0)
        if (!(ok = BCRYPT_SUCCESS(BCryptHashData(hash, buf.data(), got, 0)))) break;
    Sha256 actual;
    ok = ok && BCRYPT_SUCCESS(BCryptFinishHash(hash, actual.bytes.data(), ULONG(actual.bytes.size()), 0));
    BCryptDestroyHash(hash);
    return ok && actual == expected ? std::move(f) : Handle{};
}

// Only `inherit` crosses into the child, never a handle the stub itself inherited.
Handle spawn(const std::wstring& exe, std::wstring cmd, const std::wstring& cwd, HANDLE inherit) {
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<char> attrs(size);
    si.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrs.data());
    if (!InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &size)) return {};
    if (inherit)
        UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &inherit,
                                  sizeof(inherit), nullptr, nullptr);
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, inherit != nullptr,
                             EXTENDED_STARTUPINFO_PRESENT, nullptr, cwd.c_str(), &si.StartupInfo, &pi);
    DeleteProcThreadAttributeList(si.lpAttributeList);
    if (!ok) return {};
    CloseHandle(pi.hThread);
    return Handle(pi.hProcess);
}

// True once the launcher signals boot_ok. On exit or timeout first, the
// launcher is terminated, and its job object takes the app with it.
bool boot_launcher(const Options& o, const std::wstring& exe, const boot::RunLauncher& run, Handle lease) {
    SECURITY_ATTRIBUTES inheritable{sizeof(inheritable), nullptr, TRUE};
    Handle boot_ok(CreateEventW(&inheritable, TRUE, FALSE, nullptr));
    if (!boot_ok) return false;
    std::wstring cmd;
    append_arg(cmd, exe);
    append_arg(cmd, L"--install-root=" + o.root);
    append_arg(cmd, L"--boot-session=" + std::to_wstring(run.session));
    append_arg(cmd, L"--boot-ok-event=" + std::to_wstring(reinterpret_cast<uintptr_t>(boot_ok.h)));
    if (o.recover) append_arg(cmd, L"--recover");
    for (const auto& a : o.launcher_tests) append_arg(cmd, a);
    append_arg(cmd, L"--");
    for (const auto& a : o.forward) append_arg(cmd, a);
    Handle process = spawn(exe, std::move(cmd), o.root, boot_ok.h);
    lease = Handle{};
    if (!process) return false;
    HANDLE waits[] = {boot_ok.h, process.h};
    if (WaitForMultipleObjects(2, waits, FALSE, o.boot_wait_ms) == WAIT_OBJECT_0) return true;
    TerminateProcess(process.h, 1);
    WaitForSingleObject(process.h, 10'000);
    return false;
}

// Runs the last good app without a supervisor: no updates, same data directory.
bool start_safe_mode(const Options& o, const boot::BootContract& c) {
    std::wstring exe = o.root + L"\\" + widen(c.last_good_app.dir) + L"\\parties_client.exe";
    Handle lease = open_verified(exe, c.last_good_app.sha256);
    if (!lease) return false;
    std::wstring data = widen(c.data_dir), cmd;
    append_arg(cmd, exe);
    append_arg(cmd, L"--data-dir=" + data);
    for (const auto& a : o.forward) append_arg(cmd, a);
    DWORD attrs = GetFileAttributesW(data.c_str());
    bool data_exists = attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
    return bool(spawn(exe, std::move(cmd), data_exists ? data : o.root, nullptr));
}

std::wstring default_data_dir() {
    std::wstring base(32768, L'\0');
    base.resize(GetEnvironmentVariableW(L"LOCALAPPDATA", base.data(), DWORD(base.size())));
    return base + L"\\Parties\\data";
}

HRESULT CALLBACK on_dialog(HWND, UINT msg, WPARAM button, LPARAM, LONG_PTR data_dir) {
    if (msg != TDN_BUTTON_CLICKED || (button != kDownload && button != kDataFolder)) return S_OK;
    const wchar_t* target = button == kDownload ? kReleasesUrl : reinterpret_cast<std::wstring*>(data_dir)->c_str();
    ShellExecuteW(nullptr, L"open", target, nullptr, nullptr, SW_SHOWNORMAL);
    return S_FALSE;  // keep the dialog open
}

int ask(bool can_safe_mode, std::wstring& data_dir, const wchar_t* content) {
    const TASKDIALOG_BUTTON buttons[] = {
        {kSafeMode, L"Start Parties in safe mode\nRuns the last version that worked, without updates. "
                    L"Your data stays where it is."},
        {kDownload, L"Download the installer\nRunning it repairs Parties and keeps your data."},
        {kDataFolder, L"Open the data folder\nShows where your identity, servers and settings are kept."},
    };
    TASKDIALOGCONFIG cfg{sizeof(cfg)};
    cfg.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION;
    cfg.dwCommonButtons = TDCBF_CLOSE_BUTTON;
    cfg.pszWindowTitle = L"Parties";
    cfg.pszMainIcon = TD_WARNING_ICON;
    cfg.pszMainInstruction = L"Parties could not start";
    cfg.pszContent = content;
    cfg.pButtons = can_safe_mode ? buttons : buttons + 1;
    cfg.cButtons = can_safe_mode ? 3 : 2;
    cfg.pfCallback = on_dialog;
    cfg.lpCallbackData = reinterpret_cast<LONG_PTR>(&data_dir);
    int pressed = IDCLOSE;
    return SUCCEEDED(TaskDialogIndirect(&cfg, &pressed, nullptr, nullptr)) ? pressed : IDCLOSE;
}

// Exit code 0 when safe mode started, 1 when the user closed the dialog.
int recover(const Options& o, const std::optional<boot::BootContract>& c) {
#ifdef PARTIES_UPDATE_TESTING
    if (o.forced_button) return o.forced_button == kSafeMode && c && start_safe_mode(o, *c) ? 0 : 1;
#endif
    std::wstring data_dir = c ? widen(c->data_dir) : default_data_dir();
    const wchar_t* content = L"Parties could not find an installed version that starts. "
                             L"Your data is safe and has not been changed.";
    for (;;) {
        if (ask(c.has_value(), data_dir, content) != kSafeMode) return 1;
        if (start_safe_mode(o, *c)) return 0;
        content = L"Safe mode could not start because the app file is missing or damaged. "
                  L"Download the installer to repair Parties. Your data is not affected.";
    }
}

int run(const Options& o) {
    Handle running(OpenMutexW(SYNCHRONIZE, FALSE, (o.objects + L"Launcher.Running").c_str()));
    if (running || GetLastError() == ERROR_ACCESS_DENIED) {
        Handle activate(CreateEventW(nullptr, FALSE, FALSE, (o.objects + L"Launcher.Activate").c_str()));
        if (activate) SetEvent(activate.h);
        return 0;
    }
    // A second stub while one is booting would fold the live attempt as a failure.
    Handle booting(CreateMutexW(nullptr, TRUE, (o.objects + L"Boot").c_str()));
    if (!booting || GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    std::wstring state = o.root + L"\\state";
    auto contract = load(state + L"\\boot-contract.json", boot::parse_boot_contract);
    auto record = load(state + L"\\boot-record.json", boot::parse_boot_record).value_or(boot::BootRecord{});
    auto session = load(state + L"\\launcher-session.json", boot::parse_session_report);
    std::vector<Sha256> unusable;
    for (int failures = 0;;) {
        boot::Decision d = boot::choose(contract, record, session, unusable);
        const auto* launcher = std::get_if<boot::RunLauncher>(&d.choice);
        if (!launcher) {
            persist(state, d.next_record);
            return recover(o, contract);
        }
        std::wstring exe = o.root + L"\\" + widen(launcher->dir) + L"\\PartiesLauncher.exe";
        Handle lease = open_verified(exe, launcher->sha256);
        if (!lease) {
            // Choose again from the same inputs: a damaged file never ran, so it is not a failed boot.
            unusable.push_back(launcher->sha256);
            continue;
        }
        persist(state, d.next_record);
        if (boot_launcher(o, exe, *launcher, std::move(lease))) return 0;
        record = boot::record_boot_failure(d.next_record);
        session.reset();
        persist(state, record);
        if (++failures > kMaxReChooses) return recover(o, contract);
    }
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) { return run(parse_options()); }
