// The macOS host: the bundle's main executable. A front asks the launcher
// which build to run and loads that payload into this process, so
// LaunchServices, the Dock and TCC only ever see this executable. With no
// launcher listening, it runs the seed payload from Contents/Frameworks.

#import <AppKit/AppKit.h>

#include "front_door_client.h"
#include "payload.h"

#include <client/exit_codes.h>
#include <parties/data_lock.h>
#include <parties/front_door_socket.h>
#include <parties/macos_layout.h>
#include <parties/update/boot.h>
#include <parties/update/product.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;
namespace door = parties::host::front_door;
namespace layout = parties::update::macos;
namespace protocol = parties::update::front_door;
using parties::client::ExitCode;
using parties::update::kMacClient;

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

struct Args {
    fs::path install_root;
    bool launcher = false;                  // --role=launcher
    std::string build;                      // --build=builds\<dir>
    std::optional<fs::path> install_seed;   // --install-seed=<this Parties.app>
    bool ticketed = false;                  // --ticket=<n>: a front a launcher started
    std::string test_namespace;
    std::vector<std::string> forwarded;  // after "--"; every other unknown flag is dropped
#ifdef PARTIES_UPDATE_TESTING
    std::optional<fs::path> self_test, fake_launcher, log_dir;
#endif
};

fs::path app_support() {
    return NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory, NSUserDomainMask, YES)
        .firstObject.fileSystemRepresentation;
}

// Where parties.db lives when no launcher assigns a place.
fs::path unmanaged_data_dir() { return app_support() / layout::kDataRoot; }

std::optional<std::string_view> flag(std::string_view arg, std::string_view name) {
    if (!arg.starts_with(name)) return std::nullopt;
    return arg.substr(name.size());
}

Args parse_args(int argc, const char* argv[]) {
    Args args{.install_root = app_support() / layout::kRuntimeRoot};
    std::optional<fs::path> install_root;
    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "--") {
            args.forwarded.assign(argv + i + 1, argv + argc);
            break;
        }
        if (arg == "--role=launcher") args.launcher = true;
        if (arg.starts_with("--ticket=")) args.ticketed = true;
        if (auto v = flag(arg, "--build=")) args.build = *v;
        if (auto v = flag(arg, "--install-seed=")) args.install_seed = *v;
        if (auto v = flag(arg, "--install-root=")) install_root = *v;
#ifdef PARTIES_UPDATE_TESTING
        if (auto v = flag(arg, "--test-namespace=")) args.test_namespace = *v;
        if (auto v = flag(arg, "--self-test=")) args.self_test = *v;
        if (auto v = flag(arg, "--role=fake-launcher=")) args.fake_launcher = *v;
        if (auto v = flag(arg, "--log-dir=")) args.log_dir = *v;
#endif
    }
    // The launcher role is passed its roots by the booting front. A front
    // takes one only in testing builds.
    if (install_root && (args.launcher || kTesting)) args.install_root = *install_root;
    return args;
}

void hold_data_lock(const fs::path& data_dir) {
    for (int attempt = 0; attempt < 200; ++attempt) {
        if (auto lock = parties::update::DataLock::try_acquire(data_dir)) {
            // Never released: the kernel drops it once the process is gone,
            // after every destructor of the payload has run.
            [[maybe_unused]] auto* held = new parties::update::DataLock(std::move(*lock));
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    std::fprintf(stderr, "[host] %s/data.lock is held by another process\n", data_dir.c_str());
    door::exit_process(int(ExitCode::DataUnavailable));
}

[[noreturn]] void run_payload(const parties::host::LoadedPayload& payload, const fs::path& bundle,
                              const fs::path& build_dir, const fs::path& data_dir, const fs::path& backups_dir,
                              int link_fd, const Args& args) {
    // terminate: ends the process with exit(0) right after this notification.
    [NSNotificationCenter.defaultCenter addObserverForName:NSApplicationWillTerminateNotification
                                                    object:nil
                                                     queue:nil
                                                usingBlock:^(NSNotification*) {
        std::fprintf(stderr, "[host] terminate: exiting 0\n");
        door::send(protocol::Exiting{0});
    }];

    std::vector<const char*> argv;
    for (const std::string& arg : args.forwarded) argv.push_back(arg.c_str());
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
    door::exit_process(payload.entry(&api));
}

[[noreturn]] void run_unmanaged(const fs::path& bundle, const Args& args) {
    const fs::path build_dir = bundle / "Contents" / "Frameworks";
    const fs::path data_dir = unmanaged_data_dir();
    hold_data_lock(data_dir);
    const auto payload = parties::host::load(build_dir / kMacClient.app_file, std::nullopt, kClientEntry);
    if (!payload) door::exit_process(int(ExitCode::InitFailed));
    run_payload(*payload, bundle, build_dir, data_dir, data_dir / "backups", -1, args);
}

[[noreturn]] void run_assigned(const fs::path& bundle, const Args& args, const std::string& socket,
                               const protocol::Assign& assign) {
    const fs::path build_dir = layout::build_path(args.install_root, assign.build);
    hold_data_lock(assign.data_dir);
    const auto payload = parties::host::load(build_dir / kMacClient.app_file, assign.app_sha256, kClientEntry);
    if (!payload) {
        using parties::host::LoadError;
        if (payload.error() == LoadError::Missing || payload.error() == LoadError::DigestMismatch) {
            door::send(protocol::Corrupt{assign.build});
            door::exit_process(0);
        }
        door::exit_process(int(ExitCode::InitFailed));
    }
    const int link_fd = door::attach_link(socket, assign.ticket);
    door::send(protocol::Loaded{assign.build});
    door::watch_launcher();
    run_payload(*payload, bundle, build_dir, assign.data_dir, assign.backups_dir, link_fd, args);
}

[[noreturn]] void run_front(const Args& args) {
    const fs::path bundle = NSBundle.mainBundle.bundlePath.fileSystemRepresentation;
    const std::string socket = protocol::socket_path(args.test_namespace);
    std::fprintf(stderr, "[host] front pid=%d bundle=%s\n", getpid(), bundle.c_str());
    for (int round = 0; round < 3; ++round) {
        if (!door::connect(socket)) {
            // Its launcher died after asking for it: nothing may run unsupervised in its name.
            if (args.ticketed) door::exit_process(0);
            run_unmanaged(bundle, args);
        }
        const auto reply = door::introduce({getpid(), PARTIES_HOST_ABI, bundle.string(), false, false, args.forwarded});
        if (!reply) continue;
        if (const auto* assign = std::get_if<protocol::Assign>(&*reply)) run_assigned(bundle, args, socket, *assign);
        if (std::holds_alternative<protocol::ActivateOther>(*reply)) {
            std::fprintf(stderr, "[host] activate_other: another front owns the session\n");
            door::exit_process(0);
        }
    }
    door::exit_process(1);
}

// A launcher dylib gets this process's arguments after argv[0].
int run_launcher_payload(const parties::host::LoadedPayload& payload, const fs::path& dir, int argc,
                         const char* argv[]) {
    const std::string bundle = NSBundle.mainBundle.bundlePath.fileSystemRepresentation;
    const PartiesHostApi api{
        .abi = PARTIES_HOST_ABI,
        .size = sizeof(PartiesHostApi),
        .host_bundle = bundle.c_str(),
        .build_dir = dir.c_str(),
        .data_dir = "",
        .backups_dir = "",
        .link_fd = -1,
        .argc = argc - 1,
        .argv = argv + 1,
        .exit_process = &door::exit_process,
    };
    return payload.entry(&api);
}

// The digest the committed state pins for a build's launcher. A launcher runs
// only from the good or the previous good build.
std::optional<parties::update::Sha256> pinned_launcher(const fs::path& install_root, const std::string& build) {
    std::ifstream in(install_root / "state" / "boot-contract.json");
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const auto contract = parties::update::boot::parse_boot_contract(text);
    if (!contract) return std::nullopt;
    if (contract->good.dir == build) return contract->good.sha256;
    if (contract->previous_good && contract->previous_good->dir == build) return contract->previous_good->sha256;
    return std::nullopt;
}

// --role=launcher (process-contract.md, "Launcher argv"): verify the launcher
// dylib, load it and run it. Exit 70 when it cannot be used.
int run_launcher_role(const Args& args, int argc, const char* argv[]) {
    const fs::path bundle = NSBundle.mainBundle.bundlePath.fileSystemRepresentation;
    fs::path dir;
    std::optional<parties::update::Sha256> expected;
    std::error_code ec;
    if (args.install_seed) {
        // Only this bundle's own seed, which the bundle's signature covers.
        if (!fs::equivalent(*args.install_seed, bundle, ec)) return kLauncherUnusable;
        dir = bundle / "Contents" / "Frameworks";
    } else {
        if (!parties::update::boot::is_build_dir(args.build)) return kLauncherUnusable;
        dir = layout::build_path(args.install_root, args.build);
        expected = pinned_launcher(args.install_root, args.build);
        if (!expected) {
            std::fprintf(stderr, "[host] the committed state pins no launcher for %s\n", args.build.c_str());
            return kLauncherUnusable;
        }
    }
    const fs::path file = dir / kMacClient.launcher_file;
    if (!parties::host::meets_requirement(file)) return kLauncherUnusable;
    const auto payload = parties::host::load(file, expected, kLauncherEntry);
    if (!payload) return kLauncherUnusable;
    return run_launcher_payload(*payload, dir, argc, argv);
}

#ifdef PARTIES_UPDATE_TESTING
// The front-door e2e test's launcher: a dylib that a host process runs the way
// the real launcher will run, without LaunchServices ever starting it.
int run_fake_launcher(const fs::path& dylib, int argc, const char* argv[]) {
    constexpr parties::host::Entry entry{"parties_fake_launcher_min_host_abi", "parties_fake_launcher_main"};
    const auto payload = parties::host::load(dylib, std::nullopt, entry);
    if (!payload) return 1;
    return run_launcher_payload(*payload, dylib.parent_path(), argc, argv);
}
#endif

} // namespace

int main(int argc, const char* argv[]) {
    @autoreleasepool {
        const Args args = parse_args(argc, argv);
#ifdef PARTIES_UPDATE_TESTING
        if (args.log_dir) {
            const std::string role = args.launcher ? "launcher-" : "front-";
            std::freopen((*args.log_dir / (role + std::to_string(getpid()) + ".log")).c_str(), "a", stderr);
            std::setvbuf(stderr, nullptr, _IONBF, 0);
        }
        if (args.self_test) return parties::host::load(*args.self_test, std::nullopt, kClientEntry) ? 0 : 1;
        if (args.fake_launcher) return run_fake_launcher(*args.fake_launcher, argc, argv);
#endif
        if (args.launcher) return run_launcher_role(args, argc, argv);
        run_front(args);
    }
}
