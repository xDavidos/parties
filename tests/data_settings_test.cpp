#include "data_test_support.h"

#include <client/settings.h>
#include <client/settings_schema.h>

#include <algorithm>
#include <cstdio>
#include <span>
#include <vector>

namespace parties::client {
struct SettingsTestSeam {
    static Settings::OpenResult open(Settings& settings, const SettingsPaths& paths,
                                     std::span<const schema::Migration> migrations) {
        return settings.open_with(paths, migrations);
    }
};
} // namespace parties::client

using namespace data_test;
using parties::PublicKey;
using parties::SecretKey;
using parties::client::Settings;
using parties::client::SettingsPaths;
using parties::client::SettingsTestSeam;
namespace schema = parties::client::schema;

namespace {

PublicKey key(unsigned char byte) {
    PublicKey k{};
    k.fill(byte);
    return k;
}

SettingsPaths paths_in(const TempDir& dir) {
    return {dir.path() / "parties_client.db", dir.path() / "backups"};
}

bool ready(const Settings::OpenResult& result) {
    if (result.status != Settings::OpenStatus::Ready)
        std::fprintf(stderr, "open was not Ready: %s\n", result.reason.c_str());
    return result.status == Settings::OpenStatus::Ready;
}

schema::Version stored_version(const fs::path& db) {
    RawDb raw(db);
    return {static_cast<int>(raw.scalar("SELECT version FROM parties_schema WHERE id = 1")),
            static_cast<int>(raw.scalar("SELECT compat_floor FROM parties_schema WHERE id = 1"))};
}

void write_legacy_db(const fs::path& db, bool with_password) {
    RawDb raw(db);
    raw.exec("PRAGMA journal_mode=WAL;");
    raw.exec(legacy_v1_schema(with_password));
    raw.exec("INSERT INTO identity (id, seed_phrase, public_key, secret_key) VALUES "
             "(1, 'legacy seed words', " + key_blob(0x11) + ", " + key_blob(0x22) + ")");
    raw.exec(with_password
        ? "INSERT INTO saved_servers (name, host, port, fingerprint, last_username, password) "
          "VALUES ('Home', 'example.org', 7800, 'AA:BB', 'alice', 'hunter2')"
        : "INSERT INTO saved_servers (name, host, port, fingerprint, last_username) "
          "VALUES ('Home', 'example.org', 7800, 'AA:BB', 'alice')");
    raw.exec("INSERT INTO tofu_certs (host, port, fingerprint) VALUES ('example.org', 7800, 'AA:BB')");
    raw.exec("INSERT INTO preferences (key, value) VALUES ('audio.volume', '0.5')");
}

bool migration_table_is_contiguous_and_floor_monotonic() {
    bool ok = true;
    int floor = 0;
    for (std::size_t i = 0; i < std::size(schema::kMigrations); ++i) {
        const schema::Migration& m = schema::kMigrations[i];
        ok &= Check(m.to_version == static_cast<int>(i) + 1, "migration versions are not contiguous from 1");
        if (m.raises_floor_to != 0) {
            ok &= Check(m.raises_floor_to >= floor, "a migration lowers the compat floor");
            ok &= Check(m.raises_floor_to <= m.to_version, "a migration raises the floor above its version");
            floor = m.raises_floor_to;
        }
    }
    ok &= Check(schema::kCurrent == schema::Version{2, 1}, "this build is not data version 2, floor 1");
    return ok;
}

bool legacy_v1_migrates_and_keeps_every_row(bool with_password) {
    TempDir dir;
    const SettingsPaths paths = paths_in(dir);
    write_legacy_db(paths.db, with_password);

    bool ok = true;
    {
        Settings settings;
        ok &= Check(ready(settings.open(paths)), "legacy database did not open");

        const auto identity = settings.load_identity();
        ok &= Check(identity && identity->seed_phrase == "legacy seed words" &&
                    identity->public_key == key(0x11) && identity->secret_key == key(0x22),
                    "identity changed during migration");

        const auto servers = settings.get_saved_servers();
        ok &= Check(servers.size() == 1 && servers[0].host == "example.org" &&
                    servers[0].port == 7800 && servers[0].last_username == "alice" &&
                    servers[0].fingerprint == "AA:BB",
                    "saved server changed during migration");
        ok &= Check(!servers.empty() && servers[0].password == (with_password ? "hunter2" : ""),
                    "saved server password changed during migration");
        ok &= Check(settings.check_fingerprint("example.org", 7800, "AA:BB") ==
                    Settings::TofuResult::Trusted, "TOFU pin lost during migration");
        ok &= Check(settings.get_pref("audio.volume") == "0.5", "preference lost during migration");

        ok &= Check(settings.save_server("Work", "work.example", 7801, "CC", "bob", "pw2"),
                    "migrated database cannot store a server password");
        bool stored = false;
        for (const auto& s : settings.get_saved_servers())
            stored = stored || (s.host == "work.example" && s.password == "pw2");
        ok &= Check(stored, "server password did not round-trip after migration");
    }

    ok &= Check(stored_version(paths.db) == schema::Version{2, 1}, "database not recorded as version 2, floor 1");

    const auto snapshots = files_in(paths.backups_dir);
    ok &= Check(snapshots.size() == 1, "expected exactly one pre-migration snapshot");
    if (snapshots.size() == 1) {
        const std::string name = utf8(snapshots[0].filename());
        ok &= Check(name.starts_with("pre-migration-v1-") && name.ends_with(".db"),
                    "snapshot is not named pre-migration-v1-<utc>.db");
        RawDb snapshot(snapshots[0]);
        ok &= Check(snapshot.scalar("SELECT COUNT(*) FROM identity WHERE public_key = " +
                                    key_blob(0x11)) == 1, "snapshot does not contain the identity");
        ok &= Check(snapshot.scalar("SELECT COUNT(*) FROM saved_servers") == 1,
                    "snapshot does not contain the saved server");
    }
    return ok;
}

// Builds before the identity table existed wrote only these three tables, so
// such a database also counts as legacy v1 and skips migration 1.
bool pre_identity_database_gets_every_table() {
    TempDir dir;
    const SettingsPaths paths = paths_in(dir);
    {
        RawDb raw(paths.db);
        raw.exec(R"SQL(
            CREATE TABLE tofu_certs (host TEXT NOT NULL, port INTEGER NOT NULL, fingerprint TEXT NOT NULL,
                first_seen TEXT NOT NULL DEFAULT (datetime('now')), PRIMARY KEY (host, port));
            CREATE TABLE saved_servers (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT NOT NULL,
                host TEXT NOT NULL, port INTEGER NOT NULL DEFAULT 7800, fingerprint TEXT NOT NULL DEFAULT '',
                last_username TEXT NOT NULL DEFAULT '', last_password TEXT NOT NULL DEFAULT '', UNIQUE(host, port));
            CREATE TABLE preferences (key TEXT PRIMARY KEY, value TEXT NOT NULL);
            INSERT INTO saved_servers (name, host, port) VALUES ('Home', 'example.org', 7800);
        )SQL");
    }
    bool ok = true;
    {
        Settings settings;
        ok &= Check(ready(settings.open(paths)), "a pre-identity database did not open");
        ok &= Check(settings.save_identity("seed words", key(0x22), key(0x11)), "a pre-identity database cannot save an identity");
        const auto identity = settings.load_identity();
        ok &= Check(identity && identity->public_key == key(0x11), "the saved identity does not load");
        ok &= Check(settings.get_saved_servers().size() == 1, "the saved server was lost");
    }
    RawDb raw(paths.db);
    ok &= Check(raw.scalar("SELECT count(*) FROM sqlite_master WHERE name = 'resumption_tickets'") == 1,
                "the resumption_tickets table is missing");
    return ok;
}

bool reopening_current_database_writes_nothing_new() {
    TempDir dir;
    const SettingsPaths paths = paths_in(dir);
    write_legacy_db(paths.db, true);
    bool ok = true;
    {
        Settings settings;
        ok &= Check(ready(settings.open(paths)), "first open failed");
    }
    const auto after_first = files_in(paths.backups_dir);
    {
        Settings settings;
        ok &= Check(ready(settings.open(paths)), "second open failed");
    }
    ok &= Check(files_in(paths.backups_dir) == after_first, "reopening a v2 database wrote a snapshot");
    ok &= Check(stored_version(paths.db) == schema::Version{2, 1}, "reopening changed the version");

    TempDir fresh;
    {
        Settings settings;
        ok &= Check(ready(settings.open(paths_in(fresh))), "fresh database did not open");
        ok &= Check(!settings.has_identity(), "fresh database has an identity");
    }
    ok &= Check(files_in(paths_in(fresh).backups_dir).empty(), "creating a fresh database wrote a snapshot");
    ok &= Check(stored_version(paths_in(fresh).db) == schema::Version{2, 1}, "fresh database not at version 2");
    return ok;
}

bool newer_compatible_database_opens_as_is() {
    TempDir dir;
    const SettingsPaths paths = paths_in(dir);
    {
        Settings settings;
        settings.open(paths);
    }
    {
        RawDb raw(paths.db);
        raw.exec("UPDATE parties_schema SET version = 3, compat_floor = 1");
    }
    bool ok = true;
    {
        Settings settings;
        ok &= Check(ready(settings.open(paths)), "version 3 floor 1 database did not open");
        ok &= Check(settings.set_pref("ui.theme", "dark") && settings.get_pref("ui.theme") == "dark",
                    "version 3 floor 1 database is not writable");
    }
    ok &= Check(stored_version(paths.db) == schema::Version{3, 1}, "opening a newer database changed its version");
    ok &= Check(files_in(paths.backups_dir).empty(), "opening a newer database wrote a snapshot");
    return ok;
}

bool too_new_database_is_refused_untouched() {
    TempDir dir;
    const SettingsPaths paths = paths_in(dir);
    {
        Settings settings;
        settings.open(paths);
        settings.set_pref("audio.volume", "0.7");
    }
    {
        RawDb raw(paths.db);
        raw.exec("UPDATE parties_schema SET version = 3, compat_floor = 3");
    }
    const auto before = read_bytes(paths.db);
    bool ok = true;
    {
        Settings settings;
        const auto result = settings.open(paths);
        ok &= Check(result.status == Settings::OpenStatus::TooNew, "version 3 floor 3 database was not TooNew");
        ok &= Check(!result.reason.empty(), "TooNew carries no reason");
        ok &= Check(!settings.is_open(), "TooNew left the database open");
    }
    ok &= Check(read_bytes(paths.db) == before, "TooNew database bytes changed");
    ok &= Check(files_in(paths.backups_dir).empty(), "TooNew database got a snapshot");
    return ok;
}

bool unreadable_file_is_unavailable_and_untouched() {
    TempDir dir;
    const SettingsPaths paths = paths_in(dir);
    {
        std::ofstream out(paths.db, std::ios::binary);
        out << "this is not a database, but somebody's file all the same";
    }
    const auto before = read_bytes(paths.db);
    Settings settings;
    const auto result = settings.open(paths);
    bool ok = Check(result.status == Settings::OpenStatus::Unavailable, "garbage file was not Unavailable");
    ok &= Check(read_bytes(paths.db) == before, "garbage file was modified");
    return ok;
}

bool failed_migration_leaves_database_at_old_version() {
    TempDir dir;
    const SettingsPaths paths = paths_in(dir);
    {
        Settings settings;
        settings.open(paths);
        settings.save_identity("seed", key(0x33), key(0x44));
        settings.save_server("Home", "example.org", 7800, "AA", "alice", "pw");
    }

    std::vector<schema::Migration> broken(std::begin(schema::kMigrations), std::end(schema::kMigrations));
    broken.push_back({3, 0, "CREATE TABLE half_done (x INTEGER); INSERT INTO no_such_table VALUES (1);"});

    bool ok = true;
    {
        Settings settings;
        const auto result = SettingsTestSeam::open(settings, paths, broken);
        ok &= Check(result.status == Settings::OpenStatus::Unavailable, "failing migration did not report Unavailable");
    }
    ok &= Check(stored_version(paths.db) == schema::Version{2, 1}, "failing migration changed the version");
    {
        RawDb raw(paths.db);
        ok &= Check(raw.scalar("SELECT COUNT(*) FROM sqlite_master WHERE name = 'half_done'") == 0,
                    "failing migration left a partial change");
    }
    {
        Settings settings;
        ok &= Check(ready(settings.open(paths)), "database unusable after a failed migration");
        const auto identity = settings.load_identity();
        ok &= Check(identity && identity->public_key == key(0x44), "identity lost after a failed migration");
        const auto servers = settings.get_saved_servers();
        ok &= Check(servers.size() == 1 && servers[0].password == "pw", "server lost after a failed migration");
    }
    ok &= Check(files_in(paths.backups_dir).size() == 1, "failed migration did not snapshot first");
    return ok;
}

bool identity_history_is_append_only() {
    TempDir dir;
    const SettingsPaths paths = paths_in(dir);
    Settings settings;
    bool ok = Check(ready(settings.open(paths)), "open failed");

    ok &= Check(settings.save_identity("first words", key(0x01), key(0x02)), "saving the first identity failed");
    ok &= Check(settings.archived_identities().empty(), "saving into an empty slot archived something");

    ok &= Check(settings.save_identity("first words", key(0x01), key(0x02)), "re-saving the same identity failed");
    ok &= Check(settings.archived_identities().empty(), "re-saving the same identity archived it");

    ok &= Check(settings.save_identity("second words", key(0x03), key(0x04)), "restoring another identity failed");
    auto archive = settings.archived_identities();
    ok &= Check(archive.size() == 1 && archive[0].reason == "replaced" &&
                archive[0].identity.seed_phrase == "first words" &&
                archive[0].identity.secret_key == key(0x01) &&
                archive[0].identity.public_key == key(0x02) && !archive[0].archived_at.empty(),
                "restoring another identity did not archive the previous one");
    const auto current = settings.load_identity();
    ok &= Check(current && current->public_key == key(0x04), "restored identity is not current");

    ok &= Check(settings.delete_identity(), "delete_identity failed");
    archive = settings.archived_identities();
    ok &= Check(!settings.load_identity(), "deleted identity is still current");
    ok &= Check(archive.size() == 2 && archive[1].reason == "deleted" &&
                archive[1].identity.public_key == key(0x04),
                "delete_identity did not archive the identity");
    return ok;
}

// The narrowest real seam for "a raw-key identity survives a restart":
// AppCore::load_or_generate_identity needs the whole UI stack, and with the
// delete removed it only reads back what Settings returns here.
bool raw_key_identity_survives_restart() {
    TempDir dir;
    const SettingsPaths paths = paths_in(dir);
    bool ok = true;
    {
        Settings settings;
        settings.open(paths);
        ok &= Check(settings.save_identity("", key(0x55), key(0x66)), "saving a raw-key identity failed");
    }
    {
        Settings settings;
        ok &= Check(ready(settings.open(paths)), "reopen failed");
        const auto identity = settings.load_identity();
        ok &= Check(identity && identity->seed_phrase.empty() && identity->secret_key == key(0x55) &&
                    identity->public_key == key(0x66), "raw-key identity did not survive a restart");
        ok &= Check(settings.archived_identities().empty(), "raw-key identity was archived on restart");
    }
    return ok;
}

} // namespace

int main() {
    bool success = true;
    success &= migration_table_is_contiguous_and_floor_monotonic();
    success &= legacy_v1_migrates_and_keeps_every_row(true);
    success &= legacy_v1_migrates_and_keeps_every_row(false);
    success &= pre_identity_database_gets_every_table();
    success &= reopening_current_database_writes_nothing_new();
    success &= newer_compatible_database_opens_as_is();
    success &= too_new_database_is_refused_untouched();
    success &= unreadable_file_is_unavailable_and_untouched();
    success &= failed_migration_leaves_database_at_old_version();
    success &= identity_history_is_append_only();
    success &= raw_key_identity_survives_restart();
    std::printf(success ? "data settings tests passed\n" : "data settings tests FAILED\n");
    return success ? 0 : 1;
}
