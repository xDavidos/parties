#pragma once

#include <parties/update/data_compat.h>

#include <cstdint>
#include <span>

namespace parties::client::schema {

// What a database says about itself, stored in the single-row parties_schema
// table. compat_floor is the oldest build schema that can still read and write
// the database; it only rises for a breaking change.
struct Version {
    int version = 0;
    int compat_floor = 0;
    friend constexpr bool operator==(const Version&, const Version&) = default;
};

// A database with tables but no parties_schema predates versioning.
inline constexpr Version kLegacy{1, 1};

// SQLite has no ADD COLUMN IF NOT EXISTS, and v1 databases exist both with
// and without saved_servers.password.
struct ColumnAddition {
    const char* table;
    const char* column;
    const char* definition;
};

struct Migration {
    int to_version;
    int raises_floor_to;   // 0 keeps the current floor
    const char* sql;
    std::span<const ColumnAddition> add_columns = {};
};

inline constexpr ColumnAddition kSavedServersPassword[] = {
    {"saved_servers", "password", "TEXT NOT NULL DEFAULT ''"},
};

// Every table of data version 1, each created only if missing.
inline constexpr const char* kVersion1Tables = R"SQL(
        CREATE TABLE IF NOT EXISTS identity (
            id          INTEGER PRIMARY KEY CHECK (id = 1),
            seed_phrase TEXT NOT NULL,
            public_key  BLOB NOT NULL,
            secret_key  BLOB NOT NULL
        );

        CREATE TABLE IF NOT EXISTS tofu_certs (
            host        TEXT NOT NULL,
            port        INTEGER NOT NULL,
            fingerprint TEXT NOT NULL,
            first_seen  TEXT NOT NULL DEFAULT (datetime('now')),
            PRIMARY KEY (host, port)
        );

        CREATE TABLE IF NOT EXISTS saved_servers (
            id            INTEGER PRIMARY KEY AUTOINCREMENT,
            name          TEXT NOT NULL,
            host          TEXT NOT NULL,
            port          INTEGER NOT NULL DEFAULT 7800,
            fingerprint   TEXT NOT NULL DEFAULT '',
            last_username TEXT NOT NULL DEFAULT '',
            password      TEXT NOT NULL DEFAULT '',
            UNIQUE(host, port)
        );

        CREATE TABLE IF NOT EXISTS preferences (
            key   TEXT PRIMARY KEY,
            value TEXT NOT NULL
        );

        CREATE TABLE IF NOT EXISTS resumption_tickets (
            host     TEXT NOT NULL,
            port     INTEGER NOT NULL,
            ticket   BLOB NOT NULL,
            saved_at TEXT NOT NULL DEFAULT (datetime('now')),
            PRIMARY KEY (host, port)
        );
    )SQL";

// Ordered and forward-only. Each step runs in one transaction together with
// the parties_schema update. Never edit a released step; append a new one.
inline constexpr Migration kMigrations[] = {
    {1, 1, kVersion1Tables},
    {2, 0, R"SQL(
        CREATE TABLE identity_archive (
            id          INTEGER PRIMARY KEY AUTOINCREMENT,
            seed_phrase TEXT NOT NULL,
            public_key  BLOB NOT NULL,
            secret_key  BLOB NOT NULL,
            archived_at TEXT NOT NULL DEFAULT (datetime('now')),
            reason      TEXT NOT NULL
        );
    )SQL", kSavedServersPassword},
    // Builds before the identity table wrote only tofu_certs, saved_servers
    // and preferences. Such a database counts as legacy version 1, so step 1
    // never ran on it.
    {3, 0, kVersion1Tables},
};

// The version a database reaches after every step in `migrations` ran.
constexpr Version target_of(std::span<const Migration> migrations) {
    Version v;
    for (const Migration& m : migrations) {
        v.version = m.to_version;
        if (m.raises_floor_to != 0) v.compat_floor = m.raises_floor_to;
    }
    return v;
}

inline constexpr Version kCurrent = target_of(kMigrations);

constexpr update::Compatibility compatibility(Version db, int build_schema) {
    return update::compatibility(static_cast<uint32_t>(build_schema),
                                 {static_cast<uint32_t>(db.version),
                                  static_cast<uint32_t>(db.compat_floor)});
}

} // namespace parties::client::schema
