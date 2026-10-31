#include <client/data_location.h>

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

using parties::client::DataLocationInputs;
using parties::client::resolve_data_location;
namespace fs = std::filesystem;

namespace {

bool Check(bool condition, const char* message) {
    if (!condition) std::fprintf(stderr, "FAILED: %s\n", message);
    return condition;
}

DataLocationInputs inputs(const std::vector<std::wstring>& args, bool retail) {
    DataLocationInputs in;
    in.args = args;
    in.retail = retail;
    in.local_app_data = L"C:\\Users\\u\\AppData\\Local";
    in.exe_dir = L"D:\\Games\\Parties";
    return in;
}

} // namespace

int main() {
    bool ok = true;
    const std::vector<std::wstring> none;

    for (bool retail : {false, true}) {
        const std::vector<std::wstring> args{L"--minimized", L"--data-dir=E:\\Portable\\Data"};
        const auto r = resolve_data_location(inputs(args, retail));
        ok &= Check(r.location && r.location->data_dir == fs::path(L"E:\\Portable\\Data"),
                    "--data-dir did not win");
        ok &= Check(r.location && r.location->db_path() == fs::path(L"E:\\Portable\\Data\\parties_client.db"),
                    "--data-dir database path is wrong");
        ok &= Check(r.location && r.location->backups_dir == fs::path(L"E:\\Portable\\Data\\backups"),
                    "--data-dir backups path is wrong");
    }

    {
        const std::vector<std::wstring> args{L"--data-dir=relative\\dir"};
        const auto r = resolve_data_location(inputs(args, true));
        ok &= Check(!r.location && !r.error.empty(), "a relative --data-dir was accepted");
    }
    {
        const std::vector<std::wstring> args{L"--data-dir="};
        ok &= Check(!resolve_data_location(inputs(args, false)).location, "an empty --data-dir was accepted");
    }

    {
        const auto r = resolve_data_location(inputs(none, true));
        ok &= Check(r.location && r.location->data_dir == fs::path(L"C:\\Users\\u\\AppData\\Local\\Parties\\data"),
                    "retail data directory is not %LOCALAPPDATA%\\Parties\\data");
        ok &= Check(r.location && r.location->backups_dir == fs::path(L"C:\\Users\\u\\AppData\\Local\\Parties\\backups"),
                    "retail backups directory is not %LOCALAPPDATA%\\Parties\\backups");
        ok &= Check(r.location && r.location->sentry_dir() ==
                    fs::path(L"C:\\Users\\u\\AppData\\Local\\Parties\\data\\sentry"),
                    "retail sentry directory is not inside the data directory");
    }
    {
        auto in = inputs(none, true);
        in.local_app_data.clear();
        ok &= Check(!resolve_data_location(in).location, "retail resolved without a local app data folder");
    }

    {
        const auto r = resolve_data_location(inputs(none, false));
        ok &= Check(r.location && r.location->data_dir == fs::path(L"D:\\Games\\Parties"),
                    "non-retail data directory is not the exe directory");
    }

    // Every resolution is absolute, so the working directory can never be used.
    const std::vector<std::wstring> cases[] = {none, {L"--data-dir=C:\\x"}};
    for (const auto& args : cases) {
        for (bool retail : {false, true}) {
            const auto r = resolve_data_location(inputs(args, retail));
            ok &= Check(r.location && r.location->data_dir.is_absolute() &&
                        r.location->backups_dir.is_absolute(), "a resolution was not absolute");
        }
    }
    {
        auto in = inputs(none, false);
        in.exe_dir.clear();
        ok &= Check(!resolve_data_location(in).location, "an unknown exe directory fell back to the working directory");
    }

    std::printf(ok ? "data location tests passed\n" : "data location tests FAILED\n");
    return ok ? 0 : 1;
}
