#pragma once

#include <filesystem>
#include <string>

struct sqlite3;

namespace parties::client {

// SQLite takes UTF-8 paths; std::filesystem::path::string() would use the ANSI
// code page on Windows and break profiles with non-ASCII user names.
std::string utf8_path(const std::filesystem::path& path);

// Where a copy destined for `final_path` is written before it is complete.
// A file at this path is always left over from an interrupted run.
std::filesystem::path partial_path(const std::filesystem::path& final_path);

// Compact UTC timestamp for file names, e.g. 20261031T172600Z.
std::string utc_file_stamp();

// `<dir>/<stem>.db`, or `<stem>-2.db` and so on when that name is taken.
std::filesystem::path unique_backup_path(const std::filesystem::path& dir, const std::string& stem);

// Consistent single-file copy of `db` (WAL content included) at `dest`,
// which must not exist yet.
bool vacuum_into(sqlite3* db, const std::filesystem::path& dest, std::string& error);

// Flushes `partial` to stable storage and renames it to `final_path`, so
// `final_path` either does not exist or is complete.
bool commit_file(const std::filesystem::path& partial,
                 const std::filesystem::path& final_path, std::string& error);

// vacuum_into a partial file, then commit_file. Removes a stale partial first.
bool write_snapshot(sqlite3* db, const std::filesystem::path& dest, std::string& error);

} // namespace parties::client
