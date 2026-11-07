#pragma once

// The Windows launcher's testing hooks beyond the shared ones in
// test_hooks.h. Each exists only when PARTIES_UPDATE_TESTING is defined; in
// every other build each function returns the production value.

#include "test_hooks.h"

#include <filesystem>
#include <optional>
#include <string>

namespace parties::update::launcher::testing {

// PARTIES_TEST_IDENTITY_CHOICE=live|legacy: answer setup's identity question
// without showing it, even with --silent.
std::optional<std::string> identity_choice();

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

} // namespace parties::update::launcher::testing
