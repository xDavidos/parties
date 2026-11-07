#pragma once

// Sole writer of state\supervisor.json (+ .prev), state\boot-contract.json
// and state\launcher-session.json.

#include "install_paths.h"

#include <parties/update/boot.h>
#include <parties/update/supervisor_state.h>

#include <string>
#include <variant>

namespace parties::update::launcher {

struct NoState {};
struct NewerLauncherState {}; // written by a newer launcher: leave it alone
struct UnreadableState {};
using LoadResult = std::variant<SupervisorState, NoState, NewerLauncherState, UnreadableState>;

SupervisorState adopted_state(const BuildRecord& installed);

class StateStore {
public:
    StateStore(InstallPaths install, std::string data_dir);

    LoadResult load();

    // Keeps the current file as .prev, durably replaces supervisor.json, then
    // derives boot-contract.json from the same state.
    bool commit(const SupervisorState& state, std::string& error);

    bool write_session(const boot::SessionReport& report, std::string& error);

private:
    InstallPaths install_;
    std::string data_dir_;
    bool main_is_good_ = true;  // false after loading from .prev, so .prev is not overwritten with garbage
};

} // namespace parties::update::launcher
