#include "log.h"

#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>

#include <windows.h>

namespace parties::update::launcher {

namespace {

std::mutex g_mutex;
FILE* g_file = nullptr;

} // namespace

void open_log(const std::filesystem::path& file) {
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    std::lock_guard lock(g_mutex);
    if (g_file) std::fclose(g_file);
    g_file = _wfsopen(file.c_str(), L"ab", _SH_DENYNO);
}

void log_line(std::string_view text) {
    auto now = std::chrono::floor<std::chrono::milliseconds>(std::chrono::system_clock::now());
    std::string line = std::format("{:%Y-%m-%d %H:%M:%S} [{}] {}\n", now, GetCurrentProcessId(), text);
    std::lock_guard lock(g_mutex);
    OutputDebugStringA(line.c_str());
    if (!g_file) return;
    std::fwrite(line.data(), 1, line.size(), g_file);
    std::fflush(g_file);
}

} // namespace parties::update::launcher
