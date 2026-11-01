#include <client/settings.h>
#include <client/sqlite_file.h>
#include <parties/crypto.h>

#include <parties/log.h>

#include <sqlite3.h>

#include <algorithm>
#include <cstring>
#include <string_view>
#include <system_error>

namespace parties::client {

namespace fs = std::filesystem;

namespace {

constexpr const char* kSchemaTableSql =
    "CREATE TABLE IF NOT EXISTS parties_schema ("
    "id INTEGER PRIMARY KEY CHECK (id = 1), "
    "version INTEGER NOT NULL, "
    "compat_floor INTEGER NOT NULL)";

struct VersionRead {
    bool            ok = false;
    schema::Version version;
    std::string     error;
};

VersionRead read_version(sqlite3* db) {
    VersionRead out;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT name FROM sqlite_master WHERE type = 'table' "
                               "AND name NOT LIKE 'sqlite_%'", -1, &stmt, nullptr) != SQLITE_OK) {
        out.error = sqlite3_errmsg(db);
        return out;
    }
    bool has_tables = false;
    bool has_schema_table = false;
    int rc = SQLITE_ROW;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        has_tables = true;
        const auto* name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        if (name && std::string_view(name) == "parties_schema") has_schema_table = true;
    }
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        out.error = sqlite3_errmsg(db);
        return out;
    }
    if (!has_schema_table) {
        out.ok = true;
        out.version = has_tables ? schema::kLegacy : schema::Version{};
        return out;
    }

    if (sqlite3_prepare_v2(db, "SELECT version, compat_floor FROM parties_schema WHERE id = 1",
                           -1, &stmt, nullptr) != SQLITE_OK) {
        out.error = sqlite3_errmsg(db);
        return out;
    }
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        out.ok = true;
        out.version = {sqlite3_column_int(stmt, 0), sqlite3_column_int(stmt, 1)};
    } else {
        out.error = "parties_schema has no version row";
    }
    sqlite3_finalize(stmt);
    return out;
}

bool write_version(sqlite3* db, schema::Version v, std::string& error) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO parties_schema (id, version, compat_floor) "
                               "VALUES (1, ?, ?)", -1, &stmt, nullptr) != SQLITE_OK) {
        error = sqlite3_errmsg(db);
        return false;
    }
    sqlite3_bind_int(stmt, 1, v.version);
    sqlite3_bind_int(stmt, 2, v.compat_floor);
    const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    if (!ok) error = sqlite3_errmsg(db);
    sqlite3_finalize(stmt);
    return ok;
}

bool add_column_if_missing(sqlite3* db, const schema::ColumnAddition& add, std::string& error) {
    sqlite3_stmt* stmt = nullptr;
    const std::string table_info = std::string("PRAGMA table_info(") + add.table + ")";
    if (sqlite3_prepare_v2(db, table_info.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        error = sqlite3_errmsg(db);
        return false;
    }
    bool has_column = false;
    int rc = SQLITE_ROW;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        const auto* name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        if (name && std::strcmp(name, add.column) == 0) {
            has_column = true;
            break;
        }
    }
    sqlite3_finalize(stmt);
    if (!has_column && rc != SQLITE_DONE) {
        error = sqlite3_errmsg(db);
        return false;
    }
    if (has_column) return true;

    const std::string alter = std::string("ALTER TABLE ") + add.table + " ADD COLUMN " +
                              add.column + " " + add.definition;
    char* err = nullptr;
    if (sqlite3_exec(db, alter.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
        error = err ? err : "ALTER TABLE failed";
        sqlite3_free(err);
        return false;
    }
    return true;
}

template <class Body>
bool in_transaction(sqlite3* db, Body&& body) {
    if (sqlite3_exec(db, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) != SQLITE_OK) {
        LOG_ERROR("Failed to begin settings transaction: {}", sqlite3_errmsg(db));
        return false;
    }
    if (body() && sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr) == SQLITE_OK)
        return true;
    LOG_ERROR("Settings transaction failed: {}", sqlite3_errmsg(db));
    sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
    return false;
}

Identity identity_from_row(sqlite3_stmt* stmt) {
    Identity id;
    const auto* seed = sqlite3_column_text(stmt, 0);
    id.seed_phrase = seed ? reinterpret_cast<const char*>(seed) : "";

    auto* pk_blob = static_cast<const uint8_t*>(sqlite3_column_blob(stmt, 1));
    int pk_len = sqlite3_column_bytes(stmt, 1);
    if (pk_blob && pk_len == 32)
        std::memcpy(id.public_key.data(), pk_blob, 32);

    auto* sk_blob = static_cast<const uint8_t*>(sqlite3_column_blob(stmt, 2));
    int sk_len = sqlite3_column_bytes(stmt, 2);
    if (sk_blob && sk_len == 32)
        std::memcpy(id.secret_key.data(), sk_blob, 32);
    return id;
}

} // namespace

Settings::Settings() = default;

Settings::~Settings() {
    close();
}

Settings::OpenResult Settings::open(const SettingsPaths& paths) {
    return open_with(paths, schema::kMigrations);
}

Settings::OpenResult Settings::open_with(const SettingsPaths& paths,
                                         std::span<const schema::Migration> migrations) {
    close();
    auto fail = [&](OpenStatus status, std::string reason) {
        LOG_ERROR("Cannot use settings database {}: {}", utf8_path(paths.db), reason);
        close();
        return OpenResult{status, std::move(reason)};
    };

    if (sqlite3_open_v2(utf8_path(paths.db).c_str(), &db_,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK)
        return fail(OpenStatus::Unavailable, std::string("cannot open the database: ") +
                                             (db_ ? sqlite3_errmsg(db_) : "out of memory"));
    sqlite3_busy_timeout(db_, 5000);

    // Nothing may be written before this check: a TooNew database stays byte-identical.
    const VersionRead found = read_version(db_);
    if (!found.ok)
        return fail(OpenStatus::Unavailable, "cannot read the database: " + found.error);

    const schema::Version target = schema::target_of(migrations);
    const update::Compatibility action = schema::compatibility(found.version, target.version);
    if (action == update::Compatibility::Incompatible)
        return fail(OpenStatus::TooNew,
                    "the data was written by a newer version of Parties (data version " +
                    std::to_string(found.version.version) + ", readable from version " +
                    std::to_string(found.version.compat_floor) + "), and this version reads up to " +
                    std::to_string(target.version));

    std::string error;
    exec("PRAGMA journal_mode=WAL;", error);
    exec("PRAGMA foreign_keys=ON;", error);

    if (action == update::Compatibility::ReadsAsIs)
        return {OpenStatus::Ready, {}};

    if (found.version.version >= 1) {
        std::error_code ec;
        fs::create_directories(paths.backups_dir, ec);
        const fs::path snapshot = unique_backup_path(
            paths.backups_dir, "pre-migration-v" + std::to_string(found.version.version) + "-" +
                               utc_file_stamp());
        if (ec || !write_snapshot(db_, snapshot, error))
            return fail(OpenStatus::Unavailable, "cannot write the pre-migration backup " +
                                                 utf8_path(snapshot) + ": " +
                                                 (ec ? ec.message() : error));
        LOG_INFO("Wrote pre-migration backup {}", utf8_path(snapshot));
    }

    for (const schema::Migration& migration : migrations) {
        if (migration.to_version <= found.version.version) continue;
        if (!apply_migration(migration, error))
            return fail(OpenStatus::Unavailable, "migration to data version " +
                                                 std::to_string(migration.to_version) +
                                                 " failed: " + error);
        LOG_INFO("Migrated settings database to data version {}", migration.to_version);
    }
    return {OpenStatus::Ready, {}};
}

bool Settings::apply_migration(const schema::Migration& migration, std::string& error) {
    if (!exec("BEGIN IMMEDIATE", error)) return false;

    // Re-read under the write lock so a step another process already applied is skipped.
    const VersionRead current = read_version(db_);
    std::string ignored;
    if (!current.ok) {
        error = current.error;
        exec("ROLLBACK", ignored);
        return false;
    }
    if (current.version.version >= migration.to_version) {
        exec("ROLLBACK", ignored);
        return true;
    }

    bool ok = exec(migration.sql, error);
    for (const schema::ColumnAddition& add : migration.add_columns)
        ok = ok && add_column_if_missing(db_, add, error);
    const schema::Version next{migration.to_version,
                               (std::max)(current.version.compat_floor, migration.raises_floor_to)};
    ok = ok && exec(kSchemaTableSql, error) && write_version(db_, next, error);
    ok = ok && exec("COMMIT", error);
    if (!ok) exec("ROLLBACK", ignored);
    return ok;
}

void Settings::close() {
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

bool Settings::exec(const char* sql, std::string& error) {
    char* err = nullptr;
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        error = err ? err : sqlite3_errmsg(db_);
        sqlite3_free(err);
        return false;
    }
    return true;
}

// --- Identity ---

bool Settings::has_identity() {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT COUNT(*) FROM identity";

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return false;

    bool has = false;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        has = sqlite3_column_int(stmt, 0) > 0;

    sqlite3_finalize(stmt);
    return has;
}

bool Settings::save_identity(const std::string& seed_phrase,
                              const SecretKey& sk, const PublicKey& pk) {
    auto run = [&](const char* sql) {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
            return false;
        sqlite3_bind_text(stmt, 1, seed_phrase.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_blob(stmt, 2, pk.data(), static_cast<int>(pk.size()), SQLITE_TRANSIENT);
        sqlite3_bind_blob(stmt, 3, sk.data(), static_cast<int>(sk.size()), SQLITE_TRANSIENT);
        const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
        sqlite3_finalize(stmt);
        return ok;
    };

    return in_transaction(db_, [&] {
        return run("INSERT INTO identity_archive (seed_phrase, public_key, secret_key, reason) "
                   "SELECT seed_phrase, public_key, secret_key, 'replaced' FROM identity "
                   "WHERE id = 1 AND NOT (seed_phrase = ?1 AND public_key = ?2 AND secret_key = ?3)") &&
               run("INSERT OR REPLACE INTO identity (id, seed_phrase, public_key, secret_key) "
                   "VALUES (1, ?1, ?2, ?3)");
    });
}

bool Settings::delete_identity() {
    std::string error;
    return in_transaction(db_, [&] {
        return exec("INSERT INTO identity_archive (seed_phrase, public_key, secret_key, reason) "
                    "SELECT seed_phrase, public_key, secret_key, 'deleted' FROM identity WHERE id = 1",
                    error) &&
               exec("DELETE FROM identity WHERE id = 1", error);
    });
}

std::optional<Identity> Settings::load_identity() {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT seed_phrase, public_key, secret_key FROM identity WHERE id = 1";

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return std::nullopt;

    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return std::nullopt;
    }

    Identity id = identity_from_row(stmt);
    sqlite3_finalize(stmt);
    return id;
}

std::vector<ArchivedIdentity> Settings::archived_identities() {
    std::vector<ArchivedIdentity> result;
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT seed_phrase, public_key, secret_key, archived_at, reason "
                      "FROM identity_archive ORDER BY id";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return result;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        ArchivedIdentity archived;
        archived.identity = identity_from_row(stmt);
        archived.archived_at = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        archived.reason = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        result.push_back(std::move(archived));
    }
    sqlite3_finalize(stmt);
    return result;
}

std::string Settings::get_fingerprint() {
    auto id = load_identity();
    if (!id) return "";
    return parties::public_key_fingerprint(id->public_key);
}

// --- TOFU ---

Settings::TofuResult Settings::check_fingerprint(const std::string& host, int port,
                                                   const std::string& fingerprint) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT fingerprint FROM tofu_certs WHERE host = ? AND port = ?";

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return TofuResult::Unknown;

    sqlite3_bind_text(stmt, 1, host.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, port);

    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return TofuResult::Unknown;
    }

    std::string stored = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    sqlite3_finalize(stmt);

    return (stored == fingerprint) ? TofuResult::Trusted : TofuResult::Mismatch;
}

bool Settings::trust_fingerprint(const std::string& host, int port,
                                  const std::string& fingerprint) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "INSERT OR REPLACE INTO tofu_certs (host, port, fingerprint) "
                      "VALUES (?, ?, ?)";

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return false;

    sqlite3_bind_text(stmt, 1, host.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, port);
    sqlite3_bind_text(stmt, 3, fingerprint.c_str(), -1, SQLITE_TRANSIENT);

    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

// --- Saved servers ---

bool Settings::save_server(const std::string& name, const std::string& host, int port,
                           const std::string& fingerprint, const std::string& last_username,
                           const std::string& password) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "INSERT INTO saved_servers (name, host, port, fingerprint, last_username, password) "
                      "VALUES (?, ?, ?, ?, ?, ?) "
                      "ON CONFLICT(host, port) DO UPDATE SET "
                      "name=excluded.name, fingerprint=excluded.fingerprint, "
                      "last_username=excluded.last_username, password=excluded.password";

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return false;

    sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, host.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, port);
    sqlite3_bind_text(stmt, 4, fingerprint.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, last_username.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, password.c_str(), -1, SQLITE_TRANSIENT);

    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

std::vector<SavedServer> Settings::get_saved_servers() {
    std::vector<SavedServer> result;
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT id, name, host, port, fingerprint, last_username, password "
                      "FROM saved_servers ORDER BY name";

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return result;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        SavedServer s;
        s.id = sqlite3_column_int(stmt, 0);
        s.name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        s.host = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        s.port = sqlite3_column_int(stmt, 3);
        s.fingerprint = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        s.last_username = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
        auto pw = sqlite3_column_text(stmt, 6);
        s.password = pw ? reinterpret_cast<const char*>(pw) : "";
        result.push_back(std::move(s));
    }

    sqlite3_finalize(stmt);
    return result;
}

bool Settings::update_server_username(int id, const std::string& username) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "UPDATE saved_servers SET last_username = ? WHERE id = ?";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return false;

    sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, id);
    const bool ok = sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(db_) > 0;
    sqlite3_finalize(stmt);
    return ok;
}

bool Settings::delete_server(int id) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "DELETE FROM saved_servers WHERE id = ?";

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return false;

    sqlite3_bind_int(stmt, 1, id);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

// --- Resumption tickets ---

bool Settings::save_resumption_ticket(const std::string& host, int port,
                                       const uint8_t* data, size_t len) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "INSERT OR REPLACE INTO resumption_tickets (host, port, ticket) "
                      "VALUES (?, ?, ?)";

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return false;

    sqlite3_bind_text(stmt, 1, host.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, port);
    sqlite3_bind_blob(stmt, 3, data, static_cast<int>(len), SQLITE_TRANSIENT);

    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

std::vector<uint8_t> Settings::load_resumption_ticket(const std::string& host, int port) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT ticket FROM resumption_tickets "
                      "WHERE host = ? AND port = ? "
                      "AND saved_at > datetime('now', '-24 hours')";

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return {};

    sqlite3_bind_text(stmt, 1, host.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, port);

    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return {};
    }

    auto* blob = static_cast<const uint8_t*>(sqlite3_column_blob(stmt, 0));
    int blob_len = sqlite3_column_bytes(stmt, 0);
    std::vector<uint8_t> result(blob, blob + blob_len);
    sqlite3_finalize(stmt);
    return result;
}

bool Settings::delete_resumption_ticket(const std::string& host, int port) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "DELETE FROM resumption_tickets WHERE host = ? AND port = ?";

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return false;

    sqlite3_bind_int(stmt, 2, port);
    sqlite3_bind_text(stmt, 1, host.c_str(), -1, SQLITE_TRANSIENT);

    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

// --- Preferences ---

bool Settings::set_pref(const std::string& key, const std::string& value) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "INSERT OR REPLACE INTO preferences (key, value) VALUES (?, ?)";

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return false;

    sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, value.c_str(), -1, SQLITE_TRANSIENT);

    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

std::optional<std::string> Settings::get_pref(const std::string& key) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT value FROM preferences WHERE key = ?";

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return std::nullopt;

    sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);

    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return std::nullopt;
    }

    std::string val = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    sqlite3_finalize(stmt);
    return val;
}

} // namespace parties::client
