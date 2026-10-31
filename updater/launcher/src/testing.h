#pragma once

// Hooks that exist only when PARTIES_UPDATE_TESTING is defined. In every
// other build each function returns the production value.

#include <parties/update/manifest.h>
#include <parties/update/probation.h>

#include <chrono>
#include <filesystem>
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

// PARTIES_TEST_IDENTITY_CHOICE=live|legacy: answer setup's identity question
// without showing it, even with --silent.
std::optional<std::string> identity_choice();

// PARTIES_TEST_EXIT_AFTER_HEALTHY_MS=<n>: ask the app to exit n ms after it
// reports healthy, so a harness can end a real client through its normal path.
std::optional<std::chrono::milliseconds> exit_after_healthy();

// PARTIES_TEST_JOB_CAP_MS=<n>: how long a feed job may keep the launcher
// running after the app exits.
std::optional<std::chrono::milliseconds> job_cap();

// The update keyring: production keys, or the harness key in testing builds.
Keyring pinned_keys();

// Setup's shell writes, redirected so a harness never touches the real ones:
// PARTIES_TEST_UNINSTALL_KEY=<HKCU subkey> replaces the Apps entry's key,
// PARTIES_TEST_START_MENU=<dir> replaces the Start menu Programs folder, and
// PARTIES_TEST_RECYCLE_LOG=<file> records each path instead of recycling it.
std::optional<std::wstring> uninstall_key();
std::optional<std::filesystem::path> start_menu();
std::optional<std::filesystem::path> recycle_log();

// Where setup looks for a portable copy beyond the folders it was given:
// PARTIES_TEST_DESKTOP=<dir> replaces the Desktop, PARTIES_TEST_TASKBAR=<dir>
// replaces the taskbar's pinned links, and
// PARTIES_TEST_LEGACY_IMAGE=<file name> replaces parties_client.exe as the
// name of running portable copies. A testing build without them looks at
// neither, so a harness never reads the real ones.
std::optional<std::filesystem::path> desktop();
std::optional<std::filesystem::path> taskbar();
std::optional<std::wstring> legacy_image();

bool enabled();

} // namespace parties::update::launcher::testing
