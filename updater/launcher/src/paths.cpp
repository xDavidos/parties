#include "paths.h"

#include <windows.h>
#include <shlobj.h>

namespace parties::update::launcher {

namespace {

fs::path known_folder(REFKNOWNFOLDERID id) {
    PWSTR path = nullptr;
    fs::path out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &path))) out = path;
    CoTaskMemFree(path);
    return out;
}

} // namespace

fs::path default_install_root() {
    fs::path base = known_folder(FOLDERID_UserProgramFiles);
    return base.empty() ? base : base / "Parties";
}

fs::path default_data_root() {
    fs::path base = known_folder(FOLDERID_LocalAppData);
    return base.empty() ? base : base / "Parties";
}

} // namespace parties::update::launcher
