#include "data_test_support.h"

#include <client/legacy_import.h>
#include <client/settings.h>
#include <client/sqlite_file.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <optional>
#include <vector>

using namespace data_test;
using parties::PublicKey;
using parties::client::DataLocation;
using parties::client::import_legacy_data;
using parties::client::LegacyCandidate;
using parties::client::LegacyImportResult;
using parties::client::plan_legacy_import;
using parties::client::preserve_legacy_data;
using parties::client::Settings;
using Status = LegacyImportResult::Status;

namespace {

PublicKey key(unsigned char byte) {
    PublicKey k{};
    k.fill(byte);
    return k;
}

struct Layout {
    TempDir root;
    fs::path cwd = root.path() / "cwd";
    fs::path exe = root.path() / "exe";
    DataLocation location{root.path() / "Parties" / "data", root.path() / "Parties" / "backups"};
    Layout() {
        fs::create_directories(cwd);
        fs::create_directories(exe);
        fs::create_directories(location.data_dir);
    }
};

// A database the portable build left behind, with its last writes still in
// the -wal file, as after a crash or the old updater's ExitProcess.
void write_legacy_db(const fs::path& db, unsigned char identity_byte) {
    RawDb raw(db);
    sqlite3_db_config(raw.get(), SQLITE_DBCONFIG_NO_CKPT_ON_CLOSE, 1, nullptr);
    raw.exec("PRAGMA journal_mode=WAL;");
    raw.exec(legacy_v1_schema(true));
    raw.exec("INSERT INTO identity (id, seed_phrase, public_key, secret_key) VALUES (1, 'words', " +
             key_blob(identity_byte) + ", " + key_blob(0x77) + ")");
    raw.exec("INSERT INTO saved_servers (name, host, port) VALUES ('Home', 'example.org', 7800)");
}

std::vector<char> bytes_or_empty(const fs::path& p) {
    std::error_code ec;
    return fs::exists(p, ec) ? read_bytes(p) : std::vector<char>{};
}

std::optional<PublicKey> identity_in(const fs::path& db) {
    RawDb raw(db);
    if (raw.scalar("SELECT COUNT(*) FROM identity") != 1) return std::nullopt;
    sqlite3_stmt* stmt = nullptr;
    std::optional<PublicKey> out;
    sqlite3_prepare_v2(raw.get(), "SELECT public_key FROM identity", -1, &stmt, nullptr);
    if (sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_bytes(stmt, 0) == 32) {
        out.emplace();
        std::memcpy(out->data(), sqlite3_column_blob(stmt, 0), 32);
    }
    sqlite3_finalize(stmt);
    return out;
}

bool plan_prefers_identity_then_newest() {
    using namespace std::chrono_literals;
    const auto t0 = fs::file_time_type::clock::now();
    bool ok = true;

    {
        const LegacyCandidate c[] = {
            {"old-with-identity", true, key(1), t0},
            {"new-without-identity", true, std::nullopt, t0 + 1h},
        };
        const auto plan = plan_legacy_import(c);
        ok &= Check(plan.import == 0u, "a newer database without an identity beat one with an identity");
        ok &= Check(plan.preserve == std::vector<std::size_t>{1}, "the other candidate was not preserved");
    }
    {
        const LegacyCandidate c[] = {
            {"older", true, key(1), t0},
            {"newer", true, key(2), t0 + 1h},
        };
        const auto plan = plan_legacy_import(c);
        ok &= Check(plan.import == 1u, "the newest identity was not imported");
        ok &= Check(plan.preserve == std::vector<std::size_t>{0}, "the older identity was not preserved");
    }
    {
        const LegacyCandidate c[] = {
            {"corrupt-but-newest", false, std::nullopt, t0 + 2h},
            {"readable", true, std::nullopt, t0},
        };
        const auto plan = plan_legacy_import(c);
        ok &= Check(plan.import == 1u, "an unreadable database was chosen for import");
        ok &= Check(plan.preserve == std::vector<std::size_t>{0}, "the unreadable database was not preserved");
    }
    {
        const LegacyCandidate c[] = {{"corrupt", false, std::nullopt, t0}};
        const auto plan = plan_legacy_import(c);
        ok &= Check(!plan.import && plan.preserve == std::vector<std::size_t>{0},
                    "an unreadable lone database was not kept as a backup");
    }
    ok &= Check(!plan_legacy_import({}).import, "an empty search imported something");
    return ok;
}

bool imports_copy_first_and_only_once() {
    Layout layout;
    const fs::path source = layout.cwd / "parties_client.db";
    write_legacy_db(source, 0x42);
    const auto main_before = read_bytes(source);
    const auto wal_before = bytes_or_empty(fs::path(source) += "-wal");

    const fs::path dirs[] = {layout.cwd, layout.exe};
    const auto result = import_legacy_data(layout.location, dirs);
    bool ok = Check(result.status == Status::Imported, "legacy database was not imported");
    ok &= Check(result.imported_from == source, "imported from the wrong file");
    ok &= Check(result.preserved.empty(), "a single candidate produced a backup");
    ok &= Check(!wal_before.empty(), "fixture did not leave its writes in the -wal file");

    ok &= Check(read_bytes(source) == main_before, "the source database was modified");
    ok &= Check(bytes_or_empty(fs::path(source) += "-wal") == wal_before, "the source -wal file was modified");

    {
        Settings settings;
        const auto opened = settings.open(layout.location.settings_paths());
        ok &= Check(opened.status == Settings::OpenStatus::Ready, "imported database does not open");
        const auto identity = settings.load_identity();
        ok &= Check(identity && identity->public_key == key(0x42), "imported identity does not match the source");
        ok &= Check(settings.get_saved_servers().size() == 1, "rows from the source -wal were lost");
    }

    const auto data_before = read_bytes(layout.location.db_path());
    const auto backups_before = files_in(layout.location.backups_dir);
    const auto again = import_legacy_data(layout.location, dirs);
    ok &= Check(again.status == Status::Skipped, "a second run did not skip");
    ok &= Check(read_bytes(layout.location.db_path()) == data_before, "a second run changed the database");
    ok &= Check(files_in(layout.location.backups_dir) == backups_before, "a second run wrote backups");
    return ok;
}

bool two_candidates_import_one_and_preserve_the_other() {
    using namespace std::chrono_literals;
    Layout layout;
    const fs::path older = layout.cwd / "parties_client.db";
    const fs::path newer = layout.exe / "parties_client.db";
    write_legacy_db(older, 0xA1);
    write_legacy_db(newer, 0xB2);
    const auto now = fs::file_time_type::clock::now();
    for (const char* suffix : {"", "-wal"}) {
        fs::last_write_time(fs::path(older) += suffix, now - 2h);
        fs::last_write_time(fs::path(newer) += suffix, now - 1h);
    }

    const fs::path dirs[] = {layout.cwd, layout.exe};
    const auto result = import_legacy_data(layout.location, dirs);
    bool ok = Check(result.status == Status::Imported, "two candidates did not import");
    ok &= Check(result.imported_from == newer, "did not import the newest candidate");
    ok &= Check(identity_in(layout.location.db_path()) == key(0xB2), "live database has the wrong identity");

    const auto backups = files_in(layout.location.backups_dir);
    ok &= Check(backups.size() == 1 && result.preserved == backups, "expected exactly one preserved backup");
    if (backups.size() == 1) {
        ok &= Check(utf8(backups[0].filename()).starts_with("legacy-"), "backup is not named legacy-*");
        ok &= Check(identity_in(backups[0]) == key(0xA1), "preserved backup lost the other identity");
    }
    return ok;
}

bool same_folder_twice_is_one_candidate() {
    Layout layout;
    write_legacy_db(layout.cwd / "parties_client.db", 0x10);
    const fs::path dirs[] = {layout.cwd, layout.cwd / "." };
    const auto result = import_legacy_data(layout.location, dirs);
    bool ok = Check(result.status == Status::Imported, "duplicate folder did not import");
    ok &= Check(result.preserved.empty(), "the same file was preserved as a second candidate");
    return ok;
}

bool leftover_partial_copy_is_cleaned_up() {
    Layout layout;
    write_legacy_db(layout.exe / "parties_client.db", 0x5C);
    const fs::path partial = parties::client::partial_path(layout.location.db_path());
    {
        std::ofstream out(partial, std::ios::binary);
        out << "half of a database copy from a run that crashed";
    }
    const fs::path dirs[] = {layout.cwd, layout.exe};
    const auto result = import_legacy_data(layout.location, dirs);
    bool ok = Check(result.status == Status::Imported, "import after a crashed run failed");
    ok &= Check(!fs::exists(partial), "the crashed run's partial copy was left behind");
    ok &= Check(identity_in(layout.location.db_path()) == key(0x5C), "import after a crashed run has the wrong identity");
    return ok;
}

bool nothing_found_creates_nothing() {
    Layout layout;
    const fs::path dirs[] = {layout.cwd, layout.exe};
    const auto result = import_legacy_data(layout.location, dirs);
    bool ok = Check(result.status == Status::NoneImported, "an empty search did not report NoneImported");
    ok &= Check(!fs::exists(layout.location.db_path()), "an empty search created a database");
    return ok;
}

bool preserving_with_a_live_database_copies_each_source_once() {
    Layout layout;
    write_legacy_db(layout.location.db_path(), 0x01);
    const auto live_before = read_bytes(layout.location.db_path());
    const fs::path source = layout.cwd / "parties_client.db";
    write_legacy_db(source, 0x02);
    {
        std::ofstream out(layout.exe / "parties_client.db", std::ios::binary);
        out << "not a database";
    }
    const auto source_before = read_bytes(source);
    const auto wal_before = bytes_or_empty(fs::path(source) += "-wal");

    const fs::path dirs[] = {layout.cwd, layout.exe};
    const auto first = preserve_legacy_data(layout.location, dirs);
    bool ok = Check(first.status == Status::Skipped, "preserving failed");
    ok &= Check(first.preserved.size() == 2, "expected one backup per source");
    ok &= Check(files_in(layout.location.backups_dir).size() == 2, "expected two files in backups");
    ok &= Check(read_bytes(layout.location.db_path()) == live_before, "the live database was changed");
    ok &= Check(read_bytes(source) == source_before, "the source database was modified");
    ok &= Check(bytes_or_empty(fs::path(source) += "-wal") == wal_before, "the source -wal file was modified");
    bool copied_identity = false;
    for (const fs::path& kept : first.preserved) copied_identity |= identity_in(kept) == key(0x02);
    ok &= Check(copied_identity, "no backup holds the legacy identity");

    const auto again = preserve_legacy_data(layout.location, dirs);
    ok &= Check(again.status == Status::Skipped && again.preserved.empty(), "a second run copied again");
    ok &= Check(files_in(layout.location.backups_dir).size() == 2, "a second run added files to backups");
    return ok;
}

bool preserving_after_an_import_adds_only_the_imported_source() {
    using namespace std::chrono_literals;
    Layout layout;
    write_legacy_db(layout.cwd / "parties_client.db", 0xA1);
    write_legacy_db(layout.exe / "parties_client.db", 0xB2);
    const auto now = fs::file_time_type::clock::now();
    for (const char* suffix : {"", "-wal"}) {
        fs::last_write_time(layout.cwd / (std::string("parties_client.db") + suffix), now - 2h);
        fs::last_write_time(layout.exe / (std::string("parties_client.db") + suffix), now - 1h);
    }
    const fs::path dirs[] = {layout.cwd, layout.exe};
    const auto imported = import_legacy_data(layout.location, dirs);
    bool ok = Check(imported.status == Status::Imported && imported.preserved.size() == 1, "import failed");
    const auto kept = preserve_legacy_data(layout.location, dirs);
    ok &= Check(kept.preserved.size() == 1, "expected only the imported source to be copied");
    ok &= Check(kept.preserved.size() == 1 && identity_in(kept.preserved[0]) == key(0xB2),
                "the new backup is not the imported source");
    ok &= Check(files_in(layout.location.backups_dir).size() == 2, "expected one backup per source");
    return ok;
}

} // namespace

int main() {
    bool success = true;
    success &= plan_prefers_identity_then_newest();
    success &= imports_copy_first_and_only_once();
    success &= two_candidates_import_one_and_preserve_the_other();
    success &= same_folder_twice_is_one_candidate();
    success &= leftover_partial_copy_is_cleaned_up();
    success &= nothing_found_creates_nothing();
    success &= preserving_with_a_live_database_copies_each_source_once();
    success &= preserving_after_an_import_adds_only_the_imported_source();
    std::printf(success ? "legacy import tests passed\n" : "legacy import tests FAILED\n");
    return success ? 0 : 1;
}
