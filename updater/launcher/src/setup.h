#pragma once

// PartiesLauncher.exe's other modes. A copy that carries a payload
// (setup_payload.h) is PartiesSetup.exe and installs or repairs:
//   PartiesSetup.exe [--silent] [--no-launch] [--install-root=<abs>] [--data-root=<abs>]
// Every copy can uninstall and pack:
//   PartiesLauncher.exe --uninstall [--silent] [--remove-data] [--install-root=<abs>] [--data-root=<abs>]
//   PartiesLauncher.exe --pack=<payload dir> --out=<setup exe>
// Exit codes: 0 done, 1 failed, 2 cancelled by the user.

#include <optional>
#include <string>
#include <vector>

namespace parties::update::launcher {

// nullopt when none of these modes applies, so the caller runs the launcher.
std::optional<int> run_setup_modes(const std::vector<std::wstring>& args);

} // namespace parties::update::launcher
