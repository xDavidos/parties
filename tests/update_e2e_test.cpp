// End to end: PartiesLauncher.exe supervising fake builds (update_fake_app.exe)
// in a throwaway install root, data root and local update source. Each
// scenario adopts build A, publishes build B in a signed manifest, and runs the
// launcher the way the boot stub does until the outcome settles. With
// kill points on, every scenario is replayed once per effect index n with the
// launcher terminated right before its n-th effect, then restarted, and must
// reach the same outcome without losing the identity or any preserved copy.
//
// usage: update_e2e_test --launcher <exe> --fake-app <exe> [--only <name>] [--no-kill]
//        update_e2e_test --launcher <exe> --real-client <exe>

#include "build_info.h"
#include "file_io.h"

#include <client/settings_schema.h>
#include <parties/crypto.h>
#include <parties/update/boot.h>
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
#include <vector>

#include <windows.h>

namespace fs = std::filesystem;
using namespace parties::update;
using namespace parties::update::launcher;
using namespace std::chrono_literals;

namespace {

// Matches the harness key pinned in testing launchers (updater/launcher/src/testing.cpp).
constexpr std::string_view kTestSeed = "f769e71268cc70210831656bcc920641326a5bd43fe4a015881f3515a8f908a8";
constexpr DWORD kKilled = 0xDEAD;
// One policy second is 10 ms: health deadline 2 s, confirm 6 s, clean exit after 0.3 s.
constexpr const char* kTiming = "10,200,600,30";

fs::path g_launcher, g_fake_app;
bool g_verbose = false;
int g_failures = 0;

void fail(const std::string& scenario, const std::string& what) {
    std::cerr << "FAILED [" << scenario << "]: " << what << "\n";
    ++g_failures;
}

std::wstring widen(std::string_view s) { return {s.begin(), s.end()}; }

SigningKey test_key() {
    SigningKey k;
    k.key_id = "parties-e2e-test";
    auto seed = Sha256::from_hex(kTestSeed);
    k.secret_key = seed->bytes;
    if (!parties::derive_pubkey(k.secret_key, k.public_key)) std::abort();
    return k;
}

class TempRoot {
public:
    explicit TempRoot(const std::string& name) {
        static int counter = 0;
        path_ = fs::temp_directory_path() /
                ("parties-e2e-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(counter++) + "-" + name);
        fs::remove_all(path_);
        fs::create_directories(path_);
    }
    ~TempRoot() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

struct Release {
    BuildEntry entry;
    fs::path dir;  // the files as published
};

FileEntry file_entry(const fs::path& file) {
    auto digest = digest_file(file);
    if (!digest) std::abort();
    return {file.filename().string(), digest->size, digest->sha256,
            {"https://example.invalid/" + digest->sha256.to_hex()}};
}

Release make_release(const fs::path& root, const fs::path& app_exe, const std::string& version, uint32_t schema,
                     uint32_t floor, bool fake) {
    Release r;
    r.dir = root / "releases" / version;
    fs::create_directories(r.dir);
    fs::copy_file(g_launcher, r.dir / kLauncherFile);
    fs::copy_file(app_exe, r.dir / kAppFile);
    r.entry.version = *Version::parse(version);
    r.entry.data_schema = schema;
    r.entry.data_compat_floor = floor;
    r.entry.files = {file_entry(r.dir / kLauncherFile), file_entry(r.dir / kAppFile)};
    if (fake) {
        std::ofstream(r.dir / "fake-build.json") << "{\"version\":\"" << version << "\",\"data_schema\":" << schema
                                                 << ",\"data_compat_floor\":" << floor << "}";
        r.entry.files.push_back(file_entry(r.dir / "fake-build.json"));
    }
    return r;
}

// What setup does: copy a release into builds\ with its build.json.
fs::path install(const fs::path& install_root, const Release& r) {
    fs::path dir = install_root / "builds" / r.entry.id().dir_name();
    fs::create_directories(dir);
    for (const FileEntry& f : r.entry.files) fs::copy_file(r.dir / f.name, dir / f.name);
    std::string error;
    if (!durable_replace(dir / kBuildInfoFile, write_build_info(r.entry), error)) std::abort();
    return dir;
}

void publish(const fs::path& source, const std::vector<const Release*>& releases, uint64_t serial) {
    fs::create_directories(source / "files");
    ManifestPayload p;
    p.channel = Channel::Stable;
    p.serial = serial;
    p.issued_at = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()) - 60s;
    p.expires_at = p.issued_at + std::chrono::days(45);
    for (const Release* r : releases) {
        p.builds.push_back(r->entry);
        for (const FileEntry& f : r->entry.files)
            fs::copy_file(r->dir / f.name, source / "files" / f.sha256.to_hex(), fs::copy_options::skip_existing);
    }
    std::ranges::sort(p.builds, [](const BuildEntry& a, const BuildEntry& b) { return a.version > b.version; });
    std::string payload = write_payload(p);
    SigningKey key = test_key();
    auto envelope = sign_manifest({reinterpret_cast<const uint8_t*>(payload.data()), payload.size()}, {&key, 1});
    std::string error;
    if (!envelope || !durable_replace(source / "stable.manifest.json", *envelope, error)) std::abort();
}

class Db {
public:
    explicit Db(const fs::path& path) {
        auto u8 = path.u8string();
        std::string uri = "file:" + std::string(u8.begin(), u8.end()) + "?mode=ro";
        std::ranges::replace(uri, '\\', '/');
        if (sqlite3_open_v2(uri.c_str(), &db_, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, nullptr) != SQLITE_OK) {
            sqlite3_close(db_);
            db_ = nullptr;
        }
    }
    ~Db() { sqlite3_close(db_); }
    bool ok() const { return db_ != nullptr; }
    long long scalar(const std::string& sql) const {
        sqlite3_stmt* stmt = nullptr;
        long long v = -1;
        if (db_ && sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK &&
            sqlite3_step(stmt) == SQLITE_ROW)
            v = sqlite3_column_int64(stmt, 0);
        sqlite3_finalize(stmt);
        return v;
    }

private:
    sqlite3* db_ = nullptr;
};

struct World {
    std::string name;
    TempRoot root;
    fs::path install_root, data_root, source, counter;
    Release a, b;
    bool with_b = true;
    size_t log_mark = 0;  // launcher.log size before the latest run

    World(std::string n, const std::string& b_version, uint32_t b_schema, uint32_t b_floor, bool publish_b)
        : name(std::move(n)), root(name) {
        install_root = root.path() / "install";
        data_root = root.path() / "data-root";
        source = root.path() / "source";
        counter = root.path() / "kill-counter";
        a = make_release(root.path(), g_fake_app, "1.0.0.0", 1, 1, true);
        b = make_release(root.path(), g_fake_app, b_version, b_schema, b_floor, true);
        install(install_root, a);
        with_b = publish_b;
        if (publish_b) publish(source, {&a, &b}, 1);
        else publish(source, {&a}, 1);
    }

    fs::path launcher_in(const Release& r) const { return install_root / "builds" / r.entry.id().dir_name() / kLauncherFile; }
    fs::path db() const { return data_root / "data" / "parties_client.db"; }
    fs::path backups() const { return data_root / "backups"; }
    fs::path log_file() const { return data_root / "logs" / "launcher.log"; }

    std::optional<SupervisorState> state() const {
        auto text = read_file(install_root / "state" / "supervisor.json");
        if (!text) return std::nullopt;
        auto s = parse_state(*text);
        return s ? std::optional(*s) : std::nullopt;
    }
    std::optional<boot::SessionReport> session() const {
        auto text = read_file(install_root / "state" / "launcher-session.json");
        return text ? boot::parse_session_report(*text) : std::nullopt;
    }
    std::string last_run_log() const {
        auto text = read_file(log_file()).value_or("");
        return log_mark <= text.size() ? text.substr(log_mark) : text;
    }
};

struct RunResult {
    DWORD exit_code = 0;
    bool boot_ok = false;
    bool timed_out = false;
};

RunResult run_launcher(World& w, const std::map<std::string, std::string>& extra_env,
                       const std::vector<std::wstring>& forwarded = {}, DWORD timeout_ms = 60000,
                       const fs::path& launcher = {}) {
    w.log_mark = read_file(w.log_file()).value_or("").size();
    std::map<std::wstring, std::wstring> env;
    for (wchar_t* block = GetEnvironmentStringsW(), *p = block; *p; p += wcslen(p) + 1) {
        std::wstring kv = p;
        size_t eq = kv.find(L'=', 1);
        if (eq != std::wstring::npos) env[kv.substr(0, eq)] = kv.substr(eq + 1);
    }
    for (const auto& [k, v] : extra_env) env[widen(k)] = widen(v);
    std::wstring block;
    for (const auto& [k, v] : env) block += k + L"=" + v + L'\0';
    block += L'\0';

    SECURITY_ATTRIBUTES inherit{sizeof inherit, nullptr, TRUE};
    HANDLE boot_ok = CreateEventW(&inherit, TRUE, FALSE, nullptr);
    fs::path exe = launcher.empty() ? w.launcher_in(w.a) : launcher;
    std::wstring cmd = L"\"" + exe.wstring() + L"\" \"--install-root=" + w.install_root.wstring() +
                       L"\" \"--data-root=" + w.data_root.wstring() + L"\" \"--feed-dir=" + w.source.wstring() +
                       L"\" --boot-session=42 --boot-ok-event=" +
                       std::to_wstring(reinterpret_cast<uintptr_t>(boot_ok)) +
                       L" --test-namespace=PartiesE2E" + std::to_wstring(GetCurrentProcessId()) + L" --";
    for (const std::wstring& f : forwarded) cmd += L" " + f;
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    RunResult result;
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_UNICODE_ENVIRONMENT, block.data(),
                        w.install_root.c_str(), &si, &pi)) {
        fail(w.name, "cannot start the launcher");
        CloseHandle(boot_ok);
        return result;
    }
    if (WaitForSingleObject(pi.hProcess, timeout_ms) != WAIT_OBJECT_0) {
        result.timed_out = true;
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, INFINITE);
    }
    GetExitCodeProcess(pi.hProcess, &result.exit_code);
    result.boot_ok = WaitForSingleObject(boot_ok, 0) == WAIT_OBJECT_0;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(boot_ok);
    return result;
}

const BuildRecord* record(const SupervisorState& s, const Release& r) { return s.find(r.entry.id()); }

bool settled(const SupervisorState& s) { return std::holds_alternative<Idle>(s.operation); }

bool rolled_back_to_a(const World& w, const SupervisorState& s, FailureReason reason) {
    const BuildRecord* b = record(s, w.b);
    return settled(s) && std::holds_alternative<Steady>(s.activation) && s.good == w.a.entry.id() && b &&
           b->verdict == Verdict{Quarantined{reason}} && s.notice && s.notice->kind == Notice::Kind::RolledBack;
}

// Holds in every scenario: the identity survives in the live database and
// in every copy the launcher or the app ever wrote.
void check_identity_everywhere(const World& w, const std::string& label) {
    Db live(w.db());
    if (live.scalar("SELECT count(*) FROM identity") != 1) fail(label, "the live database lost its identity row");
    std::error_code ec;
    for (const auto& item : fs::directory_iterator(w.backups(), ec)) {
        if (item.path().extension() != ".db") continue;
        Db copy(item.path());
        if (copy.scalar("SELECT count(*) FROM identity") != 1)
            fail(label, "backup " + item.path().filename().string() + " has no identity row");
    }
}

long long markers(const fs::path& db, const std::string& build) {
    Db d(db);
    return d.scalar("SELECT count(*) FROM markers WHERE build = '" + build + "'");
}

std::optional<fs::path> find_backup(const World& w, std::string_view prefix) {
    std::error_code ec;
    for (const auto& item : fs::directory_iterator(w.backups(), ec))
        if (item.path().filename().string().starts_with(prefix) && item.path().extension() == ".db") return item.path();
    return std::nullopt;
}

struct Scenario {
    std::string name;
    std::string b_fault;
    uint32_t b_schema = 1;
    uint32_t b_floor = 1;
    bool publish_b = true;
    std::function<void(World&)> prepare;           // runs without kill points
    std::function<bool(const World&)> done;        // the outcome has settled
    std::function<void(const World&, const std::string&)> verify;
};

enum class Outcome { Converged, KillNotReached, Failed };

Outcome run_scenario(const Scenario& sc, std::optional<int> kill_at) {
    const std::string label = sc.name + (kill_at ? " kill@" + std::to_string(*kill_at) : "");
    World w(sc.name, "2.0.0.0", sc.b_schema, sc.b_floor, sc.publish_b);
    std::map<std::string, std::string> env{
        {"PARTIES_TEST_TIMING", kTiming},
        {"PARTIES_TEST_RECOVERY", "quit"},
        {"PARTIES_FAKE_RUN_MS", "700"},
        {"PARTIES_FAKE_FAULT", sc.b_fault.empty() ? "" : "2.0.0.0:" + sc.b_fault},
    };
    if (sc.prepare) sc.prepare(w);

    bool killed = false;
    for (int run = 0; run < 8; ++run) {
        auto run_env = env;
        if (kill_at && !killed) {
            run_env["PARTIES_KILL_AT"] = std::to_string(*kill_at);
            run_env["PARTIES_KILL_COUNTER"] = w.counter.string();
        }
        RunResult r = run_launcher(w, run_env);
        if (g_verbose) std::cout << "--- " << label << " run " << run << " exit " << r.exit_code << "\n" << w.last_run_log();
        if (r.timed_out) {
            fail(label, "the launcher did not exit within 60 s");
            return Outcome::Failed;
        }
        if (r.exit_code == kKilled) {
            killed = true;
            auto session = w.session();
            if (!session || session->ended_clean) fail(label, "a killed launcher left a clean session report");
            continue;
        }
        if (r.exit_code != 0) {
            fail(label, "the launcher exited with " + std::to_string(r.exit_code));
            return Outcome::Failed;
        }
        auto session = w.session();
        if (!session || !session->ended_clean || session->session != 42)
            fail(label, "an orderly exit did not leave a clean session report");
        if (sc.done(w)) {
            if (kill_at && !killed) return Outcome::KillNotReached;
            sc.verify(w, label);
            check_identity_everywhere(w, label);
            return Outcome::Converged;
        }
    }
    auto text = read_file(w.install_root / "state" / "supervisor.json");
    fail(label, "did not settle after 8 launcher runs; state:\n" + text.value_or("(none)") + "\nlast log:\n" +
                    w.last_run_log());
    return Outcome::Failed;
}

std::vector<Scenario> scenarios() {
    std::vector<Scenario> out;

    out.push_back({"update-ok", "", 1, 1, true, nullptr,
                   [](const World& w) {
                       auto s = w.state();
                       return s && settled(*s) && s->good == w.b.entry.id();
                   },
                   [](const World& w, const std::string& label) {
                       auto s = w.state();
                       if (s->previous_good != w.a.entry.id()) fail(label, "A is not kept as previous_good");
                       if (!std::holds_alternative<Steady>(s->activation)) fail(label, "not steady after confirm");
                       if (markers(w.db(), "2.0.0.0") < 1) fail(label, "B never ran");
                       auto contract = boot::parse_boot_contract(
                           read_file(w.install_root / "state" / "boot-contract.json").value_or(""));
                       if (!contract || contract->good.dir != build_dir(w.b.entry.id()))
                           fail(label, "boot-contract.json does not point at B");
                   }});

    auto rollback_scenario = [](std::string name, std::string fault, FailureReason reason) {
        return Scenario{name, fault, 1, 1, true, nullptr,
                        [reason](const World& w) {
                            auto s = w.state();
                            return s && rolled_back_to_a(w, *s, reason);
                        },
                        [](const World& w, const std::string& label) {
                            Db live(w.db());
                            if (live.scalar("SELECT version FROM parties_schema") != 1)
                                fail(label, "the live database schema changed");
                            if (markers(w.db(), "2.0.0.0") != 0) fail(label, "B wrote to the live database");
                            if (markers(w.db(), "1.0.0.0") < 1) fail(label, "A never ran");
                        }};
    };
    out.push_back(rollback_scenario("crash-before-health", "crash-before-health", FailureReason::CrashedBeforeHealthy));
    out.push_back(rollback_scenario("hang-before-health", "hang-before-health", FailureReason::HungBeforeHealthy));
    out.push_back(rollback_scenario("init-failed", "exit-init-failed", FailureReason::InitFailed));

    out.push_back({"crash-after-health", "crash-after-health", 1, 1, true, nullptr,
                   [](const World& w) {
                       auto s = w.state();
                       // A kill during B's second run leaves that outcome unobserved, which
                       // is the second strike as well.
                       return s && (rolled_back_to_a(w, *s, FailureReason::CrashedRepeatedlyAfterHealthy) ||
                                    rolled_back_to_a(w, *s, FailureReason::UnknownOutcomeRepeated));
                   },
                   [](const World& w, const std::string& label) {
                       if (markers(w.db(), "2.0.0.0") < 1) fail(label, "B never ran");
                   }});

    out.push_back({"migrate-then-crash", "migrate-then-crash", 3, 3, true, nullptr,
                   [](const World& w) {
                       auto s = w.state();
                       return s && rolled_back_to_a(w, *s, FailureReason::CrashedBeforeHealthy) &&
                              markers(w.db(), "1.0.0.0") >= 1 && w.last_run_log().find("app exited with code 0x0") !=
                                                                    std::string::npos;
                   },
                   [](const World& w, const std::string& label) {
                       auto s = w.state();
                       Db live(w.db());
                       if (live.scalar("SELECT version FROM parties_schema") != 1 ||
                           live.scalar("SELECT compat_floor FROM parties_schema") != 1)
                           fail(label, "the live database was not restored to schema 1");
                       if (markers(w.db(), "2.0.0.0") != 0) fail(label, "B's rows are still in the live database");
                       auto preserved = find_backup(w, "rolled-back-");
                       if (!preserved) return fail(label, "the migrated database was not preserved");
                       if (!s->notice || !s->notice->preserved ||
                           preserved->stem().string() != s->notice->preserved->stem)
                           fail(label, "the notice does not name the preserved copy");
                       Db kept(*preserved);
                       if (kept.scalar("SELECT version FROM parties_schema") != 3 ||
                           kept.scalar("SELECT compat_floor FROM parties_schema") != 3)
                           fail(label, "the preserved copy is not the migrated database");
                       if (kept.scalar("SELECT count(*) FROM markers WHERE build = '2.0.0.0'") < 1)
                           fail(label, "the preserved copy lost B's marker row");
                       if (!find_backup(w, "pre-")) fail(label, "no pre-candidate snapshot");
                   }});

    out.push_back({"exit-data-unavailable", "exit-data-unavailable", 1, 1, true, nullptr,
                   [](const World& w) {
                       auto s = w.state();
                       return s && settled(*s) && std::holds_alternative<Probation>(s->activation) &&
                              w.last_run_log().find("recovery shown: data_unavailable") != std::string::npos;
                   },
                   [](const World& w, const std::string& label) {
                       auto s = w.state();
                       const BuildRecord* b = record(*s, w.b);
                       if (!b || b->quarantined()) fail(label, "B was blamed for unavailable data");
                       if (s->good != w.a.entry.id()) fail(label, "good changed");
                   }});

    out.push_back({"corrupt-good-exe", "", 1, 1, false,
                   [](World& w) {
                       RunResult r = run_launcher(w, {{"PARTIES_TEST_TIMING", kTiming}, {"PARTIES_FAKE_RUN_MS", "300"}});
                       if (r.exit_code != 0 || markers(w.db(), "1.0.0.0") != 1)
                           fail(w.name, "the clean first run did not start A");
                       fs::path exe = w.install_root / "builds" / w.a.entry.id().dir_name() / kAppFile;
                       std::ofstream(exe, std::ios::binary | std::ios::app).put('\0');
                   },
                   [](const World& w) {
                       auto s = w.state();
                       return s && settled(*s) &&
                              w.last_run_log().find("recovery shown: repairing") != std::string::npos;
                   },
                   [](const World& w, const std::string& label) {
                       if (markers(w.db(), "1.0.0.0") != 1) fail(label, "the corrupt executable was started");
                       auto s = w.state();
                       if (s->good != w.a.entry.id()) fail(label, "good changed");
                   }});
    return out;
}

int smoke_real_client(const fs::path& client) {
    const std::string name = "real-client";
    World w(name, "2.0.0.0", 1, 1, false);
    fs::remove(w.source / "stable.manifest.json");
    fs::path dev_db = client.parent_path() / "parties_client.db";
    auto dev_before = digest_file(dev_db);

    Release r = make_release(w.root.path() / "real", client, "0.0.0.1",
                             static_cast<uint32_t>(parties::client::schema::kCurrent.version),
                             static_cast<uint32_t>(parties::client::schema::kCurrent.compat_floor), false);
    fs::path dir = install(w.install_root, r);
    RunResult result = run_launcher(w, {{"PARTIES_TEST_EXIT_AFTER_HEALTHY_MS", "3000"}}, {L"--allow-multiple-instances"},
                                    180000, dir / kLauncherFile);
    std::string log = read_file(w.log_file()).value_or("");
    if (result.timed_out || result.exit_code != 0) fail(name, "the launcher did not exit cleanly");
    if (!result.boot_ok) fail(name, "boot_ok was not signalled");
    if (log.find("app reported healthy") == std::string::npos) fail(name, "the real client never reported healthy");
    if (log.find("app exited with code 0x0") == std::string::npos) fail(name, "the real client did not exit cleanly");
    auto s = w.state();
    if (!s || s->good != r.entry.id() || !settled(*s)) fail(name, "unexpected supervisor state");
    if (!fs::exists(w.db())) fail(name, "the client did not use the launcher's data directory");
    if (digest_file(dev_db) != dev_before) fail(name, "the developer database next to the build changed");
    std::istringstream lines(log);
    for (std::string line; std::getline(lines, line);)
        if (line.find("adopted") != std::string::npos || line.find("started") != std::string::npos ||
            line.find("healthy") != std::string::npos || line.find("exit") != std::string::npos)
            std::cout << "  " << line << "\n";
    std::cout << "  data dir: " << w.db().string() << (fs::exists(w.db()) ? " (created)" : " (missing)") << "\n";
    std::cout << "real-client smoke: " << (g_failures ? "FAILED" : "healthy, clean exit, dev data untouched") << "\n";
    if (g_failures) std::cout << log;
    return g_failures ? 1 : 0;
}

} // namespace

int main(int argc, char** argv) {
    std::optional<std::string> only;
    std::optional<fs::path> real_client;
    bool kill_points = true;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--launcher" && i + 1 < argc) g_launcher = argv[++i];
        else if (a == "--fake-app" && i + 1 < argc) g_fake_app = argv[++i];
        else if (a == "--real-client" && i + 1 < argc) real_client = argv[++i];
        else if (a == "--only" && i + 1 < argc) only = argv[++i];
        else if (a == "--no-kill") kill_points = false;
        else if (a == "--verbose") g_verbose = true;
    }
    if (!parties::crypto_init()) return 2;
    if (g_launcher.empty() || g_fake_app.empty()) {
        std::cerr << "usage: update_e2e_test --launcher <exe> --fake-app <exe> [--only <name>] [--no-kill]\n";
        return 2;
    }
    if (real_client) return smoke_real_client(*real_client);

    std::printf("%-24s %-6s %s\n", "scenario", "plain", "kill points");
    int total_kills = 0;
    for (const Scenario& sc : scenarios()) {
        if (only && *only != sc.name) continue;
        auto started = std::chrono::steady_clock::now();
        int before = g_failures;
        bool plain = run_scenario(sc, std::nullopt) == Outcome::Converged && g_failures == before;
        int kills = 0;
        bool kills_ok = true;
        if (kill_points && plain) {
            for (int n = 1; n < 200; ++n) {
                int b = g_failures;
                Outcome o = run_scenario(sc, n);
                if (o == Outcome::KillNotReached) break;
                ++kills;
                if (o != Outcome::Converged || g_failures != b) kills_ok = false;
            }
        }
        total_kills += kills;
        auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        std::printf("%-24s %-6s %s (%d) %.0fs\n", sc.name.c_str(), plain ? "pass" : "FAIL",
                    !kill_points ? "skipped" : kills_ok && plain ? "pass" : "FAIL", kills, seconds);
        std::fflush(stdout);
    }
    std::printf("kill points exercised: %d\n", total_kills);
    return g_failures ? 1 : 0;
}
