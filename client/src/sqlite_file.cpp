#include <client/sqlite_file.h>

#include <sqlite3.h>

#include <cerrno>
#include <cstring>
#include <ctime>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <stdio.h>  // renameatx_np
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

#ifndef _WIN32
// fsync leaves the data in the drive's write cache on macOS. F_FULLFSYNC also
// flushes the cache; a file system without it falls back to fsync.
bool sync_fd(int fd) {
#ifdef F_FULLFSYNC
    if (::fcntl(fd, F_FULLFSYNC) == 0) return true;
#endif
    return ::fsync(fd) == 0;
}
#endif

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
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        error = "cannot open " + utf8_path(path) + " to flush it";
        return false;
    }
    const bool ok = sync_fd(fd);
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
    const fs::path dir = final_path.parent_path();
    const int dir_fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dir_fd < 0) {
        error = "cannot open " + utf8_path(dir) + ": " + std::strerror(errno);
        return false;
    }
    // Exclusive and atomic: a copy never replaces a live database, and nothing
    // can appear at final_path between a check and the rename. The new name is
    // resolved in the directory that is flushed below.
    const std::string name = final_path.filename().string();
#ifdef __APPLE__
    const bool moved = ::renameatx_np(AT_FDCWD, partial.c_str(), dir_fd, name.c_str(), RENAME_EXCL) == 0;
#else
    const bool moved = ::linkat(AT_FDCWD, partial.c_str(), dir_fd, name.c_str(), 0) == 0 &&
                       ::unlink(partial.c_str()) == 0;
#endif
    const int rename_errno = errno;
    // The rename is durable only once the directory entry is on disk.
    const bool synced = moved && sync_fd(dir_fd);
    ::close(dir_fd);
    if (!moved) {
        error = "cannot rename " + utf8_path(partial) + " to " + utf8_path(final_path) + ": " +
                std::strerror(rename_errno);
        return false;
    }
    if (!synced) {
        error = "cannot flush " + utf8_path(dir) + " after renaming " + utf8_path(partial);
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
