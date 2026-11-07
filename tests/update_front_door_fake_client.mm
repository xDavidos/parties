// A stand-in for PartiesClient.dylib, in two end-to-end tests.
//
// update_front_door_e2e: the file "scenario" in the directory it was loaded
// from names what it does once AppKit has finished launching.
//
// update_macos_e2e: a sealed build directory has no "scenario" but the
// build.json the launcher wrote, so one dylib plays every build: version and
// data schema come from that entry. It opens parties.db in data_dir as the
// real client does, records a marker row per run, and talks the link. Faults
// are chosen per version by PARTIES_FAKE_FAULT=<version>:<fault>[;...], read
// with PARTIES_FAKE_RUN_MS and PARTIES_FAKE_STATUS_LOG from the file
// "fake-env" in data_dir, because LaunchServices starts this process without
// the harness's environment. Every status the launcher sends is appended to
// the status log. With no link (host safe mode) it runs without a launcher.

#import <AppKit/AppKit.h>

#include <parties/host_api.h>
#include <parties/update/data_compat.h>
#include <parties/update/link.h>
#include <parties/update/manifest.h>

#include <sqlite3.h>

#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <thread>

namespace update = parties::update;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

const PartiesHostApi* g_host = nullptr;

void send_link(const update::link::AppMessage& message) {
    const std::string line = update::link::write(message);
    if (g_host->link_fd >= 0) send(g_host->link_fd, line.data(), line.size(), 0);
}

[[noreturn]] void hang() {
    for (;;) pause();
}

// A crash ReportCrash does not report on: no exiting{code}, so Abnormal.
[[noreturn]] void crash() {
    kill(getpid(), SIGKILL);
    hang();
}

void quit_app() {
    dispatch_async(dispatch_get_main_queue(), ^{
        [NSApp terminate:nil];
    });
}

void run(const std::string& scenario) {
    const std::map<std::string, int> exit_codes{{"exit2", 2}, {"exit3", 3}, {"exit4", 4}, {"exit7", 7}};
    if (auto it = exit_codes.find(scenario); it != exit_codes.end()) g_host->exit_process(it->second);

    if (scenario == "clean") [NSApp terminate:nil];
    if (scenario == "healthy_then_quit") {
        send_link(update::link::Hello{update::kLinkProtocol, update::kMinLauncherLink, *update::Version::parse("0.0.0.1")});
        send_link(update::link::Healthy{1});
        [NSApp terminate:nil];
    }
    if (scenario == "return") {
        [NSApp stop:nil];
        [NSApp postEvent:[NSEvent otherEventWithType:NSEventTypeApplicationDefined location:NSZeroPoint
                                       modifierFlags:0 timestamp:0 windowNumber:0 context:nil
                                             subtype:0 data1:0 data2:0]
                 atStart:YES];
    }
    if (scenario == "crash") *static_cast<volatile int*>(nullptr) = 1;
    if (scenario == "abort") std::abort();
    if (scenario == "stray_exit") std::exit(0);
    if (scenario == "hang") hang();
    if (scenario == "teardown_hang") {
        std::atexit([] { hang(); });
        [NSApp terminate:nil];
    }
    // "wait": stay up until killed or told to quit.
}

// ---- update_macos_e2e -------------------------------------------------------

std::map<std::string, std::string> read_env(const fs::path& file) {
    std::map<std::string, std::string> out;
    std::ifstream in(file);
    for (std::string line; std::getline(in, line);) {
        size_t eq = line.find('=');
        if (eq != std::string::npos) out[line.substr(0, eq)] = line.substr(eq + 1);
    }
    return out;
}

std::string fault_for(const std::string& spec, const std::string& version) {
    std::stringstream items(spec);
    std::string item;
    while (std::getline(items, item, ';')) {
        size_t colon = item.find(':');
        if (colon != std::string::npos && item.substr(0, colon) == version) return item.substr(colon + 1);
    }
    return {};
}

class Link {
public:
    std::optional<update::link::LauncherMessage> poll_for(std::chrono::milliseconds wait) {
        for (;;) {
            if (auto line = reader_.next()) {
                if (auto m = update::link::parse_launcher_message(*line)) return m;
                continue;
            }
            pollfd p{g_host->link_fd, POLLIN, 0};
            if (::poll(&p, 1, int(wait.count())) <= 0) return std::nullopt;
            char buffer[4096];
            ssize_t n = recv(g_host->link_fd, buffer, sizeof buffer, 0);
            if (n <= 0) {
                lost_ = true;
                return std::nullopt;
            }
            reader_.append({buffer, size_t(n)});
        }
    }
    bool lost() const { return lost_; }

private:
    update::link::LineReader reader_;
    bool lost_ = false;
};

bool exec(sqlite3* db, const std::string& sql) { return sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK; }

bool has_table(sqlite3* db, const char* name) {
    sqlite3_stmt* stmt = nullptr;
    bool found = sqlite3_prepare_v2(db, "SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = ?", -1, &stmt,
                                    nullptr) == SQLITE_OK &&
                 sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC) == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW;
    sqlite3_finalize(stmt);
    return found;
}

std::optional<update::DataInfo> read_schema(sqlite3* db) {
    if (has_table(db, "identity") && !has_table(db, "parties_schema")) return update::DataInfo{1, 1};
    sqlite3_stmt* stmt = nullptr;
    std::optional<update::DataInfo> info;
    if (sqlite3_prepare_v2(db, "SELECT version, compat_floor FROM parties_schema WHERE id = 1", -1, &stmt, nullptr) ==
            SQLITE_OK &&
        sqlite3_step(stmt) == SQLITE_ROW)
        info = update::DataInfo{uint32_t(sqlite3_column_int(stmt, 0)), uint32_t(sqlite3_column_int(stmt, 1))};
    sqlite3_finalize(stmt);
    return info;
}

// The real client's data rules: exit 3 when the data cannot be opened, 4
// when it is too new, migrate forward otherwise.
uint32_t open_data(const fs::path& path, const update::BuildEntry& build, sqlite3*& db) {
    const bool fresh = !fs::exists(path);
    if (sqlite3_open(path.c_str(), &db) != SQLITE_OK) g_host->exit_process(3);
    exec(db, "PRAGMA journal_mode=WAL");
    const std::string schema = std::to_string(build.data_schema), floor = std::to_string(build.data_compat_floor);
    if (fresh) {
        bool ok = exec(db, "BEGIN") &&
                  exec(db, "CREATE TABLE identity (id INTEGER PRIMARY KEY CHECK (id = 1), public_key TEXT NOT NULL)") &&
                  exec(db, "INSERT INTO identity VALUES (1, 'fake-identity-key')") &&
                  exec(db, "CREATE TABLE markers (n INTEGER PRIMARY KEY, build TEXT NOT NULL, opened_at INTEGER NOT NULL)") &&
                  exec(db, "CREATE TABLE parties_schema (id INTEGER PRIMARY KEY CHECK (id = 1), version INTEGER NOT NULL, "
                           "compat_floor INTEGER NOT NULL)") &&
                  exec(db, "INSERT INTO parties_schema VALUES (1, " + schema + ", " + floor + ")") && exec(db, "COMMIT");
        if (!ok) g_host->exit_process(3);
        return build.data_schema;
    }
    std::optional<update::DataInfo> info = read_schema(db);
    if (!info) g_host->exit_process(3);
    switch (update::compatibility(build.data_schema, *info)) {
    case update::Compatibility::Incompatible: g_host->exit_process(4); return 0;
    case update::Compatibility::MigratesForward: {
        const std::string kept = std::to_string(std::max(info->compat_floor, build.data_compat_floor));
        if (!exec(db, "CREATE TABLE IF NOT EXISTS parties_schema (id INTEGER PRIMARY KEY CHECK (id = 1), "
                      "version INTEGER NOT NULL, compat_floor INTEGER NOT NULL)") ||
            !exec(db, "INSERT OR REPLACE INTO parties_schema VALUES (1, " + schema + ", " + kept + ")"))
            g_host->exit_process(2);
        break;
    }
    case update::Compatibility::ReadsAsIs: break;
    }
    if (!exec(db, "CREATE TABLE IF NOT EXISTS markers (n INTEGER PRIMARY KEY, build TEXT NOT NULL, "
                  "opened_at INTEGER NOT NULL)"))
        g_host->exit_process(2);
    return info->version;
}

void play_build(const update::BuildEntry& build) {
    namespace link = update::link;
    const fs::path data_dir = g_host->data_dir;
    auto env = read_env(data_dir / "fake-env");
    const std::string version = build.version.to_string();
    const std::string fault = fault_for(env["PARTIES_FAKE_FAULT"], version);
    const int run_ms = std::atoi(env["PARTIES_FAKE_RUN_MS"].c_str());
    const std::string status_log = env["PARTIES_FAKE_STATUS_LOG"];
    // PARTIES_TEST_ENV_PROBE reaches a front only through the environment
    // LaunchServices was given for it.
    const char* probe = std::getenv("PARTIES_TEST_ENV_PROBE");
    std::fprintf(stderr, "[fake-client] pid=%d build=%s fault=%s %s env_probe=%s\n", getpid(), version.c_str(),
                 fault.c_str(), g_host->link_fd >= 0 ? "supervised" : "unsupervised", probe ? probe : "-");

    Link peer;
    if (g_host->link_fd >= 0) {
        send_link(link::Hello{update::kLinkProtocol, update::kMinLauncherLink, build.version});
        bool welcomed = false;
        for (auto until = std::chrono::steady_clock::now() + 10s; !welcomed && std::chrono::steady_clock::now() < until;) {
            auto m = peer.poll_for(100ms);
            welcomed = m && std::holds_alternative<link::Welcome>(*m);
            if (peer.lost()) break;
        }
        if (!welcomed) g_host->exit_process(2);
    }

    if (fault == "exit-init-failed") g_host->exit_process(2);
    if (fault == "exit-data-unavailable") g_host->exit_process(3);
    if (fault == "crash-before-health") crash();
    if (fault == "hang-before-health") hang();

    sqlite3* db = nullptr;
    const uint32_t opened_at = open_data(data_dir / "parties.db", build, db);
    exec(db, "INSERT INTO markers (build, opened_at) VALUES ('" + version + "', " + std::to_string(opened_at) + ")");
    if (fault == "migrate-then-crash") crash();
    if (fault == "migrate-then-orphan") {
        // A stray process that inherited data.lock outlives this front.
        if (fork() == 0) {
            sleep(unsigned(std::atoi(env["PARTIES_FAKE_ORPHAN_S"].c_str())));
            _exit(0);
        }
        crash();
    }

    if (fault == "slow-health") std::this_thread::sleep_for(2s);
    send_link(link::Healthy{build.data_schema});
    std::fprintf(stderr, "[fake-client] healthy\n");
    if (fault == "crash-after-health") {
        std::this_thread::sleep_for(100ms);
        crash();
    }

    const bool changes_mind = fault == "downgrade-then-stable";
    const bool downgrades = fault == "wait-then-downgrade" || changes_mind;
    if (downgrades) send_link(link::Request{link::SetChannel{update::Channel::Beta, link::ChannelChoice::Wait}});
    bool restart_asked = false, downgrade_asked = false, checking = false, answered = false;
    // These faults assert on what the check told the app, so the app stays
    // until the check of its channel answers, however long that takes.
    const bool until_checked = fault == "run-until-checked" || downgrades;
    const update::Channel channel = downgrades ? update::Channel::Beta : update::Channel::Stable;
    const auto started = std::chrono::steady_clock::now();
    auto running = [&] {
        auto elapsed = std::chrono::steady_clock::now() - started;
        if (until_checked && !answered) return elapsed < 30s;
        return elapsed < std::chrono::milliseconds(run_ms > 0 ? run_ms : 1000);
    };
    while (running() && !peer.lost()) {
        auto m = peer.poll_for(5ms);
        if (!m) continue;
        if (std::holds_alternative<link::ExitRequested>(*m)) break;
        auto* status = std::get_if<link::Status>(&*m);
        if (!status) continue;
        if (!status_log.empty()) std::ofstream(status_log, std::ios::app) << link::write(*m);
        if (status->phase == link::Phase::Checking) checking = true;
        else if (checking && status->channel == channel) answered = true;
        if (downgrades && !downgrade_asked && status->phase == link::Phase::Pinned && status->downgrade) {
            downgrade_asked = true;
            send_link(link::Request{link::SetChannel{update::Channel::Beta, link::ChannelChoice::DowngradeNow}});
            if (changes_mind) send_link(link::Request{link::SetChannel{update::Channel::Stable, link::ChannelChoice::Wait}});
        }
        if (!restart_asked && status->phase == link::Phase::ReadyToRestart) {
            restart_asked = true;
            send_link(link::Request{link::RestartToUpdate{}});
        }
    }
    sqlite3_close(db);
    quit_app();
}

} // namespace

@interface FakeClientDelegate : NSObject <NSApplicationDelegate>
@end

@implementation FakeClientDelegate
- (void)applicationDidFinishLaunching:(NSNotification*)notification {
    const fs::path build_dir = g_host->build_dir;
    std::string scenario;
    std::ifstream(build_dir / "scenario") >> scenario;
    std::fprintf(stderr, "[fake-client] pid=%d scenario=%s link_fd=%d data_dir=%s\n", getpid(), scenario.c_str(),
                 g_host->link_fd, g_host->data_dir);
    if (!scenario.empty()) return run(scenario);

    std::ifstream in(build_dir / "build.json");
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    auto build = update::parse_build_entry(text, update::kMacClient);
    if (!build) g_host->exit_process(2);
    // Off the main thread, so a lost launcher's terminate: is answered at once.
    std::thread([entry = *build] { play_build(entry); }).detach();
}
@end

extern "C" const uint32_t parties_client_min_host_abi = 1;

extern "C" int parties_client_main(const PartiesHostApi* host) {
    g_host = host;
    NSApplication* app = [NSApplication sharedApplication];
    app.delegate = [[FakeClientDelegate alloc] init];
    [app run];
    std::fprintf(stderr, "[fake-client] run loop returned\n");
    return 0;
}
