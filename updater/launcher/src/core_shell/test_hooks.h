#pragma once

// Hooks that exist only when PARTIES_UPDATE_TESTING is defined, the same on
// every platform. In every other build each function returns the
// production value.

#include <parties/update/manifest.h>
#include <parties/update/probation.h>

#include <chrono>
#include <optional>
#include <string>

namespace parties::update::launcher::testing {

// The exit code of a launcher a kill point ended. POSIX exit codes are 8 bits.
#ifdef _WIN32
inline constexpr unsigned kKilledAtEffect = 0xDEAD;
#else
inline constexpr unsigned kKilledAtEffect = 0xDE;
#endif

struct Timing {
    std::chrono::milliseconds unit{1000};  // how long one Policy second lasts
    Policy policy{};
};

// PARTIES_TEST_TIMING=<unit ms>,<health deadline>,<confirm uptime>,<confirm on clean exit after>
Timing timing();

// PARTIES_KILL_AT=<n> ends the process with kKilledAtEffect right before the
// n-th effect, counted across runs in PARTIES_KILL_COUNTER=<file> when set.
void before_effect();

std::optional<std::string> scripted_recovery();

// PARTIES_TEST_EXIT_AFTER_HEALTHY_MS=<n>: ask the app to exit n ms after it
// reports healthy, so a harness can end a real client through its normal path.
std::optional<std::chrono::milliseconds> exit_after_healthy();

std::optional<std::chrono::milliseconds> job_cap();

Keyring pinned_keys();

bool enabled();

// A non-empty environment variable.
std::optional<std::string> env(const char* name);

// Each platform defines it: ends the process at once, running nothing else.
[[noreturn]] void end_process(unsigned code);

} // namespace parties::update::launcher::testing
