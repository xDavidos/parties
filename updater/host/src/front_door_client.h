// The host's end of the front-door protocol (process-contract.md). A front
// keeps one front-door connection for its whole life: the kernel closing it
// at death is how the launcher learns that this front is gone.

#pragma once

#include <parties/update/front_door.h>

#include <cstdint>
#include <optional>
#include <string>

namespace parties::host::front_door {

namespace protocol = update::front_door;

bool connect(const std::string& socket_path);

// Sends front{...}, then reads until the launcher assigns a build, sends this
// front away, asks it to show recovery, or quits for a seed install.
// nullopt: the launcher closed first, or timeout_ms (0: none) passed.
std::optional<protocol::LauncherMessage> introduce(const protocol::Front& front, int timeout_ms);

// Returns once the launcher has closed the connection.
void wait_closed();

// A second connection whose first line is attach{ticket}: the payload's link.
// -1 when it cannot connect.
int attach_link(const std::string& socket_path, uint64_t ticket);

// Write failures are ignored: the launcher may already be gone.
void send(const protocol::HostMessage& message);

// PartiesHostApi.exit_process: exiting{code}, then _exit(code). The first
// caller's code wins; any other thread that calls it blocks until the exit.
[[noreturn]] void exit_process(int code);

// Reads the connection on a background thread for the rest of the process.
// activate brings the app forward. EOF means the launcher is gone: on_lost
// runs, the app is asked to quit, and the process exits 1 after 15 s at the
// latest.
void watch_launcher(void (*on_lost)());

} // namespace parties::host::front_door
