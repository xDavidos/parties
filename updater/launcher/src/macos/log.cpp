#include "log.h"

#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>

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
    g_file = std::fopen(file.c_str(), "a");
}

// Until open_log runs, lines go to stderr, which the host's own log holds.
void log_line(std::string_view text) {
    auto now = std::chrono::floor<std::chrono::milliseconds>(std::chrono::system_clock::now());
    std::string line = std::format("{:%Y-%m-%d %H:%M:%S} [{}] {}\n", now, getpid(), text);
    std::lock_guard lock(g_mutex);
    FILE* out = g_file ? g_file : stderr;
    std::fwrite(line.data(), 1, line.size(), out);
    std::fflush(out);
}

} // namespace parties::update::launcher
