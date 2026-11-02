#pragma once

#include <filesystem>
#include <format>
#include <string_view>

namespace parties::update::launcher {

// Appends to <data root>\logs\launcher.log. Until open_log runs, lines go to
// the debugger output only.
void open_log(const std::filesystem::path& file);
void log_line(std::string_view text);

template <class... Args>
void log(std::format_string<Args...> fmt, Args&&... args) {
    log_line(std::format(fmt, std::forward<Args>(args)...));
}

} // namespace parties::update::launcher
