#include <client/data_location.h>
#include <client/sqlite_file.h>

#include <string_view>
#include <variant>
#include <system_error>
#include <vector>

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

namespace parties::client {

namespace fs = std::filesystem;

namespace {

std::optional<std::variant<fs::path, std::string>> path_flag(std::span<const std::wstring> args,
                                                             std::wstring_view flag) {
    for (const std::wstring& arg : args) {
        if (!arg.starts_with(flag)) continue;
        const fs::path dir(arg.substr(flag.size()));
        if (!dir.is_absolute())
            return std::string(flag.begin(), flag.end() - 1) + " must be an absolute path, got \"" +
                   utf8_path(dir) + "\"";
        return dir.lexically_normal();
    }
    return std::nullopt;
}

DataLocationResult default_location(const DataLocationInputs& inputs) {
    if (inputs.retail) {
        if (!inputs.local_app_data.is_absolute())
            return {std::nullopt, "the local application data folder is unknown"};
        const fs::path root = inputs.local_app_data / "Parties";
        return {DataLocation{root / "data", root / "backups"}, {}};
    }
    if (!inputs.exe_dir.is_absolute())
        return {std::nullopt, "the executable's folder is unknown"};
    return {DataLocation{inputs.exe_dir, inputs.exe_dir / "backups"}, {}};
}

} // namespace

// --backups-dir lets the launcher keep the app's pre-migration copies next to
// its own snapshots instead of inside the data directory.
DataLocationResult resolve_data_location(const DataLocationInputs& inputs) {
    DataLocationResult result;
    const auto data_dir = path_flag(inputs.args, L"--data-dir=");
    if (data_dir && std::holds_alternative<std::string>(*data_dir))
        return {std::nullopt, std::get<std::string>(*data_dir)};
    if (data_dir) {
        const fs::path& dir = std::get<fs::path>(*data_dir);
        result = {DataLocation{dir, dir / "backups"}, {}};
    } else {
        result = default_location(inputs);
        if (!result.location) return result;
    }

    const auto backups_dir = path_flag(inputs.args, L"--backups-dir=");
    if (backups_dir && std::holds_alternative<std::string>(*backups_dir))
        return {std::nullopt, std::get<std::string>(*backups_dir)};
    if (backups_dir) result.location->backups_dir = std::get<fs::path>(*backups_dir);
    return result;
}

namespace {

std::vector<std::wstring> command_line_args() {
    std::vector<std::wstring> args;
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return args;
    for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
    LocalFree(argv);
    return args;
}

fs::path local_app_data() {
    PWSTR path = nullptr;
    fs::path result;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &path)))
        result = path;
    CoTaskMemFree(path);
    return result;
}

fs::path exe_dir() {
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) return {};
    buffer.resize(length);
    return fs::path(buffer).parent_path();
}

} // namespace

DataLocationResult resolve_process_data_location() {
    const std::vector<std::wstring> args = command_line_args();
    DataLocationInputs inputs;
    inputs.args = args;
#ifdef PARTIES_RETAIL
    inputs.retail = true;
#endif
    inputs.local_app_data = local_app_data();
    inputs.exe_dir = exe_dir();

    DataLocationResult result = resolve_data_location(inputs);
    if (!result.location) return result;

    std::error_code ec;
    fs::create_directories(result.location->data_dir, ec);
    if (ec)
        return {std::nullopt, "cannot create the data folder " +
                              utf8_path(result.location->data_dir) + ": " + ec.message()};
    return result;
}

} // namespace parties::client
