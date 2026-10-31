// End to end: PartiesSetup.exe (PartiesLauncher.exe packed with a payload)
// installing, repairing and uninstalling fake builds in a throwaway install
// root and data root. The Apps entry, the Start menu folder and the Recycle
// Bin are redirected into the test's own HKCU key, folder and log, so the
// real ones are never touched. Every process the test starts runs in a job
// object, and "done" means the job has no process left.
//
// usage: update_setup_test --launcher <exe> --stub <exe> --fake-app <exe> [--only <name>]
//        update_setup_test --launcher <exe> --stub <exe> --fake-app <exe> --real-client <exe>

#include "build_info.h"
#include "build_store.h"
#include "file_io.h"

#include <client/settings_schema.h>
#include <parties/crypto.h>
#include <parties/update/install_layout.h>
#include <parties/update/manifest.h>
#include <parties/update/state_codec.h>

#include <sqlite3.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <propkey.h>
#include <propvarutil.h>

namespace fs = std::filesystem;
using namespace parties::update;
using namespace parties::update::launcher;
using namespace std::chrono_literals;

namespace {

// Matches the harness key pinned in testing launchers (updater/launcher/src/testing.cpp).
constexpr std::string_view kTestSeed = "f769e71268cc70210831656bcc920641326a5bd43fe4a015881f3515a8f908a8";
// One policy second is 10 ms: health deadline 2 s, confirm 6 s, clean exit after 0.3 s.
constexpr const char* kTiming = "10,200,600,30";

fs::path g_launcher, g_stub, g_fake_app;
std::string g_scenario;
int g_failures = 0;

bool check(bool ok, const std::string& what) {
    if (!ok) {
        std::cerr << "FAILED [" << g_scenario << "]: " << what << "\n";
        ++g_failures;
    }
    return ok;
}

std::wstring widen(std::string_view s) { return fs::path(std::u8string(s.begin(), s.end())).wstring(); }

std::string narrow(const std::wstring& w) { return utf8(fs::path(w)); }

SigningKey test_key() {
    SigningKey k;
    k.key_id = "parties-e2e-test";
    k.secret_key = Sha256::from_hex(kTestSeed)->bytes;
    if (!parties::derive_pubkey(k.secret_key, k.public_key)) std::abort();
    return k;
}

std::string bytes_of(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void write_bytes(const fs::path& file, const std::string& bytes) {
    std::ofstream(file, std::ios::binary | std::ios::trunc) << bytes;
}

// Every file under `dir` with its digest, keyed by relative path.
std::map<std::string, std::string> tree(const fs::path& dir) {
    std::map<std::string, std::string> out;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        if (!it->is_regular_file()) continue;
        auto digest = digest_file(it->path());
        out[utf8(fs::relative(it->path(), dir))] = digest ? digest->sha256.to_hex() : "unreadable";
    }
    return out;
}

// A process tree in its own job object.
class Run {
public:
    Run(const fs::path& exe, const std::vector<std::wstring>& args, const std::map<std::wstring, std::wstring>& env) {
        job_ = CreateJobObjectW(nullptr, nullptr);
        std::wstring cmd = L"\"" + exe.wstring() + L"\"";
        for (const auto& a : args) cmd += L" " + quote(a);
        std::wstring block;
        for (const auto& [k, v] : env) block += k + L"=" + v + L'\0';
        block += L'\0';
        STARTUPINFOW si{sizeof si};
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE,
                            CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT, block.data(), exe.parent_path().c_str(),
                            &si, &pi)) {
            check(false, "cannot start " + utf8(exe));
            return;
        }
        AssignProcessToJobObject(job_, pi.hProcess);
        ResumeThread(pi.hThread);
        CloseHandle(pi.hThread);
        process_ = pi.hProcess;
    }
    ~Run() {
        if (process_) CloseHandle(process_);
        if (job_) {
            TerminateJobObject(job_, 1);
            CloseHandle(job_);
        }
    }
    Run(const Run&) = delete;
    Run& operator=(const Run&) = delete;

    // The exit code of the first process, once every process in the job is gone.
    std::optional<DWORD> finish(std::chrono::milliseconds timeout = 90s) {
        if (!process_ || !wait_empty(timeout)) {
            check(false, "processes still running after " + std::to_string(timeout.count()) + " ms");
            return std::nullopt;
        }
        DWORD code = 0;
        GetExitCodeProcess(process_, &code);
        return code;
    }

    std::optional<DWORD> exit_code(std::chrono::milliseconds timeout = 60s) const {
        if (!process_ || WaitForSingleObject(process_, static_cast<DWORD>(timeout.count())) != WAIT_OBJECT_0)
            return std::nullopt;
        DWORD code = 0;
        GetExitCodeProcess(process_, &code);
        return code;
    }

    bool wait_empty(std::chrono::milliseconds timeout) const {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            JOBOBJECT_BASIC_ACCOUNTING_INFORMATION info{};
            QueryInformationJobObject(job_, JobObjectBasicAccountingInformation, &info, sizeof info, nullptr);
            if (info.ActiveProcesses == 0) return true;
            if (std::chrono::steady_clock::now() > deadline) return false;
            std::this_thread::sleep_for(20ms);
        }
    }

    std::vector<DWORD> pids() const {
        struct {
            JOBOBJECT_BASIC_PROCESS_ID_LIST list;
            ULONG_PTR more[63];
        } ids{};
        ids.list.NumberOfAssignedProcesses = 64;
        QueryInformationJobObject(job_, JobObjectBasicProcessIdList, &ids, sizeof ids, nullptr);
        std::vector<DWORD> out;
        for (DWORD i = 0; i < ids.list.NumberOfProcessIdsInList; ++i)
            out.push_back(static_cast<DWORD>(ids.list.ProcessIdList[i]));
        return out;
    }

    static std::wstring quote(const std::wstring& a) {
        if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
        std::wstring out = L"\"";
        size_t slashes = 0;
        for (wchar_t c : a) {
            if (c == L'\\') {
                ++slashes;
                continue;
            }
            out.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
            slashes = 0;
            out += c;
        }
        out.append(slashes * 2, L'\\');
        return out + L'"';
    }

private:
    HANDLE job_ = nullptr;
    HANDLE process_ = nullptr;
};

struct Build {
    BuildEntry entry;
    fs::path dir;  // the build's files as released
    BuildId id() const { return entry.id(); }
};

FileEntry file_entry(const fs::path& file) {
    auto digest = digest_file(file);
    if (!digest) std::abort();
    return {file.filename().string(), digest->size, digest->sha256, {"https://example.invalid/" + digest->sha256.to_hex()}};
}

class World {
public:
    explicit World(const std::string& name) {
        static int counter = 0;
        const std::string unique = std::to_string(GetCurrentProcessId()) + "-" + std::to_string(counter++);
        root_ = fs::temp_directory_path() / ("parties-setup-" + unique + "-" + name);
        fs::remove_all(root_);
        fs::create_directories(root_);
        install = root_ / "Programs" / "Parties";
        data = root_ / "Parties";
        start_menu = root_ / "Start Menu";
        recycle_log = root_ / "recycled.txt";
        key_parent = L"Software\\PartiesTest\\Setup-" + widen(unique);
        key = key_parent + L"\\Uninstall\\Parties";
        ns = L"PartiesSetupTest" + widen(unique);
    }
    ~World() {
        RegDeleteTreeW(HKEY_CURRENT_USER, key_parent.c_str());
        RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\PartiesTest");  // fails while another run still uses it
        std::error_code ec;
        fs::remove_all(root_, ec);
    }

    fs::path root_, install, data, start_menu, recycle_log;
    std::wstring key_parent, key, ns;
    std::map<std::string, std::string> extra_env{{"PARTIES_FAKE_RUN_MS", "700"}};

    Build build(const std::string& version, const fs::path& app = g_fake_app, bool fake = true,
                uint32_t schema = 1) const {
        Build b;
        b.dir = root_ / "releases" / version;
        fs::create_directories(b.dir);
        fs::copy_file(g_launcher, b.dir / kLauncherFile, fs::copy_options::overwrite_existing);
        fs::copy_file(app, b.dir / kAppFile, fs::copy_options::overwrite_existing);
        b.entry.version = *Version::parse(version);
        b.entry.data_schema = schema;
        b.entry.data_compat_floor = schema;
        b.entry.files = {file_entry(b.dir / kLauncherFile), file_entry(b.dir / kAppFile)};
        if (fake) {
            std::ofstream(b.dir / "fake-build.json") << "{\"version\":\"" << version << "\",\"data_schema\":" << schema
                                                     << ",\"data_compat_floor\":" << schema << "}";
            b.entry.files.push_back(file_entry(b.dir / "fake-build.json"));
        }
        return b;
    }

    // PartiesSetup.exe for `b`, packed by the launcher's own --pack mode.
    fs::path setup_for(const Build& b) const {
        const fs::path payload = root_ / "payloads" / b.entry.version.to_string();
        fs::create_directories(payload);
        for (const FileEntry& f : b.entry.files)
            fs::copy_file(b.dir / f.name, payload / f.name, fs::copy_options::overwrite_existing);
        fs::copy_file(g_stub, payload / "Parties.exe", fs::copy_options::overwrite_existing);
        ManifestPayload p;
        p.channel = Channel::Beta;
        p.serial = 1;
        p.issued_at = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()) - 60s;
        p.expires_at = p.issued_at + std::chrono::days(45);
        p.builds = {b.entry};
        const std::string text = write_payload(p);
        SigningKey key = test_key();
        auto envelope = sign_manifest({reinterpret_cast<const uint8_t*>(text.data()), text.size()}, {&key, 1});
        if (!envelope) std::abort();
        write_bytes(payload / "manifest.json", *envelope);
        const fs::path out = root_ / ("PartiesSetup-" + b.entry.version.to_string() + ".exe");
        Run pack(g_launcher, {L"--pack=" + payload.wstring(), L"--out=" + out.wstring()}, env());
        check(pack.finish() == 0u, "--pack failed for " + b.entry.version.to_string());
        return out;
    }

    std::map<std::wstring, std::wstring> env() const {
        std::map<std::wstring, std::wstring> out;
        for (wchar_t* block = GetEnvironmentStringsW(), *p = block; *p; p += wcslen(p) + 1) {
            std::wstring kv = p;
            size_t eq = kv.find(L'=', 1);
            if (eq != std::wstring::npos) out[kv.substr(0, eq)] = kv.substr(eq + 1);
        }
        out[L"PARTIES_TEST_UNINSTALL_KEY"] = key;
        out[L"PARTIES_TEST_START_MENU"] = start_menu.wstring();
        out[L"PARTIES_TEST_RECYCLE_LOG"] = recycle_log.wstring();
        out[L"PARTIES_TEST_TIMING"] = widen(kTiming);
        for (const auto& [k, v] : extra_env) out[widen(k)] = widen(v);
        return out;
    }

    std::vector<std::wstring> roots() const {
        return {L"--install-root=" + install.wstring(), L"--data-root=" + data.wstring(), L"--test-namespace=" + ns};
    }

    std::unique_ptr<Run> start_setup(const fs::path& setup, std::vector<std::wstring> args = {}) const {
        args.insert(args.begin(), L"--silent");
        auto r = roots();
        args.insert(args.end(), r.begin(), r.end());
        return std::make_unique<Run>(setup, args, env());
    }
    std::optional<DWORD> setup(const fs::path& setup, std::vector<std::wstring> args = {}) const {
        return start_setup(setup, std::move(args))->finish();
    }

    // What the Start menu shortcut does, plus the test's own roots.
    std::optional<DWORD> start_via(const fs::path& target) const {
        Run run(target, {L"--data-root=" + data.wstring(), L"--test-namespace=" + ns}, env());
        return run.finish();
    }

    std::optional<SupervisorState> state() const {
        auto text = read_file(install / "state" / "supervisor.json");
        if (!text) return std::nullopt;
        auto parsed = parse_state(*text);
        return parsed ? std::optional(*parsed) : std::nullopt;
    }
    std::string log() const { return read_file(data / "logs" / "launcher.log").value_or(""); }
    fs::path db() const { return data / "data" / "parties_client.db"; }

    std::optional<std::wstring> reg_string(const wchar_t* name) const {
        wchar_t buffer[4096]{};
        DWORD size = sizeof buffer - sizeof(wchar_t);
        if (RegGetValueW(HKEY_CURRENT_USER, key.c_str(), name, RRF_RT_REG_SZ, nullptr, buffer, &size) != ERROR_SUCCESS)
            return std::nullopt;
        return buffer;
    }
    std::optional<DWORD> reg_dword(const wchar_t* name) const {
        DWORD value = 0, size = sizeof value;
        if (RegGetValueW(HKEY_CURRENT_USER, key.c_str(), name, RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS)
            return std::nullopt;
        return value;
    }
    bool entry_exists() const {
        HKEY k = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
        RegCloseKey(k);
        return true;
    }
};

struct Link {
    std::wstring target, arguments, aumid;
};

std::optional<Link> read_link(const fs::path& file, bool resolve = false) {
    IShellLinkW* link = nullptr;
    IPersistFile* persist = nullptr;
    IPropertyStore* props = nullptr;
    std::optional<Link> out;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))) &&
        SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&persist))) && SUCCEEDED(persist->Load(file.c_str(), STGM_READ)) &&
        (!resolve || SUCCEEDED(link->Resolve(nullptr, SLR_NO_UI | SLR_NOSEARCH | SLR_NOUPDATE)))) {
        wchar_t buffer[4096]{};
        Link l;
        if (SUCCEEDED(link->GetPath(buffer, 4096, nullptr, 0))) l.target = buffer;
        if (SUCCEEDED(link->GetArguments(buffer, 4096))) l.arguments = buffer;
        if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&props)))) {
            PROPVARIANT v;
            PropVariantInit(&v);
            if (SUCCEEDED(props->GetValue(PKEY_AppUserModel_ID, &v)) && v.vt == VT_LPWSTR) l.aumid = v.pwszVal;
            PropVariantClear(&v);
            props->Release();
        }
        out = l;
    }
    if (persist) persist->Release();
    if (link) link->Release();
    return out;
}

// The pin the taskbar makes from a window: target and AppUserModelID come
// from the window's relaunch properties.
bool make_pin(const fs::path& file, const std::wstring& command, const std::wstring& aumid) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(command.c_str(), &argc);
    if (!argv || argc < 1) return false;
    std::wstring target = argv[0];
    LocalFree(argv);
    IShellLinkW* link = nullptr;
    IPropertyStore* props = nullptr;
    IPersistFile* persist = nullptr;
    PROPVARIANT id;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link));
    if (SUCCEEDED(hr)) hr = link->SetPath(target.c_str());
    if (SUCCEEDED(hr)) hr = link->QueryInterface(IID_PPV_ARGS(&props));
    if (SUCCEEDED(hr)) hr = InitPropVariantFromString(aumid.c_str(), &id);
    if (SUCCEEDED(hr)) hr = props->SetValue(PKEY_AppUserModel_ID, id);
    if (SUCCEEDED(hr)) hr = props->Commit();
    if (SUCCEEDED(hr)) hr = link->QueryInterface(IID_PPV_ARGS(&persist));
    if (SUCCEEDED(hr)) hr = persist->Save(file.c_str(), TRUE);
    if (persist) persist->Release();
    if (props) props->Release();
    if (link) link->Release();
    return SUCCEEDED(hr);
}

struct WindowIdentity {
    std::wstring aumid, relaunch_command, relaunch_name, relaunch_icon;
};

std::optional<WindowIdentity> identity_of(HWND hwnd) {
    IPropertyStore* store = nullptr;
    if (FAILED(SHGetPropertyStoreForWindow(hwnd, IID_PPV_ARGS(&store)))) return std::nullopt;
    auto get = [&](const PROPERTYKEY& key) {
        PROPVARIANT v;
        PropVariantInit(&v);
        std::wstring out;
        if (SUCCEEDED(store->GetValue(key, &v)) && v.vt == VT_LPWSTR) out = v.pwszVal;
        PropVariantClear(&v);
        return out;
    };
    WindowIdentity id{get(PKEY_AppUserModel_ID), get(PKEY_AppUserModel_RelaunchCommand),
                      get(PKEY_AppUserModel_RelaunchDisplayNameResource), get(PKEY_AppUserModel_RelaunchIconResource)};
    store->Release();
    return id;
}

// The first top-level window owned by one of `pids` whose identity is set.
std::optional<WindowIdentity> wait_for_window_identity(const Run& run, std::chrono::milliseconds timeout) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        struct Search {
            std::vector<DWORD> pids;
            std::optional<WindowIdentity> found;
        } search{run.pids(), std::nullopt};
        EnumWindows(
            [](HWND hwnd, LPARAM data) -> BOOL {
                auto* s = reinterpret_cast<Search*>(data);
                DWORD pid = 0;
                GetWindowThreadProcessId(hwnd, &pid);
                if (std::ranges::find(s->pids, pid) == s->pids.end()) return TRUE;
                auto id = identity_of(hwnd);
                if (!id || id->aumid.empty()) return TRUE;
                s->found = id;
                return FALSE;
            },
            reinterpret_cast<LPARAM>(&search));
        if (search.found) return search.found;
        std::this_thread::sleep_for(50ms);
    }
    return std::nullopt;
}

bool wait_for_log(const World& w, std::string_view text, std::chrono::milliseconds timeout) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (w.log().find(text) != std::string::npos) return true;
        std::this_thread::sleep_for(50ms);
    }
    return false;
}

long long count_rows(const fs::path& db, const std::string& sql) {
    sqlite3* h = nullptr;
    std::string uri = "file:" + utf8(db) + "?mode=ro";
    std::ranges::replace(uri, '\\', '/');
    long long n = -1;
    if (sqlite3_open_v2(uri.c_str(), &h, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, nullptr) == SQLITE_OK) {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(h, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW)
            n = sqlite3_column_int64(stmt, 0);
        sqlite3_finalize(stmt);
    }
    sqlite3_close(h);
    return n;
}

long long markers(const World& w, const std::string& version) {
    return count_rows(w.db(), "SELECT count(*) FROM markers WHERE build = '" + version + "'");
}

std::wstring expected_uninstall(const World& w, const Build& b) {
    return L"\"" + (w.install / "builds" / b.id().dir_name() / kLauncherFile).wstring() +
           L"\" --uninstall \"--install-root=" + w.install.wstring() + L"\"";
}

// Fresh install of `b` that starts the app through the stub, as a user's first run does.
void install_and_run(const World& w, const Build& b) {
    check(w.setup(w.setup_for(b)) == 0u, "setup " + b.entry.version.to_string() + " exited 0");
}

void uninstall(const World& w, std::vector<std::wstring> extra) {
    auto command = w.reg_string(L"UninstallString");
    if (!check(command.has_value(), "the Apps entry has an UninstallString")) return;
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(command->c_str(), &argc);
    std::vector<std::wstring> args(argv + 1, argv + argc);
    fs::path exe = argv[0];
    LocalFree(argv);
    args.push_back(L"--silent");
    args.push_back(L"--data-root=" + w.data.wstring());
    args.push_back(L"--test-namespace=" + w.ns);
    args.insert(args.end(), extra.begin(), extra.end());
    Run run(exe, args, w.env());
    check(run.finish() == 0u, "the UninstallString command exited 0");
}

// --- Scenarios ---

void fresh_install() {
    World w("fresh");
    Build a = w.build("1.0.0.0");
    install_and_run(w, a);
    check(bytes_of(w.install / kStubFile) == bytes_of(g_stub), "Parties.exe at the install root is the bundled stub");
    const fs::path dir = w.install / "builds" / a.id().dir_name();
    check(BuildStore(InstallPaths{w.install}).adopt(dir).has_value(), "builds\\<version>-<sha8> is sealed with build.json");
    auto s = w.state();
    check(s && s->good == a.id(), "state names the bundled build as good");
    check(s && s->find(a.id()) && std::holds_alternative<Proven>(s->find(a.id())->verdict), "good is proven");
    check(w.reg_string(L"DisplayName") == L"Parties", "DisplayName");
    check(w.reg_string(L"DisplayVersion") == L"1.0.0.0", "DisplayVersion is good's version");
    check(w.reg_string(L"Publisher").has_value(), "Publisher");
    check(w.reg_string(L"InstallLocation") == w.install.wstring(), "InstallLocation");
    check(w.reg_string(L"DisplayIcon") == (w.install / kStubFile).wstring() + L",0", "DisplayIcon is the stub");
    check(w.reg_dword(L"NoModify") == 1u && w.reg_dword(L"NoRepair") == 1u, "NoModify and NoRepair");
    check(w.reg_string(L"UninstallString") == expected_uninstall(w, a), "UninstallString runs good's launcher");
    auto main = read_link(w.start_menu / "Parties.lnk");
    auto recovery = read_link(w.start_menu / "Parties Recovery.lnk");
    check(main && fs::path(main->target) == w.install / kStubFile && main->arguments.empty() &&
              main->aumid == kAppUserModelId,
          "the Parties shortcut starts the stub with the app's AppUserModelID");
    check(recovery && fs::path(recovery->target) == w.install / kStubFile && recovery->arguments == L"--recover" &&
              recovery->aumid == kAppUserModelId,
          "the Parties Recovery shortcut starts the stub with --recover");
    check(fs::exists(w.install / "state" / "boot-record.json"), "the app was started through the stub");
    check(w.log().find("app reported healthy") != std::string::npos, "the app reported healthy to the launcher");
    check(markers(w, "1.0.0.0") == 1, "the app ran once on the data root");
    auto session = boot::parse_session_report(read_file(w.install / "state" / "launcher-session.json").value_or(""));
    check(session && session->ended_clean, "the launcher session ended clean");
}

void rerun_is_noop() {
    World w("rerun");
    Build a = w.build("1.0.0.0");
    const fs::path setup = w.setup_for(a);
    install_and_run(w, a);
    const auto install_before = tree(w.install);
    const auto data_before = tree(w.data);
    const auto menu_before = tree(w.start_menu);
    const auto state_before = read_file(w.install / "state" / "supervisor.json");
    const auto uninstall_before = w.reg_string(L"UninstallString");
    check(w.setup(setup, {L"--no-launch"}) == 0u, "the second setup exited 0");
    check(read_file(w.install / "state" / "supervisor.json") == state_before, "supervisor.json is byte for byte the same");
    check(tree(w.install) == install_before, "the install root holds the same files with the same bytes");
    check(tree(w.data) == data_before, "the data root is untouched");
    check(tree(w.start_menu) == menu_before, "the shortcuts are byte for byte the same");
    check(w.reg_string(L"UninstallString") == uninstall_before, "the Apps entry is the same");
}

void newer_is_staged() {
    World w("newer");
    Build a = w.build("1.0.0.0");
    Build b = w.build("2.0.0.0");
    install_and_run(w, a);
    const auto data_before = tree(w.data);
    check(w.setup(w.setup_for(b), {L"--no-launch"}) == 0u, "setup 2.0.0.0 exited 0");
    auto s = w.state();
    auto* staged = s ? std::get_if<Staged>(&s->activation) : nullptr;
    check(s && s->good == a.id(), "good stays 1.0.0.0");
    check(staged && staged->build == b.id(), "2.0.0.0 is staged, not made good");
    check(BuildStore(InstallPaths{w.install}).adopt(w.install / "builds" / b.id().dir_name()).has_value(),
          "2.0.0.0 is sealed");
    check(tree(w.data) == data_before, "the data root is untouched");
    check(w.reg_string(L"DisplayVersion") == L"1.0.0.0", "the Apps entry still names good");
    check(w.start_via(w.install / kStubFile) == 0u, "the stub exited 0");
    s = w.state();
    check(s && s->good == b.id() && s->previous_good == a.id(), "2.0.0.0 passed probation and 1.0.0.0 is kept");
    check(markers(w, "2.0.0.0") == 1, "2.0.0.0 ran once");
    check(w.reg_string(L"DisplayVersion") == L"2.0.0.0", "the launcher moved DisplayVersion to the new good");
    check(w.reg_string(L"UninstallString") == expected_uninstall(w, b), "the launcher moved UninstallString too");
}

void newer_staged_build_wins() {
    World w("staged");
    Build a = w.build("1.0.0.0");
    Build b = w.build("2.0.0.0");
    Build c = w.build("3.0.0.0");
    install_and_run(w, a);
    check(w.setup(w.setup_for(c), {L"--no-launch"}) == 0u, "setup 3.0.0.0 exited 0");
    const auto state_before = read_file(w.install / "state" / "supervisor.json");
    check(w.setup(w.setup_for(b), {L"--no-launch"}) == 0u, "setup 2.0.0.0 exited 0");
    check(read_file(w.install / "state" / "supervisor.json") == state_before, "the staged 3.0.0.0 stays staged");
    check(!fs::exists(w.install / "builds" / b.id().dir_name()), "2.0.0.0 was not written");
}

void older_keeps_good() {
    World w("older");
    Build a = w.build("1.0.0.0");
    Build b = w.build("2.0.0.0");
    install_and_run(w, b);
    const auto install_before = tree(w.install);
    const auto data_before = tree(w.data);
    check(w.setup(w.setup_for(a), {L"--no-launch"}) == 0u, "setup 1.0.0.0 exited 0");
    auto s = w.state();
    check(s && s->good == b.id() && std::holds_alternative<Steady>(s->activation), "2.0.0.0 stays good, nothing staged");
    check(tree(w.install) == install_before, "the install root is unchanged, so 1.0.0.0 was not written");
    check(tree(w.data) == data_before, "the data root is untouched");
    check(w.reg_string(L"DisplayVersion") == L"2.0.0.0", "the Apps entry still names 2.0.0.0");

    fs::remove(w.install / kStubFile);
    fs::remove(w.start_menu / "Parties.lnk");
    check(w.setup(w.setup_for(a), {L"--no-launch"}) == 0u, "setup 1.0.0.0 exited 0 again");
    check(fs::exists(w.install / kStubFile) && fs::exists(w.start_menu / "Parties.lnk"),
          "an older setup restores a missing stub and shortcut");
    check(w.state() == s, "and still leaves the state alone");
}

void tampered_payload_aborts() {
    for (const auto& [what, marker, offset] :
         std::vector<std::tuple<std::string, std::string, size_t>>{{"a build file", "{\"version\":\"1.0.0.0\"", 13},
                                                                   {"the manifest signature", "\"sig\":\"", 7}}) {
        World w("tampered");
        Build a = w.build("1.0.0.0");
        const fs::path setup = w.setup_for(a);
        std::string bytes = bytes_of(setup);
        size_t at = bytes.find(marker);
        if (!check(at != std::string::npos, "found " + what + " inside the setup")) continue;
        bytes[at + offset] = bytes[at + offset] == 'A' ? 'B' : 'A';
        write_bytes(setup, bytes);
        check(w.setup(setup) == 1u, "setup with a tampered " + what + " exits 1");
        check(!fs::exists(w.install.parent_path()) && !fs::exists(w.data), "nothing was written to either root");
        check(!w.entry_exists() && !fs::exists(w.start_menu), "no Apps entry and no shortcuts");
    }
}

void uninstall_keeps_data() {
    World w("uninstall");
    Build a = w.build("1.0.0.0");
    install_and_run(w, a);
    const auto data_before = tree(w.data);
    uninstall(w, {});
    check(!fs::exists(w.install), "the install root is gone");
    check(!w.entry_exists(), "the Apps entry is gone");
    check(!fs::exists(w.start_menu / "Parties.lnk") && !fs::exists(w.start_menu / "Parties Recovery.lnk"),
          "the shortcuts are gone");
    auto data_after = tree(w.data);
    std::vector<std::string> added;
    for (const auto& [path, digest] : data_after)
        if (!data_before.contains(path)) added.push_back(path);
    for (const auto& [path, digest] : data_before)
        check(data_after.contains(path) && data_after[path] == digest, path + " is byte for byte the same");
    check(added.size() == 1 && added[0].starts_with("backups\\pre-uninstall-") && added[0].ends_with(".db"),
          "the only new file in the data root is one pre-uninstall snapshot");
    if (added.size() == 1)
        check(count_rows(w.data / added[0], "SELECT count(*) FROM identity") == 1, "the snapshot holds the identity");
    check(!fs::exists(w.recycle_log), "nothing was sent to the Recycle Bin");
}

void uninstall_recycles_data_on_request() {
    World w("recycle");
    Build a = w.build("1.0.0.0");
    install_and_run(w, a);
    uninstall(w, {L"--remove-data"});
    check(!fs::exists(w.install), "the install root is gone");
    check(read_file(w.recycle_log) == utf8(w.data) + "\n", "exactly the data root went to the Recycle Bin call");
    check(fs::exists(w.data / "data" / "parties_client.db"), "nothing deleted the data root outside the shell call");
    bool snapshot = false;
    for (const auto& [path, digest] : tree(w.data)) snapshot |= path.starts_with("backups\\pre-uninstall-");
    check(snapshot, "the pre-uninstall snapshot was taken first");
}

void uninstall_stops_running_app() {
    World w("running");
    w.extra_env["PARTIES_FAKE_RUN_MS"] = "120000";
    Build a = w.build("1.0.0.0");
    auto running = w.start_setup(w.setup_for(a));
    check(running->exit_code() == 0u, "setup exited 0");
    check(wait_for_log(w, "app reported healthy", 30s), "the app is up");
    const auto started = std::chrono::steady_clock::now();
    uninstall(w, {});
    const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
    check(running->wait_empty(5s), "the launcher and the app have exited");
    check(took < 30s, "uninstall did not wait for the app's own 120 s run");
    const std::string log = w.log();
    check(log.find("asked to quit") != std::string::npos, "the launcher was asked to quit");
    check(log.find("app exited with code 0x0") != std::string::npos, "the app exited cleanly");
    check(!fs::exists(w.install), "the install root is gone");
    check(markers(w, "1.0.0.0") == 1, "the app's data is intact");
}

void pin_survives_gc() {
    World w("pin");
    w.extra_env["PARTIES_FAKE_RUN_MS"] = "3000";
    Build a = w.build("1.0.0.0");
    auto running = w.start_setup(w.setup_for(a));
    auto id = wait_for_window_identity(*running, 30s);
    if (!check(id.has_value(), "the app window carries a shell identity")) return;
    const std::wstring stub = (w.install / kStubFile).wstring();
    check(id->aumid == kAppUserModelId, "the window's AppUserModelID is the shortcuts' one");
    check(id->relaunch_command == L"\"" + stub + L"\"", "the window relaunches through the stub");
    check(id->relaunch_name == L"Parties" && id->relaunch_icon == stub + L",0", "relaunch name and icon");
    const fs::path pin = w.root_ / "pinned.lnk";
    check(make_pin(pin, id->relaunch_command, id->aumid), "a pin was made from the window");
    check(running->finish().has_value(), "the first run ended");

    for (const char* version : {"2.0.0.0", "3.0.0.0"}) {
        Build next = w.build(version);
        check(w.setup(w.setup_for(next), {L"--no-launch"}) == 0u, std::string("setup ") + version);
        check(w.start_via(w.install / kStubFile) == 0u, std::string("the stub ran ") + version);
    }
    check(w.start_via(w.install / kStubFile) == 0u, "one more start collects garbage");
    check(!fs::exists(w.install / "builds" / a.id().dir_name()), "the build the pin was made from is deleted");
    auto resolved = read_link(pin, true);
    check(resolved && fs::path(resolved->target) == w.install / kStubFile, "the pin still resolves to the stub");
    const long long runs_before = markers(w, "3.0.0.0");
    if (resolved) check(w.start_via(resolved->target) == 0u, "starting the pin exits 0");
    check(markers(w, "3.0.0.0") == runs_before + 1, "the pin started the current good build");
}

int real_client(const fs::path& client) {
    g_scenario = "real-client";
    World w("real");
    const auto dev_db = client.parent_path() / "parties_client.db";
    const auto dev_before = digest_file(dev_db);
    Build r = w.build("0.0.0.1", client, false, static_cast<uint32_t>(parties::client::schema::kCurrent.version));
    r.entry.data_compat_floor = static_cast<uint32_t>(parties::client::schema::kCurrent.compat_floor);
    check(w.setup(w.setup_for(r), {L"--no-launch"}) == 0u, "setup exited 0");
    w.extra_env["PARTIES_TEST_EXIT_AFTER_HEALTHY_MS"] = "8000";
    Run run(w.install / kStubFile,
            {L"--data-root=" + w.data.wstring(), L"--test-namespace=" + w.ns, L"--", L"--allow-multiple-instances"},
            w.env());
    auto id = wait_for_window_identity(run, 120s);
    check(run.finish(180s) == 0u, "the stub exited 0");
    const std::string log = w.log();
    check(log.find("app reported healthy") != std::string::npos, "the real client reported healthy");
    check(log.find("app exited with code 0x0") != std::string::npos, "the real client exited cleanly");
    check(fs::exists(w.db()), "the client used the temp data root");
    check(digest_file(dev_db) == dev_before, "the developer database next to the build is untouched");
    check(id && id->relaunch_command == L"\"" + (w.install / kStubFile).wstring() + L"\"" &&
              id->aumid == kAppUserModelId,
          "the client window relaunches through the stub");
    std::istringstream lines(log);
    for (std::string line; std::getline(lines, line);)
        if (line.find("adopted") != std::string::npos || line.find("healthy") != std::string::npos ||
            line.find("exited") != std::string::npos || line.find("starting") != std::string::npos)
            std::cout << "  " << line << "\n";
    if (id)
        std::cout << "  window AppUserModelID: " << narrow(id->aumid) << "\n  window RelaunchCommand: "
                  << narrow(id->relaunch_command) << "\n  window RelaunchIconResource: " << narrow(id->relaunch_icon)
                  << "\n";
    std::cout << "  install root: " << utf8(w.install) << "\n  data root: " << utf8(w.data) << "\n";
    std::cout << "real-client setup smoke: " << (g_failures ? "FAILED" : "pass") << "\n";
    return g_failures ? 1 : 0;
}

} // namespace

int main(int argc, char** argv) {
    std::optional<std::string> only;
    std::optional<fs::path> client;
    for (int i = 1; i + 1 < argc; ++i) {
        std::string a = argv[i];
        if (a == "--launcher") g_launcher = argv[++i];
        else if (a == "--stub") g_stub = argv[++i];
        else if (a == "--fake-app") g_fake_app = argv[++i];
        else if (a == "--real-client") client = argv[++i];
        else if (a == "--only") only = argv[++i];
    }
    if (g_launcher.empty() || g_stub.empty() || g_fake_app.empty()) {
        std::cerr << "usage: update_setup_test --launcher <exe> --stub <exe> --fake-app <exe> [--only <name>]\n";
        return 2;
    }
    if (!parties::crypto_init() || FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 2;
    if (client) return real_client(*client);

    const std::vector<std::pair<std::string, std::function<void()>>> scenarios{
        {"a fresh-install", fresh_install},
        {"b rerun-is-noop", rerun_is_noop},
        {"c newer-is-staged", newer_is_staged},
        {"c newer-staged-build-wins", newer_staged_build_wins},
        {"d older-keeps-good", older_keeps_good},
        {"e tampered-payload", tampered_payload_aborts},
        {"f uninstall-keeps-data", uninstall_keeps_data},
        {"g uninstall-recycles", uninstall_recycles_data_on_request},
        {"h uninstall-while-running", uninstall_stops_running_app},
        {"i pin-survives-gc", pin_survives_gc},
    };
    std::printf("%-28s %s\n", "scenario", "result");
    for (const auto& [name, run] : scenarios) {
        if (only && name.find(*only) == std::string::npos) continue;
        g_scenario = name;
        const int before = g_failures;
        const auto started = std::chrono::steady_clock::now();
        run();
        std::printf("%-28s %s %.1fs\n", name.c_str(), g_failures == before ? "pass" : "FAIL",
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
        std::fflush(stdout);
    }
    return g_failures ? 1 : 0;
}
