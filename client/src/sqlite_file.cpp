#include <client/sqlite_file.h>

#include <sqlite3.h>

#include <ctime>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace parties::client {

namespace fs = std::filesystem;

std::string utf8_path(const fs::path& path) {
    const auto u8 = path.u8string();
    return {reinterpret_cast<const char*>(u8.data()), u8.size()};
}

fs::path partial_path(const fs::path& final_path) {
    fs::path partial = final_path;
    partial += ".partial";
    return partial;
}

std::string utc_file_stamp() {
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif
    char buf[32] = {};
    std::strftime(buf, sizeof(buf), "%Y%m%dT%H%M%SZ", &utc);
    return buf;
}

fs::path unique_backup_path(const fs::path& dir, const std::string& stem) {
    fs::path candidate = dir / (stem + ".db");
    std::error_code ec;
    for (int n = 2; fs::exists(candidate, ec) || fs::exists(partial_path(candidate), ec); ++n)
        candidate = dir / (stem + "-" + std::to_string(n) + ".db");
    return candidate;
}

bool vacuum_into(sqlite3* db, const fs::path& dest, std::string& error) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, "VACUUM INTO ?", -1, &stmt, nullptr) != SQLITE_OK) {
        error = sqlite3_errmsg(db);
        return false;
    }
    const std::string dest_utf8 = utf8_path(dest);
    sqlite3_bind_text(stmt, 1, dest_utf8.c_str(), -1, SQLITE_TRANSIENT);
    const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    if (!ok) error = sqlite3_errmsg(db);
    sqlite3_finalize(stmt);
    return ok;
}

namespace {

bool flush_to_disk(const fs::path& path, std::string& error) {
#ifdef _WIN32
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = "cannot open " + utf8_path(path) + " to flush it";
        return false;
    }
    const bool ok = FlushFileBuffers(file) != 0;
    CloseHandle(file);
#else
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        error = "cannot open " + utf8_path(path) + " to flush it";
        return false;
    }
    const bool ok = ::fsync(fd) == 0;
    ::close(fd);
#endif
    if (!ok) error = "cannot flush " + utf8_path(path);
    return ok;
}

} // namespace

bool commit_file(const fs::path& partial, const fs::path& final_path, std::string& error) {
    if (!flush_to_disk(partial, error)) return false;
#ifdef _WIN32
    // No MOVEFILE_REPLACE_EXISTING: a copy must never overwrite a live database.
    if (!MoveFileExW(partial.c_str(), final_path.c_str(), MOVEFILE_WRITE_THROUGH)) {
        error = "cannot rename " + utf8_path(partial) + " to " + utf8_path(final_path) +
                ": " + std::system_category().message(static_cast<int>(GetLastError()));
        return false;
    }
#else
    std::error_code ec;
    if (fs::exists(final_path, ec)) {
        error = utf8_path(final_path) + " already exists";
        return false;
    }
    fs::rename(partial, final_path, ec);
    if (ec) {
        error = "cannot rename " + utf8_path(partial) + ": " + ec.message();
        return false;
    }
#endif
    return true;
}

bool write_snapshot(sqlite3* db, const fs::path& dest, std::string& error) {
    const fs::path partial = partial_path(dest);
    std::error_code ec;
    fs::remove(partial, ec);
    if (!vacuum_into(db, partial, error)) {
        fs::remove(partial, ec);
        return false;
    }
    if (!commit_file(partial, dest, error)) {
        fs::remove(partial, ec);
        return false;
    }
    return true;
}

} // namespace parties::client
