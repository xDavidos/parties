// End to end on macOS: the real host running the real PartiesLauncher.dylib
// (Parties --role=launcher, started here the way a booting front will start
// it), which supervises fake builds (update_front_door_fake_client.dylib as
// PartiesClient.dylib) through LaunchServices fronts, in a throwaway runtime
// root, data root and feed directory under $TMPDIR. Each scenario installs
// build A from a seed bundle, publishes build B in a signed kMacClient
// manifest, and runs the launcher until the outcome settles. With kill
// points on, every scenario is replayed once per effect index n with the
// launcher (or the seed install) ended right before its n-th effect, then run
// again, and must reach the same outcome with no cleanup in between.
//
// The boot scenarios then start the bundle with `open`, as a double-click
// does, so the front boots the launcher itself (process-contract.md, "Front
// start"): broken launchers, safe mode, a launcher lost before boot_ok, and a
// bundle on a read-only disk image. They have no kill sweep.
//
// usage: update_macos_e2e_test --app <built Parties.app> --entitlements <file>
//            --fake-client <dylib> --launcher <dylib> --broken-launcher-exits <dylib>
//            --broken-launcher-hangs <dylib> [--only <name>] [--no-kill] [--verbose]

#include <parties/crypto.h>
#include <parties/data_lock.h>
#include <parties/update/boot.h>
#include <parties/update/manifest.h>
#include <parties/update/state_codec.h>

#include <sqlite3.h>

#include <CommonCrypto/CommonDigest.h>
#include <CoreGraphics/CoreGraphics.h>
#include <fcntl.h>
#include <libproc.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

extern char** environ;

namespace fs = std::filesystem;
using namespace parties::update;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

namespace {

// Matches the harness key pinned in testing launchers (updater/launcher/src/core_shell/test_hooks.cpp).
constexpr std::string_view kTestSeed = "f769e71268cc70210831656bcc920641326a5bd43fe4a015881f3515a8f908a8";
constexpr int kKilled = 0xDE;
// Policy seconds of 10 ms: a 6 s health deadline, a 6 s confirm, a clean exit
// after 0.3 s confirms. A LaunchServices front needs about a second to load.
constexpr const char* kTiming = "10,600,600,30";
constexpr const char* kRunMs = "700";

fs::path g_app, g_entitlements, g_fake_client, g_launcher, g_broken_exits, g_broken_hangs, g_work;
bool g_verbose = false;
int g_failures = 0;
std::set<int> g_fronts;  // every front pid seen, for the crash report check

void fail(const std::string& scenario, const std::string& what) {
    std::cout << "FAILED [" << scenario << "]: " << what << "\n";
    ++g_failures;
}

// The last lines of every log a world wrote: the launcher's, and each host
// process's stderr (front door trace, payload loads, the fake client).
void dump_logs(const fs::path& logs) {
    std::error_code ec;
    for (const auto& item : fs::directory_iterator(logs, ec)) {
        std::ifstream in(item.path());
        std::vector<std::string> lines;
        for (std::string line; std::getline(in, line);) lines.push_back(line);
        std::cout << "  ---- " << item.path().filename().string() << " (last 80 of " << lines.size() << " lines)\n";
        for (size_t i = lines.size() > 80 ? lines.size() - 80 : 0; i < lines.size(); ++i)
            std::cout << "  " << lines[i] << "\n";
    }
}

std::optional<std::string> read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void write_file(const fs::path& path, const std::string& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary | std::ios::trunc) << bytes;
}

Sha256 sha256_file(const fs::path& path) {
    const std::string bytes = read_file(path).value_or("");
    Sha256 out;
    CC_SHA256(bytes.data(), CC_LONG(bytes.size()), out.bytes.data());
    return out;
}

// Runs a tool and returns its exit status; its output goes to `log`.
int run_tool(const std::vector<std::string>& argv, const fs::path& log) {
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, 1, log.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    posix_spawn_file_actions_adddup2(&actions, 1, 2);
    std::vector<char*> args;
    for (const std::string& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);
    pid_t pid = 0;
    int status = -1;
    if (posix_spawnp(&pid, args[0], &actions, nullptr, args.data(), environ) == 0) waitpid(pid, &status, 0);
    posix_spawn_file_actions_destroy(&actions);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128;
}

void must(int status, const std::string& what) {
    if (status != 0) {
        std::cout << "cannot " << what << " (exit " << status << ")\n";
        std::exit(2);
    }
}

SigningKey test_key() {
    SigningKey k;
    k.key_id = "47ac18c9b12dc1be";  // what parties_manifest_tool calls this key
    k.secret_key = Sha256::from_hex(kTestSeed)->bytes;
    if (!parties::derive_pubkey(k.secret_key, k.public_key)) std::abort();
    return k;
}

// ---- builds and seed bundles -----------------------------------------------

struct Base {
    fs::path app;  // Parties.app with the real launcher and the fake client as its seed files
    FileEntry launcher, client;
};
Base g_base;

FileEntry file_entry(const fs::path& file) {
    const Sha256 sha = sha256_file(file);
    return {file.filename().string(), fs::file_size(file), sha, {"https://example.invalid/files/" + sha.to_hex()}};
}

void prepare_base() {
    const fs::path app = g_work / "base" / "Parties.app";
    const fs::path log = g_work / "codesign.log";
    must(run_tool({"ditto", g_app.string(), app.string()}, log), "copy the built Parties.app");
    const fs::path frameworks = app / "Contents" / "Frameworks";
    fs::copy_file(g_fake_client, frameworks / kMacClient.app_file, fs::copy_options::overwrite_existing);
    fs::copy_file(g_launcher, frameworks / kMacClient.launcher_file, fs::copy_options::overwrite_existing);
    for (auto [file, id] : {std::pair{kMacClient.app_file, "org.parties.client.app"},
                            std::pair{kMacClient.launcher_file, "org.parties.client.launcher"}})
        must(run_tool({"codesign", "--force", "--sign", "-", "--identifier", id, (frameworks / file).string()}, log),
             "sign " + std::string(file));
    g_base = {app, file_entry(frameworks / kMacClient.launcher_file), file_entry(frameworks / kMacClient.app_file)};
}

struct Release {
    BuildEntry entry;
    fs::path bundle;  // a Parties.app whose seed is this build
    std::string token() const { return build_dir(entry.id()); }
    std::string dir() const { return entry.id().dir_name(); }
};

// One seed bundle per build, shared by every scenario that uses the build.
// `launcher` replaces the real launcher dylib in it.
Release release(const std::string& version, uint32_t schema, uint32_t floor, const fs::path& launcher = {}) {
    static std::map<std::string, Release> made;
    const std::string key = version + "-" + std::to_string(schema) + "-" + std::to_string(floor) +
                            (launcher.empty() ? "" : "-" + launcher.stem().string());
    if (auto it = made.find(key); it != made.end()) return it->second;
    Release r;
    r.entry.version = *Version::parse(version);
    r.entry.data_schema = schema;
    r.entry.data_compat_floor = floor;
    r.entry.files = {g_base.launcher, g_base.client};
    r.bundle = g_work / "bundles" / key / "Parties.app";
    const fs::path log = g_work / "codesign.log";
    must(run_tool({"ditto", g_base.app.string(), r.bundle.string()}, log), "copy the base bundle");
    if (!launcher.empty()) {
        const fs::path file = r.bundle / "Contents" / "Frameworks" / kMacClient.launcher_file;
        fs::copy_file(launcher, file, fs::copy_options::overwrite_existing);
        must(run_tool({"codesign", "--force", "--sign", "-", "--identifier", "org.parties.client.launcher", file.string()}, log),
             "sign " + launcher.filename().string());
        r.entry.files[0] = file_entry(file);
    }
    write_file(r.bundle / "Contents" / "Resources" / "seed" / "build.json", write_build_entry(r.entry));
    must(run_tool({"codesign", "--force", "--sign", "-", "--options", "runtime", "--entitlements",
                   g_entitlements.string(), r.bundle.string()},
                  log),
         "sign the " + version + " bundle");
    return made[key] = r;
}

// ---- the world one scenario runs in ----------------------------------------

struct Publish {
    Channel channel = Channel::Stable;
    std::vector<Version> revoked;
    bool expired = false;
};

void publish(const fs::path& feed, const std::vector<const Release*>& releases, uint64_t serial,
             const Publish& how = {}) {
    fs::create_directories(feed / "files");
    ManifestPayload p;
    p.product = kMacClient.id;
    p.channel = how.channel;
    p.serial = serial;
    p.issued_at = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()) -
                  (how.expired ? std::chrono::days(50) : std::chrono::days(0)) - 60s;
    p.expires_at = p.issued_at + std::chrono::days(45);
    p.revoked = how.revoked;
    for (const Release* r : releases) {
        p.builds.push_back(r->entry);
        const fs::path frameworks = r->bundle / "Contents" / "Frameworks";
        for (const FileEntry& f : r->entry.files)
            fs::copy_file(frameworks / f.name, feed / "files" / f.sha256.to_hex(), fs::copy_options::skip_existing);
    }
    std::ranges::sort(p.builds, [](const BuildEntry& a, const BuildEntry& b) { return a.version > b.version; });
    const std::string payload = write_payload(p);
    SigningKey key = test_key();
    auto envelope = sign_manifest({reinterpret_cast<const uint8_t*>(payload.data()), payload.size()}, {&key, 1});
    if (!envelope) std::abort();
    write_file(feed / (std::string(channel_name(how.channel)) + ".manifest.json"), *envelope);
}

class Db {
public:
    explicit Db(const fs::path& path) {
        const std::string uri = "file:" + path.string() + "?mode=ro";
        if (sqlite3_open_v2(uri.c_str(), &db_, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, nullptr) != SQLITE_OK) {
            sqlite3_close(db_);
            db_ = nullptr;
        }
    }
    ~Db() { sqlite3_close(db_); }
    long long scalar(const std::string& sql) const {
        sqlite3_stmt* stmt = nullptr;
        long long v = -1;
        if (db_ && sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW)
            v = sqlite3_column_int64(stmt, 0);
        sqlite3_finalize(stmt);
        return v;
    }

private:
    sqlite3* db_ = nullptr;
};

struct RunResult {
    int exit_code = -1;
    bool boot_ok = false;
    bool timed_out = false;
};

struct World {
    std::string name;
    fs::path root, install_root, data_root, feed, logs, counter;
    Release a, b;
    size_t log_mark = 0;

    World(std::string n, const std::string& b_version, uint32_t b_schema, uint32_t b_floor)
        : name(std::move(n)) {
        static int counter_n = 0;
        root = g_work / ("w" + std::to_string(counter_n++) + "-" + name);
        fs::remove_all(root);
        install_root = root / "runtime";
        data_root = root / "data";
        feed = root / "feed";
        logs = root / "logs";
        counter = root / "kill-counter";
        fs::create_directories(logs);
        a = release("1.0.0.0", 1, 1);
        b = release(b_version, b_schema, b_floor);
    }
    ~World() {
        if (g_failures != failures_at_start) dump_logs(logs);
        std::error_code ec;
        fs::remove_all(root, ec);
    }
    int failures_at_start = g_failures;

    fs::path db() const { return data_root / "parties.db"; }
    fs::path backups() const { return data_root / "backups"; }
    fs::path log_file() const { return logs / "launcher.log"; }
    fs::path status_log() const { return root / "status.log"; }
    fs::path build_dir_of(const Release& r) const { return install_root / "builds" / r.dir(); }
    std::string statuses() const { return read_file(status_log()).value_or(""); }
    std::string full_log() const { return read_file(log_file()).value_or(""); }
    std::string seed_log() const { return read_file(logs / "install-seed.log").value_or(""); }
    std::string last_run_log() const {
        std::string text = full_log();
        return log_mark <= text.size() ? text.substr(log_mark) : text;
    }
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
    std::optional<boot::BootContract> contract() const {
        auto text = read_file(install_root / "state" / "boot-contract.json");
        return text ? boot::parse_boot_contract(*text) : std::nullopt;
    }
    bool leftovers() const {
        std::error_code ec;
        for (const auto& item : fs::directory_iterator(install_root / "builds", ec)) {
            const std::string n = item.path().filename().string();
            if (n.starts_with(".staging-") || n.starts_with(".trash-")) return true;
        }
        for (const auto& item : fs::directory_iterator(install_root / "downloads", ec))
            if (item.path().extension() == ".partial") return true;
        return false;
    }
};

std::vector<int> live_hosts() {
    std::vector<int> pids(4096);
    int n = proc_listallpids(pids.data(), int(pids.size() * sizeof(int)));
    std::vector<int> out;
    const std::string prefix = g_work.string();
    char path[PROC_PIDPATHINFO_MAXSIZE];
    for (int i = 0; i < n; ++i) {
        if (pids[i] <= 0 || proc_pidpath(pids[i], path, sizeof path) <= 0) continue;
        if (std::string_view(path).starts_with(prefix)) out.push_back(pids[i]);
    }
    return out;
}

// A front LaunchServices was asked for may still be on its way when its
// launcher dies; the next run starts only once every host process is gone.
void wait_hosts_gone(const World& w, bool after_kill) {
    if (after_kill) std::this_thread::sleep_for(2s);
    const auto until = Clock::now() + 30s;
    std::vector<int> left;
    while (!(left = live_hosts()).empty() && Clock::now() < until) std::this_thread::sleep_for(50ms);
    for (int pid : left) {
        fail(w.name, "host process " + std::to_string(pid) + " outlived its launcher by 30 s");
        kill(pid, SIGKILL);
    }
    std::error_code ec;
    for (const auto& item : fs::directory_iterator(w.logs, ec)) {
        int pid = 0;
        if (std::sscanf(item.path().filename().c_str(), "front-%d.log", &pid) == 1) g_fronts.insert(pid);
    }
}

// The fronts get the fake's settings from data_dir: LaunchServices starts
// them without this process's environment.
void write_fake_env(const World& w, const std::map<std::string, std::string>& env) {
    std::string text = "PARTIES_FAKE_STATUS_LOG=" + w.status_log().string() + "\n";
    for (const auto& [k, v] : env)
        if (k.starts_with("PARTIES_FAKE_")) text += k + "=" + v + "\n";
    write_file(w.data_root / "fake-env", text);
}

RunResult run_host(World& w, const Release& from, const std::vector<std::string>& role_args,
                   const std::map<std::string, std::string>& env, std::chrono::seconds timeout = 60s,
                   const std::function<bool()>& kill_when = {}) {
    w.log_mark = w.full_log().size();
    write_fake_env(w, env);
    std::vector<std::string> argv{(from.bundle / "Contents" / "MacOS" / "Parties").string(), "--role=launcher",
                                  "--install-root=" + w.install_root.string(), "--data-root=" + w.data_root.string(),
                                  "--test-namespace=e2e" + std::to_string(getpid()), "--log-dir=" + w.logs.string()};
    argv.insert(argv.end(), role_args.begin(), role_args.end());

    std::map<std::string, std::string> vars;
    for (char** e = environ; *e; ++e) {
        std::string kv = *e;
        size_t eq = kv.find('=');
        if (eq != std::string::npos) vars[kv.substr(0, eq)] = kv.substr(eq + 1);
    }
    for (const auto& [k, v] : env)
        if (!k.starts_with("PARTIES_FAKE_")) vars[k] = v;
    std::vector<std::string> env_strings;
    for (const auto& [k, v] : vars) env_strings.push_back(k + "=" + v);

    int boot_ok[2];
    if (pipe(boot_ok) != 0) std::abort();
    fcntl(boot_ok[0], F_SETFD, FD_CLOEXEC);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, boot_ok[1], 3);
    const fs::path stdio = w.logs / "host-stdio.log";
    posix_spawn_file_actions_addopen(&actions, 1, stdio.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    posix_spawn_file_actions_adddup2(&actions, 1, 2);
    std::vector<char*> args, envp;
    for (std::string& a : argv) args.push_back(a.data());
    args.push_back(nullptr);
    for (std::string& e : env_strings) envp.push_back(e.data());
    envp.push_back(nullptr);
    pid_t pid = 0;
    RunResult result;
    const int spawned = posix_spawn(&pid, args[0], &actions, nullptr, args.data(), envp.data());
    posix_spawn_file_actions_destroy(&actions);
    close(boot_ok[1]);
    if (spawned != 0) {
        fail(w.name, "cannot start the host: " + std::string(std::strerror(spawned)));
        close(boot_ok[0]);
        return result;
    }
    const auto deadline = Clock::now() + timeout;
    int status = 0;
    bool killed_here = false;
    while (waitpid(pid, &status, WNOHANG) == 0) {
        if ((kill_when && kill_when()) || Clock::now() > deadline) {
            result.timed_out = !kill_when || Clock::now() > deadline;
            killed_here = true;
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            break;
        }
        std::this_thread::sleep_for(20ms);
    }
    result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    fcntl(boot_ok[0], F_SETFL, O_NONBLOCK);
    char byte = 0;
    result.boot_ok = read(boot_ok[0], &byte, 1) == 1;
    close(boot_ok[0]);
    wait_hosts_gone(w, killed_here || result.exit_code == kKilled);
    return result;
}

RunResult run_launcher(World& w, const std::map<std::string, std::string>& env, const std::vector<std::string>& flags = {},
                       const std::function<bool()>& kill_when = {}) {
    auto contract = w.contract();
    if (!contract) {
        fail(w.name, "no boot-contract.json to choose a launcher from");
        return {};
    }
    std::vector<std::string> args{"--build=" + contract->good.dir, "--boot-session=42", "--boot-ok-fd=3",
                                  "--feed-dir=" + w.feed.string()};
    args.insert(args.end(), flags.begin(), flags.end());
    return run_host(w, w.a, args, env, 60s, kill_when);
}

RunResult install_seed(World& w, const Release& seed, const std::map<std::string, std::string>& env) {
    return run_host(w, seed, {"--install-seed=" + seed.bundle.string()}, env);
}

std::map<std::string, std::string> base_env() {
    return {{"PARTIES_TEST_TIMING", kTiming}, {"PARTIES_TEST_RECOVERY", "quit"}, {"PARTIES_FAKE_RUN_MS", kRunMs}};
}

const BuildRecord* record(const SupervisorState& s, const Release& r) { return s.find(r.entry.id()); }

bool settled(const SupervisorState& s) { return std::holds_alternative<Idle>(s.operation); }

bool staged(const SupervisorState& s, const Release& r) {
    auto* st = std::get_if<Staged>(&s.activation);
    return st && st->build == r.entry.id();
}

bool good_is(const World& w, const Release& r) {
    auto s = w.state();
    return s && settled(*s) && s->good == r.entry.id();
}

void prove_b(World& w) {
    for (int i = 0; i < 6 && !good_is(w, w.b); ++i) run_launcher(w, base_env());
    if (!good_is(w, w.b)) fail(w.name, "B did not become good");
}

bool rolled_back_to_a(const World& w, const SupervisorState& s, FailureReason reason) {
    const BuildRecord* b = record(s, w.b);
    return settled(s) && std::holds_alternative<Steady>(s.activation) && s.good == w.a.entry.id() && b &&
           b->verdict == Verdict{Quarantined{reason}} && s.notice && s.notice->kind == Notice::Kind::RolledBack;
}

long long markers(const fs::path& db, const std::string& build) {
    return Db(db).scalar("SELECT count(*) FROM markers WHERE build = '" + build + "'");
}

std::optional<fs::path> find_backup(const World& w, std::string_view prefix) {
    std::error_code ec;
    for (const auto& item : fs::directory_iterator(w.backups(), ec))
        if (item.path().filename().string().starts_with(prefix) && item.path().extension() == ".db") return item.path();
    return std::nullopt;
}

void check_identity_everywhere(const World& w, const std::string& label) {
    if (Db(w.db()).scalar("SELECT count(*) FROM identity") != 1) fail(label, "the live database lost its identity row");
    std::error_code ec;
    for (const auto& item : fs::directory_iterator(w.backups(), ec)) {
        if (item.path().extension() != ".db") continue;
        if (Db(item.path()).scalar("SELECT count(*) FROM identity") != 1)
            fail(label, "backup " + item.path().filename().string() + " has no identity row");
    }
}

// Lines that tell a reader what each run decided.
void quote_log(const World& w, const std::string& label) {
    std::istringstream lines(w.seed_log() + w.full_log());
    const char* keys[] = {"adopted", "sealed", "staged", "starting", "app exited", "healthy", "killed", "snapshot",
                          "restored", "preserved", "set aside", "recovery", "repaired", "deleted", "data.lock",
                          "kept the installed", "seed", "downgrading", "another launcher", "effect failed"};
    std::cout << "  -- " << label << ": launcher log\n";
    for (std::string line; std::getline(lines, line);)
        if (std::ranges::any_of(keys, [&](const char* k) { return line.find(k) != std::string::npos; }))
            std::cout << "  " << line << "\n";
}

// ---- scenarios ---------------------------------------------------------------

struct Scenario {
    std::string name;
    std::string b_fault;
    uint32_t b_schema = 1;
    uint32_t b_floor = 1;
    bool publish_b = true;
    std::function<void(World&)> prepare;
    std::function<bool(const World&)> done;
    std::function<void(const World&, const std::string&)> verify;
    bool kill_points = true;
    std::map<std::string, std::string> env{};
    std::vector<std::string> launcher_flags{};
    std::optional<std::string> seed;  // install this release's bundle before the launcher runs: "a" or "b"
    bool install_a = true;
};

enum class Outcome { Converged, KillNotReached, Failed };

Outcome run_scenario(const Scenario& sc, std::optional<int> kill_at) {
    const std::string label = sc.name + (kill_at ? " kill@" + std::to_string(*kill_at) : "");
    World w(sc.name, "2.0.0.0", sc.b_schema, sc.b_floor);
    if (sc.install_a && install_seed(w, w.a, {}).exit_code != 0) {
        fail(label, "the first install of A failed");
        return Outcome::Failed;
    }
    if (sc.publish_b) publish(w.feed, {&w.a, &w.b}, 1);
    else publish(w.feed, {&w.a}, 1);
    std::map<std::string, std::string> env = base_env();
    env["PARTIES_FAKE_FAULT"] = sc.b_fault.empty() ? "" : "2.0.0.0:" + sc.b_fault;
    for (const auto& [k, v] : sc.env) env[k] = v;
    if (sc.prepare) sc.prepare(w);

    bool killed = false;
    auto run_env = [&] {
        auto e = env;
        if (kill_at && !killed) {
            e["PARTIES_KILL_AT"] = std::to_string(*kill_at);
            e["PARTIES_KILL_COUNTER"] = w.counter.string();
        }
        return e;
    };
    if (sc.seed) {
        const Release& seed = *sc.seed == "a" ? w.a : w.b;
        for (int run = 0;; ++run) {
            RunResult r = install_seed(w, seed, run_env());
            if (r.exit_code == kKilled) {
                killed = true;
                continue;
            }
            if (r.exit_code != 0 || run > 4) {
                fail(label, "the seed install exited " + std::to_string(r.exit_code) + "\n" + w.last_run_log());
                return Outcome::Failed;
            }
            break;
        }
    }
    for (int run = 0; run < 8; ++run) {
        RunResult r = run_launcher(w, run_env(), sc.launcher_flags);
        if (g_verbose) std::cout << "--- " << label << " run " << run << " exit " << r.exit_code << "\n" << w.last_run_log();
        if (r.timed_out) {
            fail(label, "the launcher did not exit within 60 s\n" + w.last_run_log());
            return Outcome::Failed;
        }
        if (r.exit_code == kKilled) {
            killed = true;
            auto session = w.session();
            if (!session || session->ended_clean) fail(label, "a killed launcher left a clean session report");
            continue;
        }
        if (r.exit_code != 0) {
            fail(label, "the launcher exited with " + std::to_string(r.exit_code) + "\n" + w.last_run_log());
            return Outcome::Failed;
        }
        auto session = w.session();
        if (!session || !session->ended_clean || session->session != 42)
            fail(label, "an orderly exit did not leave a clean session report");
        if (sc.done(w)) {
            if (kill_at && !killed) return Outcome::KillNotReached;
            sc.verify(w, label);
            check_identity_everywhere(w, label);
            if (w.full_log().find("data.lock is held") != std::string::npos)
                fail(label, "the launcher found data.lock held with no app running");
            if (!kill_at) quote_log(w, label);
            return Outcome::Converged;
        }
    }
    fail(label, "did not settle after 8 launcher runs; state:\n" +
                    read_file(w.install_root / "state" / "supervisor.json").value_or("(none)") + "\nlast log:\n" +
                    w.last_run_log());
    return Outcome::Failed;
}

std::vector<Scenario> scenarios() {
    std::vector<Scenario> out;

    out.push_back({.name = "update-ok",
                   .done = [](const World& w) { return good_is(w, w.b); },
                   .verify = [](const World& w, const std::string& label) {
                       auto s = w.state();
                       if (s->previous_good != w.a.entry.id()) fail(label, "A is not kept as previous_good");
                       if (!std::holds_alternative<Steady>(s->activation)) fail(label, "not steady after confirm");
                       if (markers(w.db(), "2.0.0.0") < 1) fail(label, "B never ran");
                       auto contract = w.contract();
                       if (!contract || contract->good.dir != w.b.token()) fail(label, "boot-contract.json does not point at B");
                       if (w.leftovers()) fail(label, "a partial download or staging directory was left behind");
                   }});

    auto rollback = [](std::string name, std::string fault, FailureReason reason) {
        return Scenario{.name = name,
                        .b_fault = fault,
                        .done = [reason](const World& w) {
                            auto s = w.state();
                            return s && rolled_back_to_a(w, *s, reason);
                        },
                        .verify = [](const World& w, const std::string& label) {
                            if (Db(w.db()).scalar("SELECT version FROM parties_schema") != 1)
                                fail(label, "the live database schema changed");
                            if (markers(w.db(), "2.0.0.0") != 0) fail(label, "B wrote to the live database");
                            if (markers(w.db(), "1.0.0.0") < 1) fail(label, "A never ran");
                        }};
    };
    out.push_back(rollback("crash-before-health", "crash-before-health", FailureReason::CrashedBeforeHealthy));
    out.push_back(rollback("hang-before-health", "hang-before-health", FailureReason::HungBeforeHealthy));
    auto init_failed = rollback("init-failed", "exit-init-failed", FailureReason::InitFailed);
    init_failed.kill_points = false;
    out.push_back(init_failed);

    out.push_back({.name = "crash-after-health",
                   .b_fault = "crash-after-health",
                   .done = [](const World& w) {
                       auto s = w.state();
                       // A kill during B's second run leaves that outcome unobserved, which
                       // is the second strike as well.
                       return s && (rolled_back_to_a(w, *s, FailureReason::CrashedRepeatedlyAfterHealthy) ||
                                    rolled_back_to_a(w, *s, FailureReason::UnknownOutcomeRepeated));
                   },
                   .verify = [](const World& w, const std::string& label) {
                       if (markers(w.db(), "2.0.0.0") < 1) fail(label, "B never ran");
                   }});

    out.push_back({.name = "migrate-then-crash",
                   .b_fault = "migrate-then-crash",
                   .b_schema = 3,
                   .b_floor = 3,
                   .done = [](const World& w) {
                       auto s = w.state();
                       return s && rolled_back_to_a(w, *s, FailureReason::CrashedBeforeHealthy) &&
                              markers(w.db(), "1.0.0.0") >= 1 &&
                              w.last_run_log().find("app exited: Clean") != std::string::npos;
                   },
                   .verify = [](const World& w, const std::string& label) {
                       auto s = w.state();
                       Db live(w.db());
                       if (live.scalar("SELECT version FROM parties_schema") != 1 ||
                           live.scalar("SELECT compat_floor FROM parties_schema") != 1)
                           fail(label, "the live database was not restored to schema 1");
                       if (markers(w.db(), "2.0.0.0") != 0) fail(label, "B's rows are still in the live database");
                       auto preserved = find_backup(w, "rolled-back-");
                       if (!preserved) return fail(label, "the migrated database was not preserved");
                       if (!s->notice || !s->notice->preserved || preserved->stem().string() != s->notice->preserved->stem)
                           fail(label, "the notice does not name the preserved copy");
                       Db kept(*preserved);
                       if (kept.scalar("SELECT version FROM parties_schema") != 3 ||
                           kept.scalar("SELECT count(*) FROM markers WHERE build = '2.0.0.0'") < 1)
                           fail(label, "the preserved copy is not the migrated database");
                       if (!find_backup(w, "pre-")) fail(label, "no pre-candidate snapshot");
                   }});

    out.push_back({.name = "exit-data-unavailable",
                   .b_fault = "exit-data-unavailable",
                   .done = [](const World& w) {
                       auto s = w.state();
                       return s && settled(*s) && std::holds_alternative<Probation>(s->activation) &&
                              w.last_run_log().find("recovery shown: data_unavailable") != std::string::npos;
                   },
                   .verify = [](const World& w, const std::string& label) {
                       auto s = w.state();
                       const BuildRecord* b = record(*s, w.b);
                       if (!b || b->quarantined()) fail(label, "B was blamed for unavailable data");
                       if (s->good != w.a.entry.id()) fail(label, "good changed");
                   }});

    auto corrupt_a = [](World& w) {
        RunResult r = run_launcher(w, {{"PARTIES_TEST_TIMING", kTiming}, {"PARTIES_FAKE_RUN_MS", "300"}});
        if (r.exit_code != 0 || markers(w.db(), "1.0.0.0") != 1) fail(w.name, "the clean first run did not start A");
        std::ofstream(w.build_dir_of(w.a) / kMacClient.app_file, std::ios::binary | std::ios::app).put('\0');
    };
    out.push_back({.name = "corrupt-good-redownload",
                   .publish_b = false,
                   .prepare = corrupt_a,
                   .done = [](const World& w) {
                       auto s = w.state();
                       return s && settled(*s) && markers(w.db(), "1.0.0.0") >= 2;
                   },
                   .verify = [](const World& w, const std::string& label) {
                       if (w.state()->good != w.a.entry.id()) fail(label, "good changed");
                       if (sha256_file(w.build_dir_of(w.a) / kMacClient.app_file) != w.a.entry.file(kMacClient.app_file)->sha256)
                           fail(label, "the build was not sealed again");
                       if (w.full_log().find("found " + w.a.token() + " corrupt") == std::string::npos)
                           fail(label, "the front did not report the corrupt build");
                       if (w.full_log().find("recovery shown") != std::string::npos)
                           fail(label, "recovery was shown although the feed lists the build");
                       if (w.leftovers()) fail(label, "the damaged directory was left behind");
                   }});

    out.push_back({.name = "good-revoked",
                   .prepare = [](World& w) {
                       Release d = release("0.9.0.0", 1, 1);
                       w.b.entry.link_protocol = w.b.entry.min_supported_launcher_link = 99;
                       publish(w.feed, {&w.b, &d}, 2, {.revoked = {w.a.entry.version}});
                   },
                   .done = [](const World& w) {
                       auto s = w.state();
                       return s && settled(*s) && s->good.version == *Version::parse("0.9.0.0");
                   },
                   .verify = [](const World& w, const std::string& label) {
                       auto s = w.state();
                       if (s->find(w.b.entry.id()) || fs::exists(w.build_dir_of(w.b))) fail(label, "B was staged");
                       if (markers(w.db(), "0.9.0.0") < 1) fail(label, "D never ran");
                       const BuildRecord* a = record(*s, w.a);
                       if (a && !a->quarantined()) fail(label, "the revoked A can still be rolled back to");
                   }});

    out.push_back({.name = "revoked-to-previous",
                   .prepare = [](World& w) {
                       prove_b(w);
                       publish(w.feed, {&w.a}, 3, {.revoked = {w.b.entry.version}});
                   },
                   .done = [](const World& w) { return good_is(w, w.a); },
                   .verify = [](const World& w, const std::string& label) {
                       auto s = w.state();
                       const BuildRecord* b = record(*s, w.b);
                       if (b && !b->quarantined() && !s->revoked.contains(b->id.version))
                           fail(label, "the revoked B can still be rolled back to");
                   },
                   .kill_points = false});

    out.push_back({.name = "downgrade-now",
                   .b_fault = "wait-then-downgrade",
                   .b_schema = 3,
                   .b_floor = 3,
                   .prepare = [](World& w) {
                       prove_b(w);
                       publish(w.feed, {&w.a}, 1, {.channel = Channel::Beta});
                   },
                   .done = [](const World& w) {
                       auto s = w.state();
                       return s && settled(*s) && s->good == w.a.entry.id() && s->channel == Channel::Beta;
                   },
                   .verify = [](const World& w, const std::string& label) {
                       auto s = w.state();
                       if (s->previous_good != w.b.entry.id()) fail(label, "B is not kept as previous_good");
                       if (Db(w.db()).scalar("SELECT version FROM parties_schema") != 1)
                           fail(label, "the live database was not restored to schema 1");
                       if (markers(w.db(), "2.0.0.0") != 0) fail(label, "B's rows are in the restored database");
                       auto kept = find_backup(w, "pre-downgrade-");
                       if (!kept) return fail(label, "the live database was not preserved");
                       Db copy(*kept);
                       if (copy.scalar("SELECT version FROM parties_schema") != 3 ||
                           copy.scalar("SELECT count(*) FROM markers WHERE build = '2.0.0.0'") < 1)
                           fail(label, "the preserved copy is not B's database");
                       if (w.statuses().find("\"data_effect\":\"restores_backup\",\"backup_taken_at\"") == std::string::npos)
                           fail(label, "the offer did not name the backup's date");
                   }});

    auto seed_json = [](const World& w) { return read_file(w.install_root / "state" / "seed.json").value_or(""); };
    out.push_back({.name = "seed-adopt",
                   .publish_b = false,
                   .done = [](const World& w) { return good_is(w, w.a) && markers(w.db(), "1.0.0.0") >= 1; },
                   .verify = [seed_json](const World& w, const std::string& label) {
                       auto s = w.state();
                       if (s->builds.size() != 1 || s->channel != Channel::Stable) fail(label, "not a fresh Stable install of A");
                       if (seed_json(w).find(w.a.dir()) == std::string::npos) fail(label, "seed.json does not record A");
                       if (w.seed_log().find("adopted " + w.a.dir()) == std::string::npos) fail(label, "A was not adopted");
                       if (w.leftovers()) fail(label, "a staging directory was left behind");
                   },
                   .seed = "a",
                   .install_a = false});

    out.push_back({.name = "seed-stage",
                   .publish_b = false,
                   .prepare = [](World& w) {
                       if (run_launcher(w, base_env()).exit_code != 0 || markers(w.db(), "1.0.0.0") != 1)
                           fail(w.name, "A did not run before the newer seed");
                   },
                   .done = [](const World& w) { return good_is(w, w.b); },
                   .verify = [seed_json](const World& w, const std::string& label) {
                       auto s = w.state();
                       if (s->previous_good != w.a.entry.id()) fail(label, "A is not kept as previous_good");
                       if (markers(w.db(), "2.0.0.0") < 1) fail(label, "B never ran");
                       if (!find_backup(w, "pre-" + w.b.dir())) fail(label, "B ran without a pre-candidate snapshot");
                       if (seed_json(w).find(w.b.dir()) == std::string::npos) fail(label, "seed.json does not record B");
                   },
                   .seed = "b"});

    out.push_back({.name = "seed-keep",
                   .publish_b = false,
                   .done = [](const World& w) { return good_is(w, w.a) && markers(w.db(), "1.0.0.0") >= 1; },
                   .verify = [seed_json](const World& w, const std::string& label) {
                       if (w.state()->builds.size() != 1) fail(label, "installing the same seed again staged something");
                       if (w.seed_log().find("kept the installed builds") == std::string::npos)
                           fail(label, "the second install did not keep what was installed");
                       if (seed_json(w).find(w.a.dir()) == std::string::npos) fail(label, "seed.json does not record A");
                   },
                   .seed = "a"});
    return out;
}

// A stray process that inherited the front's data.lock outlives it (failure
// matrix row 34). The launcher must not decide the rollback on data that
// process may still change: it waits for the lock, then restores.
void stray_holds_data_lock() {
    const std::string label = "stray-holds-data-lock";
    World w(label, "2.0.0.0", 3, 3);
    if (install_seed(w, w.a, {}).exit_code != 0) return fail(label, "the first install of A failed");
    publish(w.feed, {&w.a, &w.b}, 1);
    auto env = base_env();
    env["PARTIES_FAKE_FAULT"] = "2.0.0.0:migrate-then-orphan";
    env["PARTIES_FAKE_ORPHAN_S"] = "8";
    const int before = g_failures;
    for (int run = 0; run < 6; ++run) {
        auto s = w.state();
        if (s && rolled_back_to_a(w, *s, FailureReason::CrashedBeforeHealthy)) break;
        run_launcher(w, env);
    }
    const std::string log = w.full_log();
    const size_t waited = log.find("waiting to read the live database: data.lock is held");
    const size_t restored = log.find("restored pre-");
    if (waited == std::string::npos) fail(label, "the launcher never met the stray's data.lock");
    if (restored == std::string::npos || restored < waited) fail(label, "the restore did not wait for the lock");
    if (log.find("DataTooNew") != std::string::npos) fail(label, "A was started on data it cannot read");
    auto s = w.state();
    if (!s || !rolled_back_to_a(w, *s, FailureReason::CrashedBeforeHealthy)) fail(label, "B was not rolled back");
    if (Db(w.db()).scalar("SELECT version FROM parties_schema") != 1 || markers(w.db(), "2.0.0.0") != 0)
        fail(label, "the live database was not restored");
    if (!find_backup(w, "rolled-back-")) fail(label, "B's database was not preserved");
    check_identity_everywhere(w, label);
    if (g_failures == before) quote_log(w, label);
    std::printf("%-26s %s\n", label.c_str(), g_failures == before ? "pass" : "FAIL");
}

// A restore that is due while a front holds data.lock fails, the effects stop
// there, and nothing touches the live database; the next start finishes it.
// The kill point leaves the rollback's Restoring committed, then this process
// holds data.lock the way a front does.
void restore_meets_held_lock() {
    const std::string label = "restore-meets-held-lock";
    const int before = g_failures;
    for (int n = 1; n < 40; ++n) {
        World w(label, "2.0.0.0", 3, 3);
        if (install_seed(w, w.a, {}).exit_code != 0) return fail(label, "the first install of A failed");
        publish(w.feed, {&w.a, &w.b}, 1);
        auto env = base_env();
        env["PARTIES_FAKE_FAULT"] = "2.0.0.0:migrate-then-crash";
        auto killing = env;
        killing["PARTIES_KILL_AT"] = std::to_string(n);
        killing["PARTIES_KILL_COUNTER"] = w.counter.string();
        RunResult first = run_launcher(w, killing);
        auto s = w.state();
        if (first.exit_code != kKilled) break;
        if (!s || !std::holds_alternative<Restoring>(s->operation)) continue;

        std::cout << "  -- " << label << ": killed at effect " << n << " with Restoring committed\n";
        std::optional<DataLock> front = DataLock::try_acquire(w.data_root);
        if (!front) return fail(label, "cannot take data.lock for the test");
        const Sha256 live_before = sha256_file(w.db());
        RunResult held = run_launcher(w, env);
        const std::string log = w.last_run_log();
        std::istringstream lines(log);
        for (std::string line; std::getline(lines, line);)
            if (line.find("effect failed") != std::string::npos || line.find("starting") != std::string::npos)
                std::cout << "  " << line << "\n";
        if (held.exit_code != 0) fail(label, "the launcher exited " + std::to_string(held.exit_code));
        if (log.find("effect failed: data.lock is held") == std::string::npos) fail(label, "the restore did not fail");
        if (log.find("starting 1.0.0.0") != std::string::npos || log.find("starting 2.0.0.0") != std::string::npos)
            fail(label, "an app started with the restore unfinished");
        s = w.state();
        if (!s || !std::holds_alternative<Restoring>(s->operation)) fail(label, "the restore was given up");
        if (s && s->notice && s->notice->kind == Notice::Kind::RestoreFailed) fail(label, "a RestoreFailed notice was raised");
        if (sha256_file(w.db()) != live_before || find_backup(w, "rolled-back-"))
            fail(label, "the live database was touched while data.lock was held");

        front.reset();
        for (int run = 0; run < 4 && !good_is(w, w.a); ++run) run_launcher(w, env);
        s = w.state();
        if (!s || !rolled_back_to_a(w, *s, FailureReason::CrashedBeforeHealthy))
            fail(label, "the next start did not finish the rollback");
        if (Db(w.db()).scalar("SELECT version FROM parties_schema") != 1) fail(label, "the live database was not restored");
        if (!find_backup(w, "rolled-back-")) fail(label, "B's database was not preserved");
        check_identity_everywhere(w, label);
        std::printf("%-26s %s\n", label.c_str(), g_failures == before ? "pass" : "FAIL");
        return;
    }
    fail(label, "no kill point left Restoring committed");
}

// ReportCrash writes a report about 30 s after a crash. None of these fronts
// should have one, since every fake crash is a SIGKILL.
void check_crash_reports() {
    std::this_thread::sleep_for(35s);
    const fs::path reports = fs::path(std::getenv("HOME")) / "Library" / "Logs" / "DiagnosticReports";
    std::error_code ec;
    int found = 0;
    for (const auto& item : fs::directory_iterator(reports, ec)) {
        const std::string name = item.path().filename().string();
        if (!name.starts_with("Parties") || item.path().extension() != ".ips") continue;
        const std::string text = read_file(item.path()).value_or("");
        for (int pid : g_fronts)
            if (text.find("\"pid\":" + std::to_string(pid) + ",") != std::string::npos ||
                text.find("\"pid\" : " + std::to_string(pid) + ",") != std::string::npos) {
                fail("crash-reports", "front " + std::to_string(pid) + " left " + name);
                ++found;
            }
    }
    std::printf("crash reports from %zu fronts: %d\n", g_fronts.size(), found);
}

// ---- boot: fronts LaunchServices starts, as a double-click does --------------

std::vector<std::string> front_flags(const World& w) {
    return {"--install-root=" + w.install_root.string(), "--data-root=" + w.data_root.string(),
            "--test-namespace=e2e" + std::to_string(getpid()), "--log-dir=" + w.logs.string(),
            "--feed-dir=" + w.feed.string()};
}

std::map<std::string, std::string> boot_env(const World& w) {
    auto env = base_env();
    env["PARTIES_TEST_ENV_PROBE"] = w.name;
    return env;
}

bool has(std::string_view text, std::string_view what) { return text.find(what) != std::string_view::npos; }

// $TMPDIR is under /var, which is /private/var: this matches either spelling.
std::string path_tail(const fs::path& path) {
    const std::string text = path.string();
    return text.starts_with("/private/") ? text.substr(8) : text;
}

size_t count(std::string_view text, std::string_view what) {
    size_t n = 0;
    for (size_t at = text.find(what); at != std::string_view::npos; at = text.find(what, at + what.size())) ++n;
    return n;
}

// The stderr of one host process, from its --log-dir file.
struct HostLog {
    int pid = 0;
    bool launcher = false;  // --role=launcher, else a front
    std::string text;
};

std::vector<HostLog> host_logs(const World& w) {
    std::vector<HostLog> out;
    std::error_code ec;
    for (const auto& item : fs::directory_iterator(w.logs, ec)) {
        const std::string name = item.path().filename().string();
        HostLog log;
        if (std::sscanf(name.c_str(), "front-%d.log", &log.pid) == 1) log.launcher = false;
        else if (std::sscanf(name.c_str(), "launcher-%d.log", &log.pid) == 1) log.launcher = true;
        else continue;
        log.text = read_file(item.path()).value_or("");
        out.push_back(std::move(log));
    }
    std::ranges::sort(out, {}, &HostLog::pid);
    return out;
}

std::vector<HostLog> fronts_with(const World& w, std::string_view what) {
    std::vector<HostLog> out;
    for (HostLog& log : host_logs(w))
        if (!log.launcher && has(log.text, what)) out.push_back(std::move(log));
    return out;
}

// The lines that tell a reader how each front started and what it ran.
void quote_boot(const World& w, const std::string& label) {
    const char* keys[] = {"[host] front pid", "[host] plan", "[host] launcher pid", "never listened", "failed boot",
                          "digest mismatch", "[host] alert", "[host] safe mode", "[host] relaunched", "launcher lost",
                          "activate_other", "recovery answered", "NSWorkspace answered", "[fake-client] pid",
                          "[fake-client] healthy"};
    std::cout << "  -- " << label << ": host processes\n";
    for (const HostLog& log : host_logs(w)) {
        std::istringstream lines(log.text);
        for (std::string line; std::getline(lines, line);)
            if (std::ranges::any_of(keys, [&](const char* k) { return has(line, k); }))
                std::cout << "  " << (log.launcher ? "launcher " : "front ") << log.pid << ": " << line << "\n";
    }
    std::istringstream lines(w.seed_log() + w.full_log());
    const char* launcher_keys[] = {"boot_ok", "move offered", "move answered", "moved to", "recovery shown"};
    for (std::string line; std::getline(lines, line);)
        if (std::ranges::any_of(launcher_keys, [&](const char* k) { return has(line, k); }))
            std::cout << "  launcher.log: " << line << "\n";
}

// `open <bundle> --args ...`, with no -n, so LaunchServices decides whether
// a running front is reused. Returns once every host process has exited;
// `during` runs while it waits.
bool open_bundle(World& w, const fs::path& bundle, const std::map<std::string, std::string>& env,
                 const std::function<void()>& during = {}) {
    w.log_mark = w.full_log().size();
    write_fake_env(w, env);
    std::vector<std::string> argv{"open"};
    for (const auto& [k, v] : env)
        if (!k.starts_with("PARTIES_FAKE_")) argv.insert(argv.end(), {"--env", k + "=" + v});
    argv.push_back(bundle.string());
    argv.push_back("--args");
    for (const std::string& flag : front_flags(w)) argv.push_back(flag);
    if (int rc = run_tool(argv, w.logs / "open.txt"); rc != 0) {
        fail(w.name, "open exited " + std::to_string(rc));
        return false;
    }
    const auto until = Clock::now() + 150s;
    while (!live_hosts().empty()) {
        if (during) during();
        if (Clock::now() > until) {
            fail(w.name, "host processes still ran 150 s after open");
            for (int pid : live_hosts()) kill(pid, SIGKILL);
            return false;
        }
        std::this_thread::sleep_for(100ms);
    }
    wait_hosts_gone(w, false);
    return true;
}

// The front that ran the app and reported healthy, supervised by a launcher.
std::optional<HostLog> supervised_app(const World& w, const Release& build) {
    for (HostLog& log : fronts_with(w, "[fake-client] healthy"))
        if (has(log.text, "build=" + build.entry.version.to_string()) && has(log.text, " supervised env_probe"))
            return std::move(log);
    return std::nullopt;
}

void expect_supervised(const World& w, const std::string& label, const Release& build) {
    auto app = supervised_app(w, build);
    if (!app) return fail(label, build.entry.version.to_string() + " did not run supervised and report healthy");
    if (!has(w.last_run_log(), "boot_ok sent")) fail(label, "no launcher sent boot_ok");
    if (!has(app->text, "[host] payload loaded")) fail(label, "the app front did not load the payload itself");
    if (!fronts_with(w, "[host] safe mode").empty()) fail(label, "a front went to safe mode");
}

// The front that booted the launcher is already running when the launcher
// first asks LaunchServices for one (createsNewApplicationInstance=NO).
void report_reuse(const World& w, const std::string& label) {
    const auto booting = fronts_with(w, "[host] launcher pid=");
    std::set<int> answered;
    for (const HostLog& log : host_logs(w)) {
        std::istringstream lines(log.text);
        for (std::string line; std::getline(lines, line);) {
            int pid = 0;
            if (const size_t at = line.find("NSWorkspace answered pid="); at != std::string::npos &&
                std::sscanf(line.c_str() + at, "NSWorkspace answered pid=%d", &pid) == 1)
                answered.insert(pid);
        }
    }
    for (const HostLog& front : booting)
        std::cout << "  -- " << label << ": LaunchServices "
                  << (answered.contains(front.pid) ? "reused the booting front " : "did not answer with the booting front ")
                  << front.pid << " (answered " << answered.size() << " open(s))\n";
}

// Window server entries owned by pid. Owner and bounds need no Screen
// Recording access, only the title does.
std::vector<std::string> windows_of(int pid) {
    std::vector<std::string> out;
    CFArrayRef list = CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly, kCGNullWindowID);
    for (CFIndex i = 0; list && i < CFArrayGetCount(list); ++i) {
        auto info = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(list, i));
        int owner = 0;
        auto number = static_cast<CFNumberRef>(CFDictionaryGetValue(info, kCGWindowOwnerPID));
        if (!number || !CFNumberGetValue(number, kCFNumberIntType, &owner) || owner != pid) continue;
        CGRect rect{};
        auto bounds = static_cast<CFDictionaryRef>(CFDictionaryGetValue(info, kCGWindowBounds));
        if (bounds) CGRectMakeWithDictionaryRepresentation(bounds, &rect);
        out.push_back(std::format("{:.0f}x{:.0f} at {:.0f},{:.0f}", rect.size.width, rect.size.height, rect.origin.x,
                                  rect.origin.y));
    }
    if (list) CFRelease(list);
    return out;
}

// A build B whose launcher is `launcher`, proven good under A's launcher, so
// that boot-contract.json names B's launcher as good and A's as previous_good.
void make_b_good(World& w, const fs::path& launcher) {
    w.b = release("2.0.0.0", 1, 1, launcher);
    if (install_seed(w, w.a, {}).exit_code != 0) return fail(w.name, "the first install of A failed");
    publish(w.feed, {&w.a, &w.b}, 1);
    prove_b(w);
    auto contract = w.contract();
    if (!contract || contract->good.dir != w.b.token() || !contract->previous_good ||
        contract->previous_good->dir != w.a.token())
        fail(w.name, "boot-contract.json does not name B good and A previous_good");
}

void append_byte(const fs::path& file) { std::ofstream(file, std::ios::binary | std::ios::app).put('\0'); }

// A's seed bundle on a read-only disk image, mounted under the world's root.
// The path of the bundle on it.
fs::path mount_image(World& w) {
    const fs::path image = w.root / "Parties.dmg", mount = w.root / "image";
    fs::create_directories(mount);
    must(run_tool({"hdiutil", "create", "-quiet", "-srcfolder", w.a.bundle.parent_path().string(), "-volname",
                   "Parties", "-format", "UDRO", "-ov", image.string()},
                  w.logs / "hdiutil.txt"),
         "create the disk image");
    must(run_tool({"hdiutil", "attach", "-readonly", "-nobrowse", "-noverify", "-mountpoint", mount.string(),
                   image.string()},
                  w.logs / "hdiutil.txt"),
         "attach the disk image");
    const fs::path nested = mount / w.a.bundle.parent_path().filename() / "Parties.app";
    return fs::exists(nested) ? nested : mount / "Parties.app";
}

void detach_image(World& w) { run_tool({"hdiutil", "detach", "-force", (w.root / "image").string()}, w.logs / "hdiutil.txt"); }

struct BootScenario {
    std::string name;
    std::function<void(World&, const std::string&)> run;
};

std::vector<BootScenario> boot_scenarios() {
    std::vector<BootScenario> out;

    out.push_back({"open-fresh", [](World& w, const std::string& label) {
                       publish(w.feed, {&w.a}, 1);
                       if (!open_bundle(w, w.a.bundle, boot_env(w))) return;
                       const auto installer = fronts_with(w, "[host] plan 2:");
                       if (installer.empty()) fail(label, "no front installed the seed");
                       else if (!has(installer.front().text, "[host] plan 3:"))
                           fail(label, "the front that installed the seed did not then boot a launcher");
                       if (!has(w.seed_log(), "seed " + w.a.dir() + " installed")) fail(label, "the seed was not installed");
                       if (!good_is(w, w.a)) fail(label, "A is not the good build");
                       expect_supervised(w, label, w.a);
                       report_reuse(w, label);
                   }});

    out.push_back({"open-installed", [](World& w, const std::string& label) {
                       if (install_seed(w, w.a, {}).exit_code != 0) return fail(label, "the first install of A failed");
                       publish(w.feed, {&w.a}, 1);
                       if (!open_bundle(w, w.a.bundle, boot_env(w))) return;
                       if (!fronts_with(w, "[host] plan 2:").empty()) fail(label, "a front installed the seed again");
                       if (fronts_with(w, "[host] plan 3:").empty()) fail(label, "no front booted a launcher");
                       expect_supervised(w, label, w.a);
                       report_reuse(w, label);
                   }});

    // Two failed boots of good's launcher, then previous_good's launcher
    // supervises the good build's app.
    auto falls_back = [](std::string name, std::function<fs::path()> launcher, std::string boot_wait_ms) {
        return BootScenario{name, [launcher, boot_wait_ms](World& w, const std::string& label) {
                                make_b_good(w, launcher());
                                auto env = boot_env(w);
                                env["PARTIES_TEST_BOOT_WAIT_MS"] = boot_wait_ms;
                                if (!open_bundle(w, w.a.bundle, env)) return;
                                const auto booting = fronts_with(w, "[host] launcher pid=");
                                if (booting.size() != 1) return fail(label, "expected one booting front");
                                const std::string& text = booting.front().text;
                                if (count(text, "[host] failed boot recorded") != 2)
                                    fail(label, "B's launcher did not fail exactly twice");
                                if (count(text, "from " + w.b.token()) != 2 || count(text, "from " + w.a.token()) != 1)
                                    fail(label, "the front did not boot B's launcher twice, then A's");
                                expect_supervised(w, label, w.b);
                            }};
    };
    out.push_back(falls_back("launcher-exits", [] { return g_broken_exits; }, "300000"));
    out.push_back(falls_back("launcher-hangs", [] { return g_broken_hangs; }, "6000"));

    out.push_back({"launcher-digest", [](World& w, const std::string& label) {
                       make_b_good(w, g_broken_exits);
                       append_byte(w.build_dir_of(w.b) / kMacClient.launcher_file);
                       if (!open_bundle(w, w.a.bundle, boot_env(w))) return;
                       const auto booting = fronts_with(w, "[host] launcher pid=");
                       if (booting.size() != 1) return fail(label, "expected one booting front");
                       const std::string& text = booting.front().text;
                       if (has(text, "[host] failed boot recorded")) fail(label, "a launcher that does not match counted as a failed boot");
                       if (!has(text, "wait status " + std::to_string(70 << 8)))
                           fail(label, "B's launcher process did not exit 70");
                       if (count(text, "from " + w.a.token()) != 1) fail(label, "A's launcher was not booted next");
                       bool mismatch = false;
                       for (const HostLog& log : host_logs(w)) mismatch = mismatch || (log.launcher && has(log.text, "digest mismatch"));
                       if (!mismatch) fail(label, "no launcher process reported the digest mismatch");
                       expect_supervised(w, label, w.b);
                   }});

    // Both launchers unusable. First no answer is scripted, so the host draws
    // its NSAlert before [NSApp run], and this process looks for its window.
    // Then Start the last working version runs B's app with no launcher.
    out.push_back({"both-broken", [](World& w, const std::string& label) {
                       make_b_good(w, g_broken_exits);
                       append_byte(w.build_dir_of(w.a) / kMacClient.launcher_file);
                       auto env = boot_env(w);
                       env.erase("PARTIES_TEST_RECOVERY");
                       std::vector<std::string> seen;
                       int alert_pid = 0;
                       bool looked = false;
                       Clock::time_point alert_at;
                       open_bundle(w, w.a.bundle, env, [&] {
                           if (looked) return;
                           if (!alert_pid) {
                               auto shown = fronts_with(w, "[host] alert shown: Parties could not start");
                               if (shown.empty()) return;
                               alert_pid = shown.front().pid;
                               alert_at = Clock::now();
                           }
                           seen = windows_of(alert_pid);
                           if (seen.empty() && Clock::now() - alert_at < 15s) return;
                           looked = true;
                           std::cout << "  -- " << label << ": front " << alert_pid << " has " << seen.size()
                                     << " window(s) on screen while its NSAlert runs"
                                     << (seen.empty() ? "" : ", the first " + seen.front()) << "\n";
                           kill(alert_pid, SIGKILL);
                       });
                       if (!alert_pid) fail(label, "no front drew the safe mode alert");
                       else if (seen.empty()) fail(label, "the safe mode NSAlert put no window on screen");
                       const long long b_runs = markers(w.db(), "2.0.0.0");
                       const size_t launcher_log = w.full_log().size();

                       env["PARTIES_TEST_RECOVERY"] = "start_last_good";
                       if (!open_bundle(w, w.a.bundle, env)) return;
                       if (fronts_with(w, "[host] alert answered by PARTIES_TEST_RECOVERY: Parties could not start").empty())
                           fail(label, "the scripted answer did not reach the safe mode alert");
                       if (fronts_with(w, "[host] safe mode: start_last_good").empty())
                           fail(label, "safe mode did not start the last good app");
                       const auto unsupervised = fronts_with(w, "safe mode runs " + w.b.token() + " unsupervised");
                       if (unsupervised.size() != 1) return fail(label, "no front ran B's app in safe mode");
                       const std::string& text = unsupervised.front().text;
                       if (!has(text, "unsupervised env_probe=" + w.name))
                           fail(label, "the --safe-mode front did not get its environment through NSWorkspaceOpenConfiguration");
                       if (!has(text, "[fake-client] healthy")) fail(label, "B's app did not run to health in safe mode");
                       if (markers(w.db(), "2.0.0.0") != b_runs + 1) fail(label, "B's app did not open the data once");
                       if (w.full_log().size() != launcher_log) fail(label, "a launcher ran in safe mode");
                   }});

    // The launcher dies while its app runs and before boot_ok: the front
    // relaunches itself with --boot-round=1, which chooses again.
    out.push_back({"boot-ok-lost", [](World& w, const std::string& label) {
                       if (install_seed(w, w.a, {}).exit_code != 0) return fail(label, "the first install of A failed");
                       publish(w.feed, {&w.a}, 1);
                       auto env = boot_env(w);
                       env["PARTIES_FAKE_FAULT"] = "1.0.0.0:slow-health";
                       int killed = 0;
                       open_bundle(w, w.a.bundle, env, [&] {
                           if (killed) return;
                           for (const HostLog& front : fronts_with(w, "[host] payload loaded")) {
                               int pid = 0;
                               const size_t at = front.text.find("[host] launcher pid=");
                               if (at == std::string::npos || std::sscanf(front.text.c_str() + at, "[host] launcher pid=%d", &pid) != 1)
                                   continue;
                               if (has(w.last_run_log(), "boot_ok sent")) return fail(label, "boot_ok came before the kill");
                               kill(pid, SIGKILL);
                               killed = pid;
                               std::cout << "  -- " << label << ": killed launcher " << pid << " under front " << front.pid << "\n";
                               return;
                           }
                       });
                       if (!killed) return fail(label, "the app never loaded under a booted launcher");
                       const auto lost = fronts_with(w, "[host] launcher lost");
                       if (lost.empty() || !has(lost.front().text, "[host] failed boot recorded") ||
                           !has(lost.front().text, "relaunched with --boot-round=1"))
                           fail(label, "the front did not count the lost launcher and relaunch itself with --boot-round=1");
                       const auto round_1 = fronts_with(w, "boot round 1");
                       if (round_1.empty() || !has(round_1.front().text, "[host] launcher pid="))
                           fail(label, "the --boot-round=1 front did not boot a launcher");
                       expect_supervised(w, label, w.a);
                   }});

    // A read-only disk image: the launcher offers the move, Not Now keeps
    // Parties running from the image, and the update to B happens there.
    out.push_back({"read-only-image", [](World& w, const std::string& label) {
                       const fs::path bundle = mount_image(w);
                       publish(w.feed, {&w.a, &w.b}, 1);
                       auto env = boot_env(w);
                       env["PARTIES_TEST_RECOVERY"] = "keep";
                       for (int run = 0; run < 8 && !good_is(w, w.b); ++run) open_bundle(w, bundle, env);
                       const std::string launcher_log = w.full_log();
                       detach_image(w);
                       if (count(launcher_log, "move offered: Parties runs from ") != 1 || !has(launcher_log, path_tail(bundle)))
                           fail(label, "the move was not offered exactly once");
                       if (!has(launcher_log, "move answered: keep")) fail(label, "Not Now was not the answer");
                       if (!fs::exists(w.install_root / "state" / "move-declined")) fail(label, "Not Now was not recorded");
                       if (!good_is(w, w.b)) fail(label, "B did not become good from the image");
                       for (const HostLog& front : host_logs(w))
                           if (!front.launcher && has(front.text, "[host] front pid=") &&
                               !has(front.text, path_tail(bundle) + " boot round"))
                               fail(label, "front " + std::to_string(front.pid) + " did not run from the image");
                       expect_supervised(w, label, w.b);
                   }});

    // Move to Applications: the copy boots a launcher of its own.
    out.push_back({"move-to-applications", [](World& w, const std::string& label) {
                       const fs::path bundle = mount_image(w);
                       publish(w.feed, {&w.a}, 1);
                       const fs::path applications = w.root / "Applications";
                       fs::create_directories(applications);
                       auto env = boot_env(w);
                       env["PARTIES_TEST_RECOVERY"] = "move";
                       env["PARTIES_TEST_APPLICATIONS_DIR"] = applications.string();
                       open_bundle(w, bundle, env);
                       detach_image(w);
                       const fs::path moved = applications / "Parties.app";
                       if (!has(w.full_log(), "moved to ") || !has(w.full_log(), path_tail(moved)))
                           fail(label, "the launcher did not move the bundle");
                       const auto copies = fronts_with(w, path_tail(moved) + " boot round");
                       if (copies.empty() || !has(copies.front().text, "[host] launcher pid="))
                           return fail(label, "the moved bundle did not boot a launcher of its own");
                       auto app = supervised_app(w, w.a);
                       if (!app || !has(app->text, path_tail(moved) + " boot round"))
                           fail(label, "A did not run supervised from the moved bundle");
                       if (fs::exists(w.install_root / "state" / "move-declined")) fail(label, "the move was recorded as declined");
                   }});
    return out;
}

void run_boot_scenario(const BootScenario& sc) {
    const auto t0 = Clock::now();
    const int before = g_failures;
    {
        World w(sc.name, "2.0.0.0", 1, 1);
        sc.run(w, sc.name);
        quote_boot(w, sc.name);
    }
    std::printf("%-26s %-6s %s %.0fs\n", sc.name.c_str(), g_failures == before ? "pass" : "FAIL", "no kill sweep",
                std::chrono::duration<double>(Clock::now() - t0).count());
    std::fflush(stdout);
}

} // namespace

int main(int argc, char** argv) {
    std::optional<std::string> only;
    bool kill_points = true;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--app" && i + 1 < argc) g_app = argv[++i];
        else if (a == "--entitlements" && i + 1 < argc) g_entitlements = argv[++i];
        else if (a == "--fake-client" && i + 1 < argc) g_fake_client = argv[++i];
        else if (a == "--launcher" && i + 1 < argc) g_launcher = argv[++i];
        else if (a == "--broken-launcher-exits" && i + 1 < argc) g_broken_exits = argv[++i];
        else if (a == "--broken-launcher-hangs" && i + 1 < argc) g_broken_hangs = argv[++i];
        else if (a == "--only" && i + 1 < argc) only = argv[++i];
        else if (a == "--no-kill") kill_points = false;
        else if (a == "--verbose") g_verbose = true;
    }
    if (g_app.empty() || g_entitlements.empty() || g_fake_client.empty() || g_launcher.empty() ||
        g_broken_exits.empty() || g_broken_hangs.empty()) {
        std::cerr << "usage: update_macos_e2e_test --app <Parties.app> --entitlements <file> --fake-client <dylib> "
                     "--launcher <dylib> --broken-launcher-exits <dylib> --broken-launcher-hangs <dylib> "
                     "[--only <name>] [--no-kill] [--verbose]\n";
        return 2;
    }
    if (!parties::crypto_init()) return 2;
    const char* tmp = std::getenv("TMPDIR");
    g_work = fs::path(tmp ? tmp : "/tmp") / ("parties-macos-e2e-" + std::to_string(getpid()));
    fs::remove_all(g_work);
    fs::create_directories(g_work);
    g_work = fs::canonical(g_work);
    std::cout << "work directory " << g_work.string() << "\n";
    prepare_base();

    const auto started = Clock::now();
    std::printf("%-26s %-6s %s\n", "scenario", "plain", "kill points");
    std::fflush(stdout);
    int total_kills = 0;
    for (const Scenario& sc : scenarios()) {
        if (only && *only != sc.name) continue;
        const auto t0 = Clock::now();
        int before = g_failures;
        bool plain = run_scenario(sc, std::nullopt) == Outcome::Converged && g_failures == before;
        int kills = 0;
        bool kills_ok = true;
        if (kill_points && sc.kill_points && plain) {
            for (int n = 1; n < 200; ++n) {
                int b = g_failures;
                Outcome o = run_scenario(sc, n);
                if (o == Outcome::KillNotReached) break;
                ++kills;
                if (o != Outcome::Converged || g_failures != b) kills_ok = false;
            }
        }
        total_kills += kills;
        const double seconds = std::chrono::duration<double>(Clock::now() - t0).count();
        std::printf("%-26s %-6s %s (%d) %.0fs\n", sc.name.c_str(), plain ? "pass" : "FAIL",
                    !kill_points || !sc.kill_points ? "skipped" : kills_ok && plain ? "pass" : "FAIL", kills, seconds);
        std::fflush(stdout);
    }
    if (!only || *only == "stray-holds-data-lock") stray_holds_data_lock();
    if (!only || *only == "restore-meets-held-lock") restore_meets_held_lock();
    std::printf("kill points exercised: %d\n", total_kills);
    for (const BootScenario& sc : boot_scenarios())
        if (!only || *only == sc.name) run_boot_scenario(sc);
    check_crash_reports();
    std::printf("total %.0fs, %s\n", std::chrono::duration<double>(Clock::now() - started).count(),
                g_failures ? "FAILED" : "passed");
    if (g_failures == 0) {
        std::error_code ec;
        fs::remove_all(g_work, ec);
    }
    return g_failures ? 1 : 0;
}
