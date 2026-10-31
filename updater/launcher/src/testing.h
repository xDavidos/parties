#pragma once

// Hooks that exist only when PARTIES_UPDATE_TESTING is defined. In every
// other build each function returns the production value.

#include <parties/update/manifest.h>
#include <parties/update/probation.h>

#include <chrono>
#include <optional>
#include <string>

namespace parties::update::launcher::testing {

// The exit code of a launcher stopped by PARTIES_KILL_AT.
inline constexpr unsigned kKilledAtEffect = 0xDEAD;

struct Timing {
    std::chrono::milliseconds unit{1000};  // how long one Policy second lasts
    Policy policy{};
};

// PARTIES_TEST_TIMING=<unit ms>,<health s>,<confirm s>,<clean exit s>
Timing timing();

// PARTIES_KILL_AT=<n>: terminate right before the n-th effect, counted from 1
// across this process, or across processes through PARTIES_KILL_COUNTER=<file>.
void before_effect();

// PARTIES_TEST_RECOVERY=<choice>: answer the recovery dialog without showing it.
std::optional<std::string> scripted_recovery();

// PARTIES_TEST_EXIT_AFTER_HEALTHY_MS=<n>: ask the app to exit n ms after it
// reports healthy, so a harness can end a real client through its normal path.
std::optional<std::chrono::milliseconds> exit_after_healthy();

// The update keyring: production keys, or the harness key in testing builds.
Keyring pinned_keys();

bool enabled();

} // namespace parties::update::launcher::testing
