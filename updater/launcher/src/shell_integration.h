#pragma once

// What an install leaves outside its install root: the "Apps > Installed
// apps" entry under HKCU and the Start menu shortcuts. Setup creates both and
// the launcher keeps the entry pointing at the current good build. Testing
// builds redirect every write (testing.h).

#include "paths.h"

#include <parties/update/version.h>

#include <string>
#include <vector>

namespace parties::update::launcher {

struct Shortcut {
    fs::path file;
    fs::path target;
    std::wstring arguments;
    std::wstring description;
};

// "Parties" and "Parties Recovery", both starting the stub.
std::vector<Shortcut> shortcuts(const InstallPaths& install);

// The folders a portable copy may run from or keep its database in: the
// target folder and the "Start in" folder of every link to parties_client.exe
// (or named Parties.lnk) in the Start menu, its subfolders, on the Desktop and
// among the taskbar pins. Read before setup rewrites the Start menu. Needs COM
// on the calling thread.
std::vector<fs::path> portable_shortcut_dirs();

// Creates each shortcut or rewrites one that differs; one already right is
// left byte for byte. Needs COM initialized on the calling thread.
bool write_shortcuts(const InstallPaths& install, std::string& error);
void remove_shortcuts(const InstallPaths& install);

// "<root>\builds\<good>\PartiesLauncher.exe" --uninstall "--install-root=<root>".
// Good's directory exists for as long as it is good, and the launcher
// rewrites the string whenever good changes, so it survives updates and GC.
std::wstring uninstall_command(const InstallPaths& install, const BuildId& good);

// Writes the whole entry for `good`.
bool write_uninstall_entry(const InstallPaths& install, const BuildId& good, std::string& error);

// Rewrites the entry for `good`, but only one that setup created for this
// install root, so a development launcher never registers itself.
void refresh_uninstall_entry(const InstallPaths& install, const BuildId& good);

// Deletes the entry if it belongs to this install root.
void remove_uninstall_entry(const InstallPaths& install);

// Moves `path` to the Recycle Bin. Fails, deleting nothing, wherever the
// shell would delete permanently instead (no Recycle Bin on the volume, or
// the folder is too large for it).
bool recycle(const fs::path& path, std::string& error);

} // namespace parties::update::launcher
