#include <client/legacy_import.h>
#include <client/sqlite_file.h>

#include <parties/log.h>

#include <sqlite3.h>

#include <cstring>
#include <memory>
#include <system_error>

namespace parties::client {

namespace fs = std::filesystem;

LegacyImportPlan plan_legacy_import(std::span<const LegacyCandidate> candidates) {
    auto better = [&](std::size_t a, std::size_t b) {
        if (candidates[a].identity.has_value() != candidates[b].identity.has_value())
            return candidates[a].identity.has_value();
        return candidates[a].modified > candidates[b].modified;
    };

    LegacyImportPlan plan;
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        if (candidates[i].readable && (!plan.import || better(i, *plan.import)))
            plan.import = i;
    }
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        if (i != plan.import) plan.preserve.push_back(i);
    }
    return plan;
}

namespace {

struct DbCloser {
    void operator()(sqlite3* db) const { sqlite3_close(db); }
};
using Db = std::unique_ptr<sqlite3, DbCloser>;

Db open_read_only(const fs::path& path) {
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(utf8_path(path).c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        sqlite3_close(db);
        return nullptr;
    }
    sqlite3_busy_timeout(db, 2000);
    return Db(db);
}

bool integrity_ok(sqlite3* db) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, "PRAGMA integrity_check", -1, &stmt, nullptr) != SQLITE_OK)
        return false;
    bool ok = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const auto* result = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        ok = result && std::strcmp(result, "ok") == 0;
    }
    sqlite3_finalize(stmt);
    return ok;
}

std::optional<PublicKey> identity_key(sqlite3* db) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT public_key FROM identity WHERE id = 1", -1, &stmt, nullptr) != SQLITE_OK)
        return std::nullopt;
    std::optional<PublicKey> key;
    if (sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_bytes(stmt, 0) == 32) {
        key.emplace();
        std::memcpy(key->data(), sqlite3_column_blob(stmt, 0), 32);
    }
    sqlite3_finalize(stmt);
    return key;
}

fs::path sidecar(const fs::path& db, const char* suffix) {
    fs::path p = db;
    p += suffix;
    return p;
}

fs::file_time_type newest_write(const fs::path& db) {
    std::error_code ec;
    fs::file_time_type newest = fs::last_write_time(db, ec);
    const fs::file_time_type wal = fs::last_write_time(sidecar(db, "-wal"), ec);
    if (!ec && wal > newest) newest = wal;
    return newest;
}

LegacyCandidate probe(const fs::path& path) {
    LegacyCandidate candidate;
    candidate.path = path;
    candidate.modified = newest_write(path);
    if (Db db = open_read_only(path); db && integrity_ok(db.get())) {
        candidate.readable = true;
        candidate.identity = identity_key(db.get());
    }
    return candidate;
}

std::vector<LegacyCandidate> find_candidates(const DataLocation& location,
                                             std::span<const fs::path> search_dirs) {
    std::vector<LegacyCandidate> found;
    std::error_code ec;
    for (const fs::path& dir : search_dirs) {
        const fs::path path = dir / "parties_client.db";
        if (!fs::is_regular_file(path, ec)) continue;
        bool duplicate = fs::equivalent(path, location.db_path(), ec);
        for (const LegacyCandidate& seen : found)
            duplicate = duplicate || fs::equivalent(path, seen.path, ec);
        if (!duplicate) found.push_back(probe(path));
    }
    return found;
}

bool copy_raw(const fs::path& from, const fs::path& to, std::string& error) {
    const fs::path partial = partial_path(to);
    std::error_code ec;
    fs::remove(partial, ec);
    if (!fs::copy_file(from, partial, ec)) {
        error = "cannot copy " + utf8_path(from) + ": " + ec.message();
        return false;
    }
    return commit_file(partial, to, error);
}

bool preserve(const LegacyCandidate& candidate, const fs::path& dest, std::string& error) {
    if (candidate.readable) {
        Db db = open_read_only(candidate.path);
        if (db) return write_snapshot(db.get(), dest, error);
    }
    // Unreadable files are kept byte for byte so nothing is lost even when
    // SQLite cannot make sense of them.
    std::error_code ec;
    const fs::path wal = sidecar(candidate.path, "-wal");
    if (fs::exists(wal, ec) && !copy_raw(wal, sidecar(dest, "-wal"), error)) return false;
    return copy_raw(candidate.path, dest, error);
}

bool import_copy(const LegacyCandidate& candidate, const fs::path& db_path, std::string& error) {
    Db source = open_read_only(candidate.path);
    if (!source) {
        error = "cannot open " + utf8_path(candidate.path) + " read-only";
        return false;
    }
    const fs::path partial = partial_path(db_path);
    if (!vacuum_into(source.get(), partial, error)) return false;

    {
        Db copy = open_read_only(partial);
        if (!copy || !integrity_ok(copy.get())) {
            error = "the copy failed its integrity check";
            return false;
        }
        if (identity_key(copy.get()) != identity_key(source.get())) {
            error = "the copied identity does not match the source";
            return false;
        }
    }
    return commit_file(partial, db_path, error);
}

void remove_partial(const fs::path& db_path) {
    std::error_code ec;
    const fs::path partial = partial_path(db_path);
    fs::remove(partial, ec);
    fs::remove(sidecar(partial, "-journal"), ec);
}

} // namespace

LegacyImportResult import_legacy_data(const DataLocation& location,
                                      std::span<const fs::path> search_dirs) {
    using Status = LegacyImportResult::Status;
    const fs::path db_path = location.db_path();
    remove_partial(db_path);

    LegacyImportResult result;
    std::error_code ec;
    if (fs::exists(db_path, ec)) return result;

    const std::vector<LegacyCandidate> candidates = find_candidates(location, search_dirs);
    const LegacyImportPlan plan = plan_legacy_import(candidates);

    // Preserve first: a crash after the import would otherwise skip the copies
    // forever, because the next start sees a database and does nothing.
    const std::string stamp = utc_file_stamp();
    if (!plan.preserve.empty()) fs::create_directories(location.backups_dir, ec);
    for (std::size_t i = 0; i < plan.preserve.size(); ++i) {
        const LegacyCandidate& candidate = candidates[plan.preserve[i]];
        const fs::path dest = unique_backup_path(
            location.backups_dir, "legacy-" + stamp + "-" + std::to_string(i));
        if (!preserve(candidate, dest, result.error)) {
            result.status = Status::Failed;
            result.error = "cannot preserve " + utf8_path(candidate.path) + ": " + result.error;
            LOG_ERROR("Legacy import: {}", result.error);
            return result;
        }
        LOG_INFO("Legacy import: preserved {} as {}", utf8_path(candidate.path), utf8_path(dest));
        result.preserved.push_back(dest);
    }

    if (!plan.import) {
        result.status = Status::NoneImported;
        return result;
    }

    const LegacyCandidate& chosen = candidates[*plan.import];
    if (!import_copy(chosen, db_path, result.error)) {
        remove_partial(db_path);
        result.status = Status::Failed;
        result.error = "cannot import " + utf8_path(chosen.path) + ": " + result.error;
        LOG_ERROR("Legacy import: {}", result.error);
        return result;
    }
    LOG_INFO("Legacy import: imported {} into {}", utf8_path(chosen.path), utf8_path(db_path));
    result.status = Status::Imported;
    result.imported_from = chosen.path;
    return result;
}

} // namespace parties::client
