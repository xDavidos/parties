// PartiesLauncher.dylib's entry point. A host process started with
// --role=launcher verified this file's digest, loaded it and calls
// parties_launcher_main. It never calls NSApplication, so it never registers
// with LaunchServices; it uses NSWorkspace only to launch fronts.

#import <Foundation/Foundation.h>

#include "log.h"
#include "supervisor.h"
#include "test_hooks.h"

#include <parties/host_api.h>
#include <parties/macos_layout.h>

#include <signal.h>

#include <charconv>
#include <string_view>

namespace macos = parties::update::launcher::macos;
namespace launcher = parties::update::launcher;
namespace fs = std::filesystem;

namespace parties::update::launcher::macos {

namespace {

fs::path user_directory(NSSearchPathDirectory directory) {
    return NSSearchPathForDirectoriesInDomains(directory, NSUserDomainMask, YES).firstObject.fileSystemRepresentation;
}

std::optional<uint64_t> number(std::string_view text) {
    uint64_t value = 0;
    auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || end != text.data() + text.size()) return std::nullopt;
    return value;
}

} // namespace

std::optional<LauncherOptions> parse_options(const std::vector<std::string>& args, std::string& error) {
    LauncherOptions o;
    std::optional<fs::path> install_root, data_root;
    for (const std::string& a : args) {
        auto value_of = [&](std::string_view flag) -> std::optional<std::string> {
            if (!a.starts_with(flag)) return std::nullopt;
            return a.substr(flag.size());
        };
        auto absolute = [&](const std::string& v, const char* what) -> std::optional<fs::path> {
            fs::path p(v);
            if (!p.is_absolute()) {
                error = std::string(what) + " must be an absolute path";
                return std::nullopt;
            }
            return p.lexically_normal();
        };
        if (a == "--role=launcher") {
        } else if (a == "--recover") {
            o.recover = true;
        } else if (auto v = value_of("--install-root=")) {
            if (!(install_root = absolute(*v, "--install-root"))) return std::nullopt;
        } else if (auto v = value_of("--data-root=")) {
            if (!(data_root = absolute(*v, "--data-root"))) return std::nullopt;
        } else if (auto v = value_of("--install-seed=")) {
            if (!(o.install_seed = absolute(*v, "--install-seed"))) return std::nullopt;
        } else if (auto v = value_of("--boot-session=")) {
            auto n = number(*v);
            if (!n) return (error = "--boot-session must be a number", std::nullopt);
            o.boot_session = *n;
        } else if (auto v = value_of("--boot-ok-fd=")) {
            auto n = number(*v);
            if (!n || *n > 1024) return (error = "--boot-ok-fd must be a descriptor", std::nullopt);
            o.boot_ok_fd = int(*n);
        } else if (auto v = value_of("--build=")) {
            // The host already resolved it: build_dir is where this file was loaded from.
        } else if (auto v = value_of("--test-namespace=")) {
            if (testing::enabled() && !v->empty() && v->find('/') == std::string::npos) o.test_namespace = *v;
        } else if (auto v = value_of("--feed-dir=")) {
            if (!testing::enabled()) continue;
            if (!(o.feed_dir = absolute(*v, "--feed-dir"))) return std::nullopt;
        } else if (auto v = value_of("--log-dir=")) {
            if (!testing::enabled()) continue;
            if (!(o.log_dir = absolute(*v, "--log-dir"))) return std::nullopt;
        } else {
            log("ignoring unknown argument {}", a);
        }
    }
    const fs::path support = user_directory(NSApplicationSupportDirectory);
    o.install.root = install_root ? *install_root : support / parties::update::macos::kRuntimeRoot;
    o.data_root = data_root ? *data_root : support / parties::update::macos::kDataRoot;
    if (support.empty() && (!install_root || !data_root)) {
        error = "the per-user Application Support folder is unknown";
        return std::nullopt;
    }
    return o;
}

} // namespace parties::update::launcher::macos

extern "C" const uint32_t parties_launcher_min_host_abi = 1;

extern "C" int parties_launcher_main(const PartiesHostApi* host) {
    @autoreleasepool {
        // The booting front may have closed its end of the boot_ok pipe.
        signal(SIGPIPE, SIG_IGN);
        std::vector<std::string> args(host->argv, host->argv + host->argc);
        std::string error;
        auto options = macos::parse_options(args, error);
        if (!options) {
            launcher::log("bad arguments: {}", error);
            return 2;
        }
        options->host_bundle = host->host_bundle;
        options->build_dir = host->build_dir;
        const fs::path logs = options->log_dir ? *options->log_dir : macos::user_directory(NSLibraryDirectory) / "Logs" / "Parties";
        launcher::open_log(logs / (options->install_seed ? "install-seed.log" : "launcher.log"));
        if (options->install_seed) return macos::run_install_seed(*options);
        return macos::run_launcher(*options);
    }
}
