#include <parties/update/boot.h>

#include <windows.h>
#include <bcrypt.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace boot = parties::update::boot;
using parties::update::Sha256;

namespace {

fs::path g_stub, g_fake;
std::wstring g_namespace;
const char* g_scenario = "";
int g_failures = 0;

void check(bool ok, const std::string& what) {
    if (ok) return;
    ++g_failures;
    std::fprintf(stderr, "FAIL [%s] %s\n", g_scenario, what.c_str());
}

std::string read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), {}};
}

Sha256 sha256_of(const std::string& bytes) {
    Sha256 out;
    BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, (PUCHAR)bytes.data(), ULONG(bytes.size()), out.bytes.data(),
               ULONG(out.bytes.size()));
    return out;
}

Sha256 other_than(Sha256 s) {
    s.bytes[0] ^= 1;
    return s;
}

std::wstring quote(const std::wstring& arg) {
    std::wstring out = L"\"";
    size_t slashes = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') {
            ++slashes;
            continue;
        }
        out.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        slashes = 0;
        out += c;
    }
    out.append(slashes * 2, L'\\');
    return out + L"\"";
}

bool alive(DWORD pid) {
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!h) return false;
    bool running = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
    CloseHandle(h);
    return running;
}

struct Run {
    DWORD pid = 0;
    std::string exe;
    std::vector<std::string> args;
    bool has(const std::string& a) const { return std::ranges::find(args, a) != args.end(); }
    std::vector<std::string> after_separator() const {
        auto it = std::ranges::find(args, std::string("--"));
        return it == args.end() ? std::vector<std::string>{} : std::vector<std::string>(it + 1, args.end());
    }
    std::optional<std::string> value(const std::string& prefix) const {
        for (const auto& a : args)
            if (a.starts_with(prefix)) return a.substr(prefix.size());
        return std::nullopt;
    }
};

struct Build {
    std::string name;
    fs::path dir;
    Sha256 launcher, app;
    boot::LauncherRef launcher_ref() const { return {"builds\\" + name, launcher}; }
    boot::AppRef app_ref() const { return {"builds\\" + name, app}; }
};

using Snapshot = std::map<std::string, std::pair<uintmax_t, fs::file_time_type>>;

class Root {
public:
    fs::path path;
    fs::path data;

    explicit Root(const char* scenario) {
        g_scenario = scenario;
        path = fs::temp_directory_path() / "parties stub test" / scenario;
        fs::remove_all(path);
        fs::create_directories(path / "state");
        data = fs::temp_directory_path() / "parties stub test" / (std::string(scenario) + " data");
        fs::remove_all(data);
        fs::create_directories(data);
        fs::copy_file(g_stub, path / "Parties.exe");
    }
    ~Root() { settle(); }

    Build build(const std::string& name, const std::string& behavior) {
        Build b{name, path / "builds" / name};
        fs::create_directories(b.dir);
        b.launcher = place(b.dir / "PartiesLauncher.exe", name + " launcher");
        b.app = place(b.dir / "parties_client.exe", name + " app");
        set_behavior(b, behavior);
        return b;
    }

    void set_behavior(const Build& b, const std::string& behavior) {
        std::ofstream(b.dir / "behavior.txt") << behavior << ' ' << b.launcher.to_hex();
        behaviors_[b.dir.string()] = behavior;
    }

    boot::BootContract contract(const Build& good, const Build* previous, const Build& app) const {
        return {good.launcher_ref(), previous ? std::optional(previous->launcher_ref()) : std::nullopt,
                app.app_ref(), data.string()};
    }
    void write_contract(const boot::BootContract& c) {
        std::ofstream(path / "state" / "boot-contract.json", std::ios::binary) << boot::write_boot_contract(c);
    }

    std::optional<boot::BootRecord> record() const {
        return boot::parse_boot_record(read_file(path / "state" / "boot-record.json"));
    }
    std::optional<boot::LauncherHealth> health(const Sha256& launcher) const {
        auto rec = record();
        if (!rec) return std::nullopt;
        for (const auto& h : rec->launchers)
            if (h.launcher == launcher) return h;
        return std::nullopt;
    }

    std::vector<Run> runs(const Build& b, const std::string& exe = "PartiesLauncher.exe") const {
        std::vector<Run> out;
        std::istringstream log(read_file(b.dir / "runs.log"));
        for (std::string line; std::getline(log, line);) {
            std::vector<std::string> parts;
            std::istringstream fields(line);
            for (std::string f; std::getline(fields, f, '|');) parts.push_back(f);
            if (parts.size() < 2 || parts[1] != exe) continue;
            out.push_back({DWORD(std::stoul(parts[0])), parts[1], {parts.begin() + 2, parts.end()}});
        }
        return out;
    }

    // Safe mode spawns the app and exits, so the app's line can land after the stub is gone.
    std::vector<Run> wait_for_runs(const Build& b, const std::string& exe, size_t count) const {
        auto out = runs(b, exe);
        for (int i = 0; i < 200 && out.size() < count; ++i) {
            Sleep(50);
            out = runs(b, exe);
        }
        return out;
    }

    HANDLE launch(std::vector<std::wstring> args, const fs::path& exe = {}) {
        if (std::ranges::none_of(args, [](auto& a) { return a.starts_with(L"--test-recovery="); }))
            args.push_back(L"--test-recovery=close");
        args.push_back(L"--test-namespace=" + g_namespace);
        fs::path program = exe.empty() ? path / "Parties.exe" : exe;
        std::wstring cmd = quote(program.wstring());
        for (const auto& a : args) cmd += L" " + quote(a);
        STARTUPINFOW si{sizeof(si)};
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(program.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
            check(false, "stub process starts");
            return nullptr;
        }
        CloseHandle(pi.hThread);
        return pi.hProcess;
    }

    int start(std::vector<std::wstring> args, const std::function<void()>& while_live = {},
              const fs::path& exe = {}) {
        Snapshot before = snapshot();
        HANDLE stub = launch(std::move(args), exe);
        if (!stub) return -1;
        DWORD code = DWORD(-1);
        if (WaitForSingleObject(stub, 60'000) != WAIT_OBJECT_0) {
            check(false, "stub exits within 60 s");
            TerminateProcess(stub, 99);
        }
        GetExitCodeProcess(stub, &code);
        CloseHandle(stub);
        if (while_live) while_live();
        settle();
        for (const auto& [name, stat] : snapshot()) {
            if (before.contains(name) && before[name] == stat) continue;
            bool allowed = name == "state\\boot-record.json" || name == "state\\launcher-session.json" ||
                           name.ends_with("\\runs.log");
            check(allowed, "stub wrote only its record, not " + name);
        }
        return int(code);
    }

    void settle() {
        for (const auto& [dir, behavior] : behaviors_) {
            Build b{"", dir};
            for (const auto& exe : {"PartiesLauncher.exe", "parties_client.exe"}) {
                for (const Run& r : runs(b, exe)) {
                    HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, r.pid);
                    if (!h) continue;
                    if (WaitForSingleObject(h, behavior == "clean" ? 10'000 : 0) == WAIT_TIMEOUT) {
                        TerminateProcess(h, 1);
                        WaitForSingleObject(h, 10'000);
                    }
                    CloseHandle(h);
                }
            }
        }
    }

private:
    std::map<std::string, std::string> behaviors_;

    static Sha256 place(const fs::path& to, const std::string& tag) {
        // Bytes after the PE image are ignored by the loader and give every copy its own digest.
        std::string bytes = read_file(g_fake) + tag;
        std::ofstream(to, std::ios::binary) << bytes;
        return sha256_of(bytes);
    }

    Snapshot snapshot() const {
        Snapshot out;
        for (const auto& e : fs::recursive_directory_iterator(path))
            if (e.is_regular_file())
                out[fs::relative(e.path(), path).string()] = {e.file_size(), e.last_write_time()};
        return out;
    }
};

void good_launcher_signals_boot_ok() {
    Root r("boot ok");
    Build good = r.build("1.0.0.2-aaaaaaaa", "ok");
    Build prev = r.build("1.0.0.1-bbbbbbbb", "ok");
    r.write_contract(r.contract(good, &prev, good));
    std::vector<std::wstring> user = {L"parties://join/abc", L"two words", L"C:\\trailing\\", L"say \"hi\""};
    int code = r.start(user, [&] {
        auto runs = r.runs(good);
        check(runs.size() == 1 && alive(runs[0].pid), "stub exits on boot_ok while the launcher keeps running");
    });
    check(code == 0, "stub exits 0 after boot_ok");
    auto runs = r.runs(good);
    check(runs.size() == 1, "good launcher ran once");
    check(r.runs(prev).empty(), "previous launcher did not run");
    if (runs.size() != 1) return;
    const Run& run = runs[0];
    auto rec = r.record();
    check(rec && rec->attempt && rec->attempt->launcher == good.launcher, "record holds the attempt for good");
    check(rec && rec->attempt && run.value("--boot-session=") == std::to_string(rec->attempt->session),
          "session id in the record is the one passed to the launcher");
    check(run.value("--install-root=") == r.path.string(), "install root resolved from the stub location");
    check(run.value("--boot-ok-event=").has_value(), "boot_ok event handle passed");
    check(!run.has("--recover"), "no --recover without Shift or --recover");
    check(run.after_separator() ==
              std::vector<std::string>{"parties://join/abc", "two words", "C:\\trailing\\", "say \"hi\""},
          "user args arrive verbatim after --");
}

void unconfirmed_launcher_failing_twice_falls_back() {
    Root r("fails within start");
    Build good = r.build("1.0.0.2-aaaaaaaa", "fail");
    Build prev = r.build("1.0.0.1-bbbbbbbb", "ok");
    r.write_contract(r.contract(good, &prev, good));
    check(r.start({}) == 0, "stub exits 0 once previous_good boots");
    check(r.runs(good).size() == 2, "good launcher tried twice");
    check(r.runs(prev).size() == 1, "previous launcher ran after the second failure");
    auto h = r.health(good.launcher);
    check(h && h->failed_boots == 2 && !h->confirmed, "record counts two failed boots for good");
}

void unclean_sessions_across_starts_fall_back() {
    Root r("fails across starts");
    Build good = r.build("1.0.0.2-aaaaaaaa", "ok");
    Build prev = r.build("1.0.0.1-bbbbbbbb", "ok");
    r.write_contract(r.contract(good, &prev, good));
    for (int start = 1; start <= 2; ++start) {
        check(r.start({}) == 0, "stub exits 0 on boot_ok");
        check(r.runs(prev).empty(), "previous launcher not yet used");
    }
    check(r.start({}) == 0, "third start exits 0");
    check(r.runs(good).size() == 2, "good launcher ran in the first two starts only");
    check(r.runs(prev).size() == 1, "third start runs previous_good after two unclean sessions");
}

void hash_mismatch_never_executes() {
    Root r("hash mismatch");
    Build good = r.build("1.0.0.2-aaaaaaaa", "ok");
    Build prev = r.build("1.0.0.1-bbbbbbbb", "ok");
    auto c = r.contract(good, &prev, good);
    c.good.sha256 = other_than(good.launcher);
    r.write_contract(c);
    check(r.start({}) == 0, "stub exits 0 after previous_good boots");
    check(r.runs(good).empty(), "launcher with a wrong digest never ran");
    check(r.runs(prev).size() == 1, "previous launcher ran instead");
    auto h = r.health(c.good.sha256);
    check(!h || h->failed_boots == 0, "a damaged file is not counted as a failed boot");

    fs::remove(good.dir / "PartiesLauncher.exe");
    r.write_contract(r.contract(good, &prev, good));
    check(r.start({}) == 0, "stub exits 0 with a missing good launcher");
    check(r.runs(prev).size() == 2, "previous launcher ran when good's file is missing");
}

void no_bootable_launcher_offers_safe_mode() {
    Root r("safe mode");
    Build good = r.build("1.0.0.2-aaaaaaaa", "fail");
    Build app = r.build("1.0.0.1-bbbbbbbb", "app");
    auto c = r.contract(good, nullptr, app);
    r.write_contract(c);
    check(r.start({L"--test-recovery=safe-mode", L"parties://join/x"}) == 0, "safe mode started");
    check(r.runs(good).size() == 2, "unconfirmed good tried twice before recovery");
    check(r.runs(app, "PartiesLauncher.exe").empty(), "no supervisor started for safe mode");
    auto apps = r.wait_for_runs(app, "parties_client.exe", 1);
    check(apps.size() == 1, "safe mode ran the last good app");
    if (apps.size() == 1)
        check(apps[0].args == std::vector<std::string>{"--data-dir=" + r.data.string(), "parties://join/x"},
              "safe mode passes --data-dir and the user args, and no --supervisor-pipe");

    c.last_good_app.sha256 = other_than(app.app);
    c.good.sha256 = other_than(good.launcher);
    r.write_contract(c);
    check(r.start({L"--test-recovery=safe-mode"}) == 1, "safe mode refuses an app with a wrong digest");
    check(r.runs(app, "parties_client.exe").size() == 1, "damaged app never ran");

    fs::remove(r.path / "state" / "boot-contract.json");
    check(r.start({}) == 1, "no contract leads to recovery");
    auto rec = r.record();
    check(rec && !rec->attempt, "no attempt recorded without a contract");
}

void running_launcher_is_activated() {
    Root r("activate");
    Build good = r.build("1.0.0.2-aaaaaaaa", "ok");
    r.write_contract(r.contract(good, nullptr, good));
    fs::path record = r.path / "state" / "boot-record.json";
    std::ofstream(record, std::ios::binary) << boot::write_boot_record({7, boot::Attempt{7, good.launcher}, {}});
    std::string bytes = read_file(record);
    auto mtime = fs::last_write_time(record);
    HANDLE mutex = CreateMutexW(nullptr, TRUE, (L"Local\\" + g_namespace + L".Launcher.Running").c_str());
    HANDLE activate = CreateEventW(nullptr, FALSE, FALSE, (L"Local\\" + g_namespace + L".Launcher.Activate").c_str());
    check(r.start({}) == 0, "stub exits 0 when a launcher runs");
    check(WaitForSingleObject(activate, 0) == WAIT_OBJECT_0, "activate event set");
    check(read_file(record) == bytes && fs::last_write_time(record) == mtime, "boot-record.json untouched");
    check(r.runs(good).empty(), "no launcher started");
    CloseHandle(activate);
    CloseHandle(mutex);
}

void recover_flag_reaches_launcher() {
    Root r("recover");
    Build good = r.build("1.0.0.2-aaaaaaaa", "ok");
    r.write_contract(r.contract(good, nullptr, good));
    check(r.start({L"--install-root=" + r.path.wstring(), L"--recover"}, {}, g_stub) == 0, "stub exits 0");
    auto runs = r.runs(good);
    check(runs.size() == 1 && runs[0].has("--recover"), "--recover passed to the launcher");
    check(runs.size() == 1 && runs[0].value("--install-root=") == r.path.string(),
          "--install-root overrides the stub location");
    check(runs.size() == 1 && runs[0].after_separator().empty(), "no user args after --");
}

void confirmed_launcher_does_not_fall_back() {
    Root r("confirmed");
    Build good = r.build("1.0.0.2-aaaaaaaa", "clean");
    Build prev = r.build("1.0.0.1-bbbbbbbb", "ok");
    r.write_contract(r.contract(good, &prev, good));
    check(r.start({}) == 0 && r.start({}) == 0, "two clean sessions");
    r.set_behavior(good, "fail");
    check(r.start({}) == 1, "confirmed launcher failing ends in recovery");
    auto h = r.health(good.launcher);
    check(h && h->confirmed && h->failed_boots == 0, "two clean sessions confirmed good");
    check(r.runs(good).size() == 5, "confirmed good retried twice after its first failure");
    check(r.runs(prev).empty(), "confirmed good never falls back to previous_good");
}

void hung_launcher_is_terminated() {
    Root r("hang");
    Build good = r.build("1.0.0.2-aaaaaaaa", "hang");
    Build prev = r.build("1.0.0.1-bbbbbbbb", "ok");
    r.write_contract(r.contract(good, &prev, good));
    check(r.start({L"--test-boot-wait-ms=1000"}, [&] {
        auto runs = r.runs(good);
        check(runs.size() == 2, "hung launcher tried twice");
        for (const Run& run : runs) check(!alive(run.pid), "stub terminated the hung launcher");
    }) == 0, "stub exits 0 once previous_good boots");
    check(r.runs(prev).size() == 1, "previous launcher ran after two timeouts");
}

void second_stub_leaves_a_booting_stub_alone() {
    Root r("second stub");
    Build good = r.build("1.0.0.2-aaaaaaaa", "hang");
    r.write_contract(r.contract(good, nullptr, good));
    HANDLE first = r.launch({L"--test-boot-wait-ms=30000"});
    for (int i = 0; i < 200 && r.runs(good).empty(); ++i) Sleep(50);
    std::string record = read_file(r.path / "state" / "boot-record.json");
    check(r.start({}, [&] {
        check(r.runs(good).size() == 1, "second stub started no launcher");
        check(read_file(r.path / "state" / "boot-record.json") == record, "second stub left the record alone");
        // Stop the first stub before its launcher is killed, or it would record that failure.
        TerminateProcess(first, 1);
        WaitForSingleObject(first, 10'000);
    }) == 0, "second stub exits 0");
    CloseHandle(first);
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: update_stub_test <Parties.exe> <update_fake_launcher.exe>\n");
        return 2;
    }
    g_stub = fs::absolute(argv[1]);
    g_fake = fs::absolute(argv[2]);
    g_namespace = L"PartiesStubTest" + std::to_wstring(GetCurrentProcessId());

    good_launcher_signals_boot_ok();
    unconfirmed_launcher_failing_twice_falls_back();
    unclean_sessions_across_starts_fall_back();
    hash_mismatch_never_executes();
    no_bootable_launcher_offers_safe_mode();
    running_launcher_is_activated();
    recover_flag_reaches_launcher();
    confirmed_launcher_does_not_fall_back();
    hung_launcher_is_terminated();
    second_stub_leaves_a_booting_stub_alone();

    fs::remove_all(fs::temp_directory_path() / "parties stub test");
    if (g_failures) std::fprintf(stderr, "%d check(s) failed\n", g_failures);
    else std::printf("update_stub: all scenarios passed\n");
    return g_failures ? 1 : 0;
}
