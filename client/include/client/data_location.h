#pragma once

#include <client/settings.h>

#include <filesystem>
#include <optional>
#include <span>
#include <string>

namespace parties::client {

// Where this Windows client keeps its user data. Never the working directory:
// a shortcut with another "Start in" folder must not look like a fresh install.
struct DataLocation {
    std::filesystem::path data_dir;
    std::filesystem::path backups_dir;

    std::filesystem::path db_path() const { return data_dir / "parties_client.db"; }
    std::filesystem::path sentry_dir() const { return data_dir / "sentry"; }
    SettingsPaths settings_paths() const { return {db_path(), backups_dir}; }
};

struct DataLocationInputs {
    std::span<const std::wstring> args;
    bool retail = false;
    std::filesystem::path local_app_data;
    std::filesystem::path exe_dir;
};

struct DataLocationResult {
    std::optional<DataLocation> location;
    std::string error;
};

// --data-dir=<absolute path> wins; retail builds use %LOCALAPPDATA%\Parties;
// other builds use the exe's directory so dev copies (build/client2..4) stay separate.
DataLocationResult resolve_data_location(const DataLocationInputs& inputs);

DataLocationResult resolve_process_data_location();

} // namespace parties::client
