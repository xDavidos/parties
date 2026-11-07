#pragma once

#include "paths.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace parties::update::launcher {

struct LauncherOptions {
    InstallPaths install;
    DataPaths data;
    uint64_t boot_session = 0;
    void* boot_ok_event = nullptr;
    bool recover = false;
    std::vector<std::wstring> forwarded;
    std::optional<fs::path> feed_dir;
    std::optional<std::string> feed_base;
    std::wstring object_prefix = L"Parties";
};

std::optional<LauncherOptions> parse_options(const std::vector<std::wstring>& args, std::string& error);

int run_launcher(const LauncherOptions& options);

} // namespace parties::update::launcher
