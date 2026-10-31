#include "data_guard.h"

#include "file_io.h"
#include "log.h"

#include <client/sqlite_file.h>
#include <parties/update/retention.h>

#include <sqlite3.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <vector>

#include <windows.h>

namespace parties::update::launcher {

namespace {

class Db {
public:
    // immutable: the file is a finished copy, so SQLite takes no locks and
    // never creates -wal or -shm beside it.
    Db(const fs::path& path, bool immutable) {
        std::string uri = "file:" + client::utf8_path(path);
        for (size_t i = 0; i < uri.size(); ++i) {
            if (uri[i] == '\\') uri[i] = '/';
            else if (uri[i] == '?' || uri[i] == '#' || uri[i] == '%') {
                char hex[4];
                std::snprintf(hex, sizeof hex, "%%%02X", static_cast<unsigned char>(uri[i]));
                uri.replace(i, 1, hex);
                i += 2;
            }
        }
        if (immutable) uri += "?immutable=1";
        if (sqlite3_open_v2(uri.c_str(), &db_, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, nullptr) != SQLITE_OK) {
            error_ = db_ ? sqlite3_errmsg(db_) : "out of memory";
            sqlite3_close(db_);
            db_ = nullptr;
        }
    }
    ~Db() { sqlite3_close(db_); }
    Db(const Db&) = delete;
    Db& operator=(const Db&) = delete;

    sqlite3* get() const { return db_; }
    const std::string& error() const { return error_; }

private:
    sqlite3* db_ = nullptr;
    std::string error_;
};

bool integrity_ok(sqlite3* db) {
    sqlite3_stmt* stmt = nullptr;
    bool ok = sqlite3_prepare_v2(db, "PRAGMA integrity_check", -1, &stmt, nullptr) == SQLITE_OK &&
              sqlite3_step(stmt) == SQLITE_ROW &&
              std::string_view(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0))) == "ok";
    sqlite3_finalize(stmt);
    return ok;
}

bool copy_intact(const fs::path& path) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) return false;
    Db db(path, true);
    return db.get() && integrity_ok(db.get());
}

std::expected<DataInfo, std::string> read_info(sqlite3* db) {
    sqlite3_stmt* stmt = nullptr;
    int has_table = -1;
    if (sqlite3_prepare_v2(db, "SELECT count(*) FROM sqlite_master WHERE type = 'table' AND name = 'parties_schema'",
                           -1, &stmt, nullptr) == SQLITE_OK &&
        sqlite3_step(stmt) == SQLITE_ROW)
        has_table = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    if (has_table < 0) return std::unexpected(std::string(sqlite3_errmsg(db)));
    if (has_table == 0) return DataInfo::legacy();

    std::optional<DataInfo> info;
    if (sqlite3_prepare_v2(db, "SELECT version, compat_floor FROM parties_schema WHERE id = 1", -1, &stmt,
                           nullptr) == SQLITE_OK &&
        sqlite3_step(stmt) == SQLITE_ROW) {
        int64_t version = sqlite3_column_int64(stmt, 0);
        int64_t floor = sqlite3_column_int64(stmt, 1);
        if (version >= 1 && floor >= 1 && floor <= version && version <= UINT32_MAX)
            info = DataInfo{static_cast<uint32_t>(version), static_cast<uint32_t>(floor)};
    }
    sqlite3_finalize(stmt);
    if (!info) return std::unexpected(std::string("parties_schema is unreadable"));
    return *info;
}

bool same_bytes(const fs::path& a, const fs::path& b) {
    std::error_code ec;
    if (!fs::is_regular_file(a, ec) || !fs::is_regular_file(b, ec)) return false;
    if (fs::file_size(a, ec) != fs::file_size(b, ec) || ec) return false;
    std::ifstream fa(a, std::ios::binary), fb(b, std::ios::binary);
    std::vector<char> ba(1 << 16), bb(1 << 16);
    while (fa && fb) {
        fa.read(ba.data(), static_cast<std::streamsize>(ba.size()));
        fb.read(bb.data(), static_cast<std::streamsize>(bb.size()));
        if (fa.gcount() != fb.gcount() || !std::equal(ba.begin(), ba.begin() + fa.gcount(), bb.begin()))
            return false;
    }
    return true;
}

fs::path with_suffix(fs::path p, const char* suffix) {
    p += suffix;
    return p;
}

bool has_wal_content(const fs::path& db) {
    std::error_code ec;
    fs::path wal = with_suffix(db, "-wal");
    return fs::exists(wal, ec) && fs::file_size(wal, ec) > 0;
}

// VACUUM INTO a partial file, integrity_check it, flush, rename into place.
bool consistent_copy(const fs::path& source, const fs::path& dest, std::string& error) {
    const fs::path partial = client::partial_path(dest);
    std::error_code ec;
    fs::remove(partial, ec);
    Db db(source, false);
    if (!db.get()) {
        error = "cannot open " + utf8(source) + ": " + db.error();
        return false;
    }
    if (!client::vacuum_into(db.get(), partial, error)) {
        fs::remove(partial, ec);
        return false;
    }
    if (!copy_intact(partial)) {
        fs::remove(partial, ec);
        error = "the copy of " + utf8(source) + " failed its integrity check";
        return false;
    }
    if (!client::commit_file(partial, dest, error)) {
        fs::remove(partial, ec);
        return false;
    }
    return true;
}

// The database cannot be read by SQLite (corrupt), so keep its raw bytes.
bool raw_copy(const fs::path& source, const fs::path& dest, std::string& error) {
    for (const char* suffix : {"-wal", ""}) {
        fs::path from = with_suffix(source, suffix);
        fs::path to = with_suffix(dest, suffix);
        std::error_code ec;
        if (!fs::exists(from, ec)) continue;
        fs::path partial = client::partial_path(to);
        fs::remove(partial, ec);
        if (!CopyFileW(from.c_str(), partial.c_str(), FALSE)) {
            error = "cannot copy " + utf8(from) + ": " + last_error_text();
            return false;
        }
        if (!client::commit_file(partial, to, error)) return false;
    }
    return true;
}

} // namespace

fs::path DataGuard::backup_path(const SnapshotId& id) const {
    return paths_.backups() / (id.stem + ".db");
}

std::expected<std::optional<DataInfo>, std::string> DataGuard::inspect() const {
    std::error_code ec;
    if (!fs::exists(paths_.db(), ec)) return std::optional<DataInfo>{};
    Db db(paths_.db(), false);
    if (!db.get()) return std::unexpected("cannot open " + utf8(paths_.db()) + ": " + db.error());
    auto info = read_info(db.get());
    if (!info) return std::unexpected(info.error());
    return std::optional<DataInfo>(*info);
}

bool DataGuard::snapshot(const SnapshotId& id, std::string& error) const {
    const fs::path dest = backup_path(id);
    std::error_code ec;
    fs::create_directories(paths_.backups(), ec);
    if (fs::exists(dest, ec)) {
        if (copy_intact(dest)) return true;
        fs::path aside = with_suffix(dest, (".damaged-" + std::to_string(GetTickCount64())).c_str());
        if (!MoveFileExW(dest.c_str(), aside.c_str(), MOVEFILE_WRITE_THROUGH)) {
            error = "cannot move the damaged " + utf8(dest) + " aside: " + last_error_text();
            return false;
        }
    }
    if (!consistent_copy(paths_.db(), dest, error)) return false;
    log("snapshot {} written", id.stem);
    return true;
}

bool DataGuard::restore(const SnapshotId& from, const SnapshotId& preserve_live_as, std::string& error) const {
    const fs::path snap = backup_path(from);
    const fs::path live = paths_.db();
    if (!copy_intact(snap)) {
        error = "backup " + utf8(snap) + " is missing or damaged";
        return false;
    }
    if (!has_wal_content(live) && same_bytes(live, snap)) return true;

    std::error_code ec;
    const fs::path preserved = backup_path(preserve_live_as);
    if (fs::exists(live, ec) && !fs::exists(preserved, ec)) {
        std::string why;
        if (!consistent_copy(live, preserved, why)) {
            log("cannot copy the live database consistently ({}), keeping its raw bytes", why);
            if (!raw_copy(live, preserved, error)) return false;
        }
        log("live database preserved as {}", preserve_live_as.stem);
    }

    for (const char* suffix : {"-wal", "-shm"}) {
        fs::path side = with_suffix(live, suffix);
        if (fs::exists(side, ec) && !DeleteFileW(side.c_str())) {
            error = "cannot remove " + utf8(side) + ": " + last_error_text();
            return false;
        }
    }
    fs::create_directories(paths_.data(), ec);
    const fs::path partial = with_suffix(live, ".restore.partial");
    fs::remove(partial, ec);
    if (!CopyFileW(snap.c_str(), partial.c_str(), FALSE)) {
        error = "cannot copy " + utf8(snap) + ": " + last_error_text();
        return false;
    }
    if (!flush_file(partial, error)) return false;
    if (!MoveFileExW(partial.c_str(), live.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = "cannot put " + utf8(snap) + " in place: " + last_error_text();
        return false;
    }
    log("restored {} (live kept as {})", from.stem, preserve_live_as.stem);
    return true;
}

std::vector<Backup> DataGuard::list() const {
    std::vector<Backup> out;
    std::error_code ec;
    for (const auto& item : fs::directory_iterator(paths_.backups(), ec)) {
        if (!item.is_regular_file(ec) || item.path().extension() != ".db") continue;
        Db db(item.path(), true);
        if (!db.get() || !integrity_ok(db.get())) continue;
        auto info = read_info(db.get());
        if (!info) continue;
        out.push_back({SnapshotId{item.path().stem().string()}, *info, item.last_write_time(ec)});
    }
    std::ranges::sort(out, [](const Backup& a, const Backup& b) { return a.written > b.written; });
    return out;
}

void DataGuard::prune(const SupervisorState& committed) const {
    std::vector<BackupFile> files;
    std::error_code ec;
    for (const auto& item : fs::directory_iterator(paths_.backups(), ec)) {
        if (!item.is_regular_file(ec) || item.path().extension() != ".db") continue;
        files.push_back({item.path().stem().string(), item.last_write_time(ec)});
    }
    for (const std::string& stem : backups_to_prune(files, committed)) {
        fs::path file = backup_path(SnapshotId{stem});
        if (DeleteFileW(file.c_str())) log("retention deleted {}", stem);
        else log("retention cannot delete {}: {}", stem, last_error_text());
    }
}

} // namespace parties::update::launcher
