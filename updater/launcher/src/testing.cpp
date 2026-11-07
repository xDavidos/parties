#include "testing.h"

#include <windows.h>

namespace parties::update::launcher::testing {

void end_process(unsigned code) {
    TerminateProcess(GetCurrentProcess(), code);
    ExitProcess(code);
}

#ifdef PARTIES_UPDATE_TESTING

namespace {

std::optional<std::wstring> wide_env(const char* name) {
    auto value = env(name);
    if (!value) return std::nullopt;
    return std::filesystem::path(std::u8string(value->begin(), value->end())).wstring();
}

} // namespace

std::optional<std::string> identity_choice() { return env("PARTIES_TEST_IDENTITY_CHOICE"); }
std::optional<std::wstring> uninstall_key() { return wide_env("PARTIES_TEST_UNINSTALL_KEY"); }
std::optional<std::filesystem::path> start_menu() { return wide_env("PARTIES_TEST_START_MENU"); }
std::optional<std::filesystem::path> recycle_log() { return wide_env("PARTIES_TEST_RECYCLE_LOG"); }
std::optional<std::filesystem::path> desktop() { return wide_env("PARTIES_TEST_DESKTOP"); }
std::optional<std::filesystem::path> taskbar() { return wide_env("PARTIES_TEST_TASKBAR"); }
std::optional<std::wstring> legacy_image() { return wide_env("PARTIES_TEST_LEGACY_IMAGE"); }

#else

std::optional<std::string> identity_choice() { return std::nullopt; }
std::optional<std::wstring> uninstall_key() { return std::nullopt; }
std::optional<std::filesystem::path> start_menu() { return std::nullopt; }
std::optional<std::filesystem::path> recycle_log() { return std::nullopt; }
std::optional<std::filesystem::path> desktop() { return std::nullopt; }
std::optional<std::filesystem::path> taskbar() { return std::nullopt; }
std::optional<std::wstring> legacy_image() { return std::nullopt; }

#endif

} // namespace parties::update::launcher::testing
