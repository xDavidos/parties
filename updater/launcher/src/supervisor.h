#pragma once

#include "paths.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace parties::update::launcher {

// The parsed command line (contracts.md, "Stub to launcher").
struct LauncherOptions {
    InstallPaths install;
    DataPaths data;
    uint64_t boot_session = 0;
    void* boot_ok_event = nullptr;
    bool recover = false;
    std::vector<std::wstring> forwarded;   // everything after "--"
    std::optional<fs::path> feed_dir;      // testing builds only: a local update source
    std::optional<std::string> feed_base;  // testing builds only: http://127.0.0.1:<port>/
    std::wstring object_prefix = L"Parties";  // --test-namespace in testing builds
};

// nullopt with `error` set when the command line is unusable.
std::optional<LauncherOptions> parse_options(const std::vector<std::wstring>& args, std::string& error);

// Feeds Resumed, then StartRequested (or opens recovery for --recover), and
// supervises the app until nothing is left to run. Returns the exit code.
int run_launcher(const LauncherOptions& options);

} // namespace parties::update::launcher
