#include <client/legacy_import.h>
#include <client/sqlite_file.h>

#include <parties/log.h>

#include <sqlite3.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
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

// Missing files count as equal, so a copy without a -wal matches a backup without one.
bool same_bytes(const fs::path& a, const fs::path& b) {
    std::error_code ec_a, ec_b;
    const bool has_a = fs::exists(a, ec_a), has_b = fs::exists(b, ec_b);
    if (!has_a || !has_b) return has_a == has_b;
    if (fs::file_size(a, ec_a) != fs::file_size(b, ec_b) || ec_a || ec_b) return false;
    std::ifstream in_a(a, std::ios::binary), in_b(b, std::ios::binary);
    return std::equal(std::istreambuf_iterator<char>(in_a), std::istreambuf_iterator<char>(),
                      std::istreambuf_iterator<char>(in_b), std::istreambuf_iterator<char>());
}

// A legacy-* backup holding exactly `db` and its -wal.
std::optional<fs::path> existing_copy(const fs::path& backups_dir, const fs::path& db) {
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(backups_dir, ec)) {
        const std::string name = utf8_path(entry.path().filename());
        if (!name.starts_with("legacy-") || !name.ends_with(".db") || !entry.is_regular_file(ec)) continue;
        if (same_bytes(entry.path(), db) && same_bytes(sidecar(entry.path(), "-wal"), sidecar(db, "-wal")))
            return entry.path();
    }
    return std::nullopt;
}

// Copies `candidate` to `dest` and returns `dest`, or returns an empty path
// when a legacy backup already holds the same bytes. VACUUM INTO writes the
// same file for the same content, so an unchanged source is copied once.
std::optional<fs::path> preserve(const LegacyCandidate& candidate, const fs::path& dest, std::string& error) {
    const fs::path backups_dir = dest.parent_path();
    std::error_code ec;
    if (candidate.readable) {
        if (Db db = open_read_only(candidate.path)) {
            const fs::path partial = partial_path(dest);
            fs::remove(partial, ec);
            if (!vacuum_into(db.get(), partial, error)) {
                fs::remove(partial, ec);
                return std::nullopt;
            }
            if (existing_copy(backups_dir, partial)) {
                fs::remove(partial, ec);
                return fs::path();
            }
            if (!commit_file(partial, dest, error)) return std::nullopt;
            return dest;
        }
    }
    // Unreadable files are kept byte for byte so nothing is lost even when
    // SQLite cannot make sense of them.
    if (existing_copy(backups_dir, candidate.path)) return fs::path();
    const fs::path wal = sidecar(candidate.path, "-wal");
    if (fs::exists(wal, ec) && !copy_raw(wal, sidecar(dest, "-wal"), error)) return std::nullopt;
    if (!copy_raw(candidate.path, dest, error)) return std::nullopt;
    return dest;
}

bool preserve_all(const DataLocation& location, const std::vector<LegacyCandidate>& candidates,
                  std::span<const std::size_t> indices, LegacyImportResult& result) {
    if (indices.empty()) return true;
    std::error_code ec;
    fs::create_directories(location.backups_dir, ec);
    const std::string stamp = utc_file_stamp();
    for (std::size_t i = 0; i < indices.size(); ++i) {
        const LegacyCandidate& candidate = candidates[indices[i]];
        const fs::path dest =
            unique_backup_path(location.backups_dir, "legacy-" + stamp + "-" + std::to_string(i));
        const std::optional<fs::path> kept = preserve(candidate, dest, result.error);
        if (!kept) {
            result.status = LegacyImportResult::Status::Failed;
            result.error = "cannot preserve " + utf8_path(candidate.path) + ": " + result.error;
            LOG_ERROR("Legacy import: {}", result.error);
            return false;
        }
        if (kept->empty()) continue;
        LOG_INFO("Legacy import: preserved {} as {}", utf8_path(candidate.path), utf8_path(*kept));
        result.preserved.push_back(*kept);
    }
    return true;
}

// A verified copy of `source` at partial_path(db_path), not yet in place.
bool prepare_copy(const fs::path& source_path, const fs::path& db_path, std::string& error) {
    Db source = open_read_only(source_path);
    if (!source) {
        error = "cannot open " + utf8_path(source_path) + " read-only";
        return false;
    }
    const fs::path partial = partial_path(db_path);
    if (!vacuum_into(source.get(), partial, error)) return false;
    Db copy = open_read_only(partial);
    if (!copy || !integrity_ok(copy.get())) {
        error = "the copy failed its integrity check";
        return false;
    }
    if (identity_key(copy.get()) != identity_key(source.get())) {
        error = "the copied identity does not match the source";
        return false;
    }
    return true;
}

bool import_copy(const LegacyCandidate& candidate, const fs::path& db_path, std::string& error) {
    return prepare_copy(candidate.path, db_path, error) && commit_file(partial_path(db_path), db_path, error);
}

bool archived(sqlite3* db, const PublicKey& key) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT count(*) FROM identity_archive WHERE public_key = ?", -1, &stmt, nullptr) !=
        SQLITE_OK)
        return false;
    sqlite3_bind_blob(stmt, 1, key.data(), static_cast<int>(key.size()), SQLITE_STATIC);
    const bool found = sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_int64(stmt, 0) > 0;
    sqlite3_finalize(stmt);
    return found;
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
    if (!preserve_all(location, candidates, plan.preserve, result)) return result;

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

LegacyImportResult preserve_legacy_data(const DataLocation& location,
                                        std::span<const fs::path> search_dirs) {
    LegacyImportResult result;
    const std::vector<LegacyCandidate> candidates = find_candidates(location, search_dirs);
    std::vector<std::size_t> all(candidates.size());
    for (std::size_t i = 0; i < all.size(); ++i) all[i] = i;
    preserve_all(location, candidates, all, result);
    return result;
}

std::optional<IdentityConflict> find_identity_conflict(const DataLocation& location,
                                                       std::span<const fs::path> search_dirs,
                                                       std::span<const PublicKey> settled) {
    Db live = open_read_only(location.db_path());
    if (!live) return std::nullopt;
    const std::optional<PublicKey> live_key = identity_key(live.get());
    std::vector<LegacyCandidate> candidates = find_candidates(location, search_dirs);
    std::erase_if(candidates, [&](const LegacyCandidate& c) {
        return !c.readable || !c.identity || c.identity == live_key || archived(live.get(), *c.identity) ||
               std::ranges::find(settled, *c.identity) != settled.end();
    });
    const LegacyImportPlan plan = plan_legacy_import(candidates);
    if (!plan.import) return std::nullopt;
    const LegacyCandidate& best = candidates[*plan.import];
    return IdentityConflict{best.path, *best.identity, live_key};
}

bool adopt_legacy_database(const DataLocation& location, const fs::path& legacy_db, fs::path& kept,
                           std::string& error) {
    const fs::path db_path = location.db_path();
    std::error_code ec;
    fs::create_directories(location.backups_dir, ec);
    kept = unique_backup_path(location.backups_dir, "pre-identity-switch-" + utc_file_stamp());
    {
        Db live = open_read_only(db_path);
        if (!live || !write_snapshot(live.get(), kept, error)) {
            if (error.empty()) error = "cannot open " + utf8_path(db_path);
            return false;
        }
    }
    remove_partial(db_path);
    if (!prepare_copy(legacy_db, db_path, error)) {
        remove_partial(db_path);
        return false;
    }
    // The live data is safe in `kept`. A crash before the rename leaves no
    // database, and the next start imports the legacy one anyway.
    for (const char* suffix : {"", "-wal", "-shm"}) {
        const fs::path file = sidecar(db_path, suffix);
        if (fs::exists(file, ec) && !fs::remove(file, ec)) {
            error = "cannot replace " + utf8_path(file) + ": " + ec.message();
            remove_partial(db_path);
            return false;
        }
    }
    if (!commit_file(partial_path(db_path), db_path, error)) return false;
    LOG_INFO("Legacy import: {} replaced the live database, which is kept as {}", utf8_path(legacy_db),
             utf8_path(kept));
    return true;
}

} // namespace parties::client
