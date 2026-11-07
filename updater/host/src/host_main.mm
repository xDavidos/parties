// The macOS host: the bundle's main executable. A front asks the launcher
// which build to run and loads that payload into this process, so
// LaunchServices, the Dock and TCC only ever see this executable. With no
// launcher listening, the front boots one as Parties.exe does on Windows.

#import <AppKit/AppKit.h>

#include "front_door_client.h"
#include "payload.h"

#include <client/exit_codes.h>
#include <parties/data_lock.h>
#include <parties/front_door_socket.h>
#include <parties/macos_layout.h>
#include <parties/update/boot.h>
#include <parties/update/front_start.h>
#include <parties/update/product.h>

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

extern char** environ;

namespace fs = std::filesystem;
namespace boot = parties::update::boot;
namespace door = parties::host::front_door;
namespace layout = parties::update::macos;
namespace protocol = parties::update::front_door;
namespace start = parties::update::front_start;
using parties::client::ExitCode;
using parties::update::kMacClient;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

namespace {

#ifdef PARTIES_UPDATE_TESTING
constexpr bool kTesting = true;
#else
constexpr bool kTesting = false;
#endif

constexpr parties::host::Entry kClientEntry{"parties_client_min_host_abi", "parties_client_main"};
constexpr parties::host::Entry kLauncherEntry{"parties_launcher_min_host_abi", "parties_launcher_main"};
// The booting front reads it as "this launcher is unusable", not as a failed boot.
constexpr int kLauncherUnusable = 70;

start::HostArgs g_args;

// The launcher this front started, until it sends boot_ok.
struct Boot {
    int lock = -1;       // state/boot.lock: one front chooses at a time
    pid_t launcher = 0;  // 0: none, or it is reaped
    int boot_ok = -1;    // the read end of its --boot-ok-fd
    bool ok = false;
    boot::BootRecord record;
};
Boot g_boot;

fs::path app_support() {
    return NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory, NSUserDomainMask, YES)
        .firstObject.fileSystemRepresentation;
}

fs::path state_dir() { return fs::path(g_args.install_root) / "state"; }
fs::path own_bundle() { return NSBundle.mainBundle.bundlePath.fileSystemRepresentation; }

template <class Parse>
auto load(const fs::path& path, Parse parse) -> decltype(parse(std::string_view{})) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    return parse(std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()));
}

// A failed write is not fatal: starting Parties matters more than the bookkeeping.
void persist(const fs::path& path, const std::string& text) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    const std::string tmp = path.string() + ".tmp";
    int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return;
    const bool ok = write(fd, text.data(), text.size()) == ssize_t(text.size()) && fcntl(fd, F_FULLFSYNC) == 0;
    close(fd);
    if (!ok || rename(tmp.c_str(), path.c_str()) != 0) unlink(tmp.c_str());
}

// Every front starts through LaunchServices, the one that replaces this
// process too. --after makes it wait until this process is gone.
void relaunch(const fs::path& bundle, std::vector<std::string> extra) {
    const auto arguments = start::relaunch_arguments(g_args, std::move(extra), getpid());
    NSWorkspaceOpenConfiguration* configuration = [NSWorkspaceOpenConfiguration configuration];
    NSMutableArray<NSString*>* list = [NSMutableArray array];
    for (const std::string& arg : arguments) [list addObject:@(arg.c_str())];
    configuration.arguments = list;
    configuration.createsNewApplicationInstance = YES;
    // LaunchServices starts it with launchd's environment otherwise.
    NSMutableDictionary<NSString*, NSString*>* environment = [NSMutableDictionary dictionary];
    [NSProcessInfo.processInfo.environment enumerateKeysAndObjectsUsingBlock:^(NSString* key, NSString* value, BOOL*) {
        if (kTesting && [key hasPrefix:@"PARTIES_TEST_"]) environment[key] = value;
    }];
    configuration.environment = environment;
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    [NSWorkspace.sharedWorkspace openApplicationAtURL:[NSURL fileURLWithPath:@(bundle.c_str()) isDirectory:YES]
                                        configuration:configuration
                                    completionHandler:^(NSRunningApplication* app, NSError* error) {
        std::fprintf(stderr, "[host] relaunched with %s: %s\n", arguments.front().c_str(),
                     app ? std::to_string(app.processIdentifier).c_str() : error.description.UTF8String);
        dispatch_semaphore_signal(done);
    }];
    dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC));
}

// The id of the option the user picked, or kQuit when the alert is closed.
// Open the data folder and Download act here and show the alert again.
std::string ask(const start::Alert& alert, const fs::path& data_dir) {
    const char* scripted = kTesting ? std::getenv("PARTIES_TEST_RECOVERY") : nullptr;
    std::fprintf(stderr, "[host] alert %s: %s\n", scripted ? "answered by PARTIES_TEST_RECOVERY" : "shown", alert.title.c_str());
    if (scripted) return scripted;
    [NSApplication sharedApplication];
    for (;;) {
        NSAlert* panel = [[NSAlert alloc] init];
        panel.messageText = @(alert.title.c_str());
        panel.informativeText = @(alert.message.c_str());
        for (const auto& option : alert.options) [panel addButtonWithTitle:@(option.label.c_str())];
        const size_t pick = size_t([panel runModal] - NSAlertFirstButtonReturn);
        const std::string id = pick < alert.options.size() ? alert.options[pick].id : std::string(start::kQuit);
        if (id == start::kOpenData) [NSWorkspace.sharedWorkspace openURL:[NSURL fileURLWithPath:@(data_dir.c_str())]];
        else if (id == start::kDownload) [NSWorkspace.sharedWorkspace openURL:[NSURL URLWithString:@(start::kReleasesUrl)]];
        else return id;
    }
}

// Host safe mode, the twin of Parties.exe's dialog, when no launcher starts.
[[noreturn]] void safe_mode(bool can_start) {
    const auto contract = load(state_dir() / "boot-contract.json", boot::parse_boot_contract);
    can_start = can_start && contract;
    const std::string id = ask(start::safe_mode_alert(can_start), contract ? contract->data_dir : g_args.data_root);
    const bool start_last_good = can_start && id == start::kStartLastGood;
    std::fprintf(stderr, "[host] safe mode: %s\n", id.c_str());
    if (start_last_good) relaunch(own_bundle(), {"--safe-mode"});
    door::exit_process(start_last_good ? 0 : 1);
}

void hold_data_lock(const fs::path& data_dir) {
    for (int attempt = 0; attempt < 200; ++attempt) {
        if (auto lock = parties::update::DataLock::try_acquire(data_dir)) {
            // Never released: the kernel drops it once the process is gone,
            // after every destructor of the payload has run.
            [[maybe_unused]] auto* held = new parties::update::DataLock(std::move(*lock));
            return;
        }
        std::this_thread::sleep_for(50ms);
    }
    std::fprintf(stderr, "[host] %s/data.lock is held by another process\n", data_dir.c_str());
    door::exit_process(int(ExitCode::DataUnavailable));
}

// Runs a payload's entry point; its arguments are argv, without argv[0].
int run_entry(const parties::host::LoadedPayload& payload, const fs::path& build_dir, const fs::path& data_dir,
              const fs::path& backups_dir, int link_fd, std::vector<const char*> argv) {
    const std::string bundle = own_bundle();
    const PartiesHostApi api{
        .abi = PARTIES_HOST_ABI,
        .size = sizeof(PartiesHostApi),
        .host_bundle = bundle.c_str(),
        .build_dir = build_dir.c_str(),
        .data_dir = data_dir.c_str(),
        .backups_dir = backups_dir.c_str(),
        .link_fd = link_fd,
        .argc = int(argv.size()),
        .argv = argv.data(),
        .exit_process = &door::exit_process,
    };
    return payload.entry(&api);
}

[[noreturn]] void run_payload(const parties::host::LoadedPayload& payload, const fs::path& build_dir,
                              const fs::path& data_dir, const fs::path& backups_dir, int link_fd) {
    // terminate: ends the process with exit(0) right after this notification.
    [NSNotificationCenter.defaultCenter addObserverForName:NSApplicationWillTerminateNotification
                                                    object:nil
                                                     queue:nil
                                                usingBlock:^(NSNotification*) {
        std::fprintf(stderr, "[host] terminate: exiting 0\n");
        door::send(protocol::Exiting{0});
    }];
    std::vector<const char*> argv;
    for (const std::string& arg : g_args.forwarded) argv.push_back(arg.c_str());
    door::exit_process(run_entry(payload, build_dir, data_dir, backups_dir, link_fd, argv));
}

bool boot_ok_seen() {
    char byte = 0;
    return g_boot.ok = g_boot.ok || read(g_boot.boot_ok, &byte, 1) == 1;
}

// The launcher this front started ended or stalled before boot_ok: a failed boot.
void count_failed_boot() {
    if (g_boot.launcher) {
        kill(g_boot.launcher, SIGKILL);
        waitpid(g_boot.launcher, nullptr, 0);
    }
    g_boot.launcher = 0;
    g_boot.record = boot::record_boot_failure(g_boot.record);
    persist(state_dir() / "boot-record.json", boot::write_boot_record(g_boot.record));
    std::fprintf(stderr, "[host] failed boot recorded\n");
}

// Launcher loss (process-contract.md): when this front's launcher dies
// before boot_ok, a new front chooses again with the failure counted.
void on_launcher_lost() {
    if (!g_boot.launcher || boot_ok_seen()) return;
    count_failed_boot();
    close(g_boot.lock);
    g_boot.lock = -1;
    relaunch(own_bundle(), {"--boot-round=" + std::to_string(g_args.boot_round + 1)});
}

// socket: the launcher's, for an assigned build. nullptr: safe mode, which
// shows its alert again when the build cannot be loaded.
[[noreturn]] void run_build(const protocol::Assign& assign, const std::string* socket) {
    const fs::path build_dir = layout::build_path(g_args.install_root, assign.build);
    hold_data_lock(assign.data_dir);
    const auto payload = parties::host::load(build_dir / kMacClient.app_file, assign.app_sha256, kClientEntry);
    if (!payload && !socket) safe_mode(false);
    if (!payload) {
        using parties::host::LoadError;
        if (payload.error() == LoadError::Missing || payload.error() == LoadError::DigestMismatch) {
            door::send(protocol::Corrupt{assign.build});
            door::exit_process(0);
        }
        door::exit_process(int(ExitCode::InitFailed));
    }
    const int link_fd = socket ? door::attach_link(*socket, assign.ticket) : -1;
    if (socket) {
        door::send(protocol::Loaded{assign.build});
        door::watch_launcher(&on_launcher_lost);
    }
    run_payload(*payload, build_dir, assign.data_dir, assign.backups_dir, link_fd);
}

// --safe-mode: the contract's last good app, with no launcher and no link.
[[noreturn]] void run_unsupervised() {
    if (const auto contract = load(state_dir() / "boot-contract.json", boot::parse_boot_contract)) {
        std::fprintf(stderr, "[host] safe mode runs %s unsupervised\n", contract->last_good_app.dir.c_str());
        const fs::path data = contract->data_dir;
        run_build({contract->last_good_app.dir, contract->last_good_app.sha256, data.string(), (data / "backups").string(), 0},
                  nullptr);
    }
    safe_mode(false);
}

pid_t spawn_launcher(const std::vector<std::string>& role_args, int boot_ok_fd) {
    auto argv = start::launcher_arguments(g_args, NSBundle.mainBundle.executablePath.fileSystemRepresentation, role_args);
    std::vector<char*> raw;
    for (std::string& arg : argv) raw.push_back(arg.data());
    raw.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    if (boot_ok_fd >= 0) posix_spawn_file_actions_adddup2(&actions, boot_ok_fd, 3);
    pid_t pid = 0;
    if (posix_spawn(&pid, raw[0], &actions, nullptr, raw.data(), environ) != 0) pid = 0;
    posix_spawn_file_actions_destroy(&actions);
    return pid;
}

bool install_seed() {
    int status = 0;
    const pid_t pid = spawn_launcher({"--install-seed=" + own_bundle().string()}, -1);
    return pid && waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

// boot::choose until a launcher listens, as Parties.exe chooses: one that
// cannot be used (exit 70) is passed over, and any other end before it
// listens is a failed boot. False: no launcher can run.
bool boot_launcher(const std::string& socket) {
    start::LauncherChoice choice(load(state_dir() / "boot-contract.json", boot::parse_boot_contract),
                                 load(state_dir() / "boot-record.json", boot::parse_boot_record).value_or(boot::BootRecord{}),
                                 load(state_dir() / "launcher-session.json", boot::parse_session_report));
    for (;;) {
        const auto run = choice.next();
        persist(state_dir() / "boot-record.json", boot::write_boot_record(choice.record()));
        int pipe_fds[2];
        if (!run || pipe(pipe_fds) != 0) return false;
        for (int fd : pipe_fds) fcntl(fd, F_SETFD, FD_CLOEXEC);
        fcntl(pipe_fds[0], F_SETFL, O_NONBLOCK);
        std::vector<std::string> role{"--build=" + run->dir, "--boot-session=" + std::to_string(run->session), "--boot-ok-fd=3"};
        if (g_args.recover) role.push_back("--recover");
        const pid_t pid = spawn_launcher(role, pipe_fds[1]);
        close(pipe_fds[1]);
        close(g_boot.boot_ok);
        g_boot = {g_boot.lock, pid, pipe_fds[0], false, choice.record()};
        std::fprintf(stderr, "[host] launcher pid=%d from %s, session %llu\n", pid, run->dir.c_str(), run->session);
        int status = -1;
        for (const auto until = Clock::now() + std::chrono::milliseconds(g_args.boot_wait_ms);
             pid && Clock::now() < until; std::this_thread::sleep_for(50ms)) {
            if (door::connect(socket)) return true;
            // Exit 0 after boot_ok: another launcher holds launcher.lock, and that is the one to reach.
            if (status < 0 && waitpid(pid, &status, WNOHANG) == pid && !(status == 0 && boot_ok_seen())) break;
        }
        if (status >= 0) g_boot.launcher = 0;
        std::fprintf(stderr, "[host] launcher pid=%d never listened: wait status %d\n", pid, status);
        if (WIFEXITED(status) && WEXITSTATUS(status) == kLauncherUnusable) {
            choice.unusable();
            continue;
        }
        count_failed_boot();
        choice.failed();
    }
}

// Connects to the launcher, or takes boot.lock so this front may start one.
// A front that waited 60 s for another front's boot leaves it to that one.
bool reach_launcher(const std::string& socket) {
    if (door::connect(socket)) return true;
    if (g_args.ticketed) door::exit_process(0);  // its launcher died after asking for it
    if (g_boot.lock >= 0) return false;
    std::error_code ec;
    fs::create_directories(state_dir(), ec);
    const int fd = open((state_dir() / "boot.lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    for (const auto until = Clock::now() + 60s; Clock::now() < until; std::this_thread::sleep_for(100ms)) {
        const bool locked = fd < 0 || flock(fd, LOCK_EX | LOCK_NB) == 0;
        if (door::connect(socket)) {
            close(fd);
            return true;
        }
        if (locked) {
            g_boot.lock = fd;
            return false;
        }
    }
    door::exit_process(0);
}


// Front start (process-contract.md): reach the launcher, or install the seed
// and boot one, then do what it says.
[[noreturn]] void run_front() {
    const fs::path bundle = own_bundle();
    const std::string socket = protocol::socket_path(g_args.test_namespace);
    std::fprintf(stderr, "[host] front pid=%d bundle=%s boot round %d\n", getpid(), bundle.c_str(), g_args.boot_round);
    for (int i = 0; g_args.after && kill(g_args.after, 0) == 0 && i < 400; ++i) std::this_thread::sleep_for(50ms);
    if (g_args.safe_mode) run_unsupervised();
    auto record = load(state_dir() / "host.json", start::parse_host_record).value_or(start::HostRecord{});
    for (int round = 0; round < 6; ++round) {
        const bool reachable = reach_launcher(socket);
        const auto seed = parties::host::digest_file(bundle / "Contents" / "Resources" / "seed" / "build.json");
        const start::FrontFacts facts{reachable, load(state_dir() / "boot-contract.json", boot::parse_boot_contract).has_value(),
                                      seed ? std::optional(seed->to_hex()) : std::nullopt,
                                      load(state_dir() / "seed.json", start::parse_installed_seed), record,
                                      uint8_t(std::min(g_args.boot_round, 255))};
        const start::FrontPlan plan = start::plan_front(facts);
        std::fprintf(stderr, "[host] plan %d: reachable %d, seed %s\n", int(plan), reachable,
                     facts.bundle_seed.value_or("none").c_str());
        if (plan == start::FrontPlan::InstallSeed) {
            if (!install_seed())
                persist(state_dir() / "host.json", start::write_host_record(record = start::record_seed_failure(record, *facts.bundle_seed)));
            continue;
        }
        if (plan == start::FrontPlan::SafeMode || (plan == start::FrontPlan::ChooseLauncher && !boot_launcher(socket)))
            safe_mode(true);
        const auto reply = door::introduce({getpid(), PARTIES_HOST_ABI, bundle.string(), g_args.recover,
                                            plan == start::FrontPlan::ConnectSeedPending, g_args.forwarded},
                                           g_boot.launcher ? g_args.boot_wait_ms : 0);
        if (!reply) {
            if (g_boot.launcher && !boot_ok_seen()) count_failed_boot();
            continue;
        }
        if (const auto* assign = std::get_if<protocol::Assign>(&*reply)) run_build(*assign, &socket);
        if (const auto* recovery = std::get_if<protocol::Recovery>(&*reply)) {
            const std::string id = ask(start::recovery_alert(*recovery), recovery->data_dir);
            std::fprintf(stderr, "[host] recovery answered: %s\n", id.c_str());
            door::send(protocol::RecoveryChoice{id});
            door::exit_process(0);
        }
        if (std::holds_alternative<protocol::ActivateOther>(*reply)) {
            std::fprintf(stderr, "[host] activate_other: another front owns the session\n");
            door::exit_process(0);
        }
        door::wait_closed();  // quit_for_install: the launcher leaves for the seed install
    }
    door::exit_process(1);
}
// --role=launcher (process-contract.md, "Launcher argv"): verify the launcher
// dylib, load it and run it. Exit 70 when it cannot be used.
int run_launcher_role(int argc, const char* argv[]) {
    const fs::path bundle = own_bundle();
    fs::path dir;
    std::optional<parties::update::Sha256> expected;
    std::error_code ec;
    if (g_args.install_seed) {
        // Only this bundle's own seed, which the bundle's signature covers.
        if (!fs::equivalent(*g_args.install_seed, bundle, ec)) return kLauncherUnusable;
        dir = bundle / "Contents" / "Frameworks";
    } else {
        if (!boot::is_build_dir(g_args.build)) return kLauncherUnusable;
        dir = layout::build_path(g_args.install_root, g_args.build);
        if (auto contract = load(state_dir() / "boot-contract.json", boot::parse_boot_contract))
            expected = start::pinned_launcher(*contract, g_args.build);
        if (!expected) {
            std::fprintf(stderr, "[host] the committed state pins no launcher for %s\n", g_args.build.c_str());
            return kLauncherUnusable;
        }
    }
    const fs::path file = dir / kMacClient.launcher_file;
    if (!parties::host::meets_requirement(file)) return kLauncherUnusable;
    const auto payload = parties::host::load(file, expected, kLauncherEntry);
    if (!payload) return kLauncherUnusable;
    return run_entry(*payload, dir, "", "", -1, {argv + 1, argv + argc});
}

} // namespace

int main(int argc, const char* argv[]) {
    @autoreleasepool {
        g_args = start::parse_host_args({argv + 1, size_t(argc - 1)}, kTesting, app_support() / layout::kRuntimeRoot,
                                        app_support() / layout::kDataRoot);
        g_args.recover = g_args.recover || (NSEvent.modifierFlags & NSEventModifierFlagOption);
        if (const char* ms = kTesting ? std::getenv("PARTIES_TEST_BOOT_WAIT_MS") : nullptr) g_args.boot_wait_ms = std::atoi(ms);
#ifdef PARTIES_UPDATE_TESTING
        if (g_args.log_dir) {
            const std::string role = g_args.launcher ? "launcher-" : "front-";
            std::freopen((fs::path(*g_args.log_dir) / (role + std::to_string(getpid()) + ".log")).c_str(), "a", stderr);
            std::setvbuf(stderr, nullptr, _IONBF, 0);
        }
        if (g_args.self_test) return parties::host::load(*g_args.self_test, std::nullopt, kClientEntry) ? 0 : 1;
#endif
        if (g_args.launcher) return run_launcher_role(argc, argv);
        run_front();
    }
}
