#pragma once

// Names the installed layout shares between setup, the launcher and the app.

#include <filesystem>
#include <optional>

namespace parties::update {

// The boot stub at the install root. Shortcuts, pins and the uninstall
// entry's icon point here because updates never move or delete it.
inline constexpr wchar_t kStubFile[] = L"Parties.exe";

// Carried by the Start menu shortcuts and the app window alike, so the
// taskbar groups them and a pin made from the window starts the stub.
inline constexpr wchar_t kAppUserModelId[] = L"Parties.Client";

// <root> when `exe` runs from <root>\builds\<dir>\ and the stub sits at
// <root>; nullopt for a portable or development copy.
inline std::optional<std::filesystem::path> install_root_of(const std::filesystem::path& exe) {
    const std::filesystem::path builds = exe.parent_path().parent_path();
    if (builds.filename() != L"builds") return std::nullopt;
    std::filesystem::path root = builds.parent_path();
    std::error_code ec;
    if (!std::filesystem::is_regular_file(root / kStubFile, ec)) return std::nullopt;
    return root;
}

} // namespace parties::update
