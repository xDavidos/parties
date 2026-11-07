#pragma once

// PartiesLauncher.dylib's two modes, run inside a host process started with
// --role=launcher (process-contract.md, "Launcher argv").

#include "install_paths.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace parties::update::launcher::macos {

namespace fs = std::filesystem;

struct LauncherOptions {
    InstallPaths install;
    fs::path data_root;
    fs::path host_bundle;  // the Parties.app this process runs from
    fs::path build_dir;    // where this launcher was loaded from
    uint64_t boot_session = 0;
    int boot_ok_fd = -1;
    bool recover = false;
    std::optional<fs::path> install_seed;  // the Parties.app whose seed build to install
    std::optional<fs::path> feed_dir;
    std::optional<fs::path> log_dir;
    std::string test_namespace;
};

// state/launcher.lock, taken with flock(LOCK_EX | LOCK_NB) and retried for up
// to `wait`. True once this process holds it, which it then does until it
// exits; the kernel drops it at the process's death.
bool take_launcher_lock(const InstallPaths& install, std::chrono::milliseconds wait);

std::optional<LauncherOptions> parse_options(const std::vector<std::string>& args, std::string& error);

// The supervisor: one per user (state/launcher.lock), serving every front.
int run_launcher(const LauncherOptions& options);

// --install-seed: puts the bundle's seed build into the runtime root with
// plan_install (Adopt, Stage or Keep), records it in state/seed.json, and
// exits. Converges when killed at any step and run again.
int run_install_seed(const LauncherOptions& options);

} // namespace parties::update::launcher::macos
