// SIGKILL at every write, truncate and sync while Settings::open migrates a
// legacy database, which first commits a pre-migration snapshot through
// commit_file. Each kill runs in a forked child; the parent then looks at the
// files the way the next start would. A snapshot is either absent or a whole
// copy of the previous database, the live database is whole at a version a
// migration step committed, and the next start opens it.

#include "data_test_support.h"

#include <client/settings.h>
#include <client/settings_schema.h>
#include <client/sqlite_file.h>

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <string>

using namespace data_test;
using parties::client::Settings;
using parties::client::SettingsPaths;
namespace schema = parties::client::schema;

namespace {

constexpr int kRows = 200;
constexpr int kValueBytes = 1000;

// A VFS over the default one that counts every write, truncate and sync, and
// kills the process just before the one numbered g_kill_at.
sqlite3_vfs* g_real = nullptr;
long g_kill_at = 0;
long g_steps = 0;

void step() {
    if (++g_steps == g_kill_at) kill(getpid(), SIGKILL);
}

struct ShimFile {
    sqlite3_file base;
    sqlite3_file* real;
};

sqlite3_file* real(sqlite3_file* f) { return reinterpret_cast<ShimFile*>(f)->real; }
const sqlite3_io_methods& io(sqlite3_file* f) { return *real(f)->pMethods; }

int x_close(sqlite3_file* f) { return io(f).xClose(real(f)); }
int x_read(sqlite3_file* f, void* b, int n, sqlite3_int64 at) { return io(f).xRead(real(f), b, n, at); }
int x_write(sqlite3_file* f, const void* b, int n, sqlite3_int64 at) {
    step();
    return io(f).xWrite(real(f), b, n, at);
}
int x_truncate(sqlite3_file* f, sqlite3_int64 size) {
    step();
    return io(f).xTruncate(real(f), size);
}
int x_sync(sqlite3_file* f, int flags) {
    step();
    return io(f).xSync(real(f), flags);
}
int x_file_size(sqlite3_file* f, sqlite3_int64* size) { return io(f).xFileSize(real(f), size); }
int x_lock(sqlite3_file* f, int level) { return io(f).xLock(real(f), level); }
int x_unlock(sqlite3_file* f, int level) { return io(f).xUnlock(real(f), level); }
int x_check_reserved_lock(sqlite3_file* f, int* out) { return io(f).xCheckReservedLock(real(f), out); }
int x_file_control(sqlite3_file* f, int op, void* arg) { return io(f).xFileControl(real(f), op, arg); }
int x_sector_size(sqlite3_file* f) { return io(f).xSectorSize(real(f)); }
int x_device_characteristics(sqlite3_file* f) { return io(f).xDeviceCharacteristics(real(f)); }
int x_shm_map(sqlite3_file* f, int region, int size, int extend, void volatile** out) {
    return io(f).xShmMap(real(f), region, size, extend, out);
}
int x_shm_lock(sqlite3_file* f, int offset, int n, int flags) { return io(f).xShmLock(real(f), offset, n, flags); }
void x_shm_barrier(sqlite3_file* f) { io(f).xShmBarrier(real(f)); }
int x_shm_unmap(sqlite3_file* f, int remove) { return io(f).xShmUnmap(real(f), remove); }
int x_fetch(sqlite3_file* f, sqlite3_int64 at, int n, void** out) { return io(f).xFetch(real(f), at, n, out); }
int x_unfetch(sqlite3_file* f, sqlite3_int64 at, void* p) { return io(f).xUnfetch(real(f), at, p); }

const sqlite3_io_methods kMethods{
    3,          x_close,      x_read,         x_write,       x_truncate,          x_sync,
    x_file_size, x_lock,      x_unlock,       x_check_reserved_lock, x_file_control, x_sector_size,
    x_device_characteristics, x_shm_map,      x_shm_lock,    x_shm_barrier,       x_shm_unmap,
    x_fetch,    x_unfetch,
};

int x_open(sqlite3_vfs*, const char* name, sqlite3_file* f, int flags, int* out_flags) {
    auto* shim = reinterpret_cast<ShimFile*>(f);
    shim->real = reinterpret_cast<sqlite3_file*>(shim + 1);
    const int rc = g_real->xOpen(g_real, name, shim->real, flags, out_flags);
    // The unix VFS gives every file version 3 methods, which kMethods forwards.
    shim->base.pMethods = shim->real->pMethods ? &kMethods : nullptr;
    return rc;
}
int x_delete(sqlite3_vfs*, const char* name, int sync_dir) { return g_real->xDelete(g_real, name, sync_dir); }
int x_access(sqlite3_vfs*, const char* name, int flags, int* out) { return g_real->xAccess(g_real, name, flags, out); }
int x_full_pathname(sqlite3_vfs*, const char* name, int n, char* out) {
    return g_real->xFullPathname(g_real, name, n, out);
}

void install_counting_vfs() {
    static sqlite3_vfs vfs;
    g_real = sqlite3_vfs_find(nullptr);
    vfs = *g_real;
    vfs.zName = "parties-kill-at";
    vfs.szOsFile = int(sizeof(ShimFile)) + g_real->szOsFile;
    vfs.pNext = nullptr;
    vfs.xOpen = x_open;
    vfs.xDelete = x_delete;
    vfs.xAccess = x_access;
    vfs.xFullPathname = x_full_pathname;
    sqlite3_vfs_register(&vfs, 1);
}

std::string text(RawDb& db, const char* sql) {
    sqlite3_stmt* stmt = nullptr;
    std::string out = "<error>";
    if (sqlite3_prepare_v2(db.get(), sql, -1, &stmt, nullptr) == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW) {
        const auto* value = sqlite3_column_text(stmt, 0);
        out = value ? reinterpret_cast<const char*>(value) : "";
    }
    sqlite3_finalize(stmt);
    return out;
}

void write_legacy_db(const fs::path& db) {
    RawDb raw(db);
    raw.exec(legacy_v1_schema(true));
    raw.exec("INSERT INTO identity (id, seed_phrase, public_key, secret_key) VALUES "
             "(1, 'legacy seed words', " + key_blob(0x11) + ", " + key_blob(0x22) + ")");
    raw.exec("WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM n WHERE i < " + std::to_string(kRows) +
             ") INSERT INTO preferences (key, value) SELECT 'test.' || i, printf('%0" + std::to_string(kValueBytes) +
             "d', i) FROM n");
}

// Whole, with every row the legacy database had. Returns its data version, or
// -1 when it is not whole.
int whole_version(const fs::path& db) {
    RawDb raw(db);
    if (text(raw, "PRAGMA integrity_check") != "ok") return -1;
    if (raw.scalar("SELECT count(*) FROM identity WHERE seed_phrase = 'legacy seed words'") != 1) return -1;
    if (raw.scalar("SELECT count(*) FROM preferences WHERE key LIKE 'test.%'") != kRows) return -1;
    if (raw.scalar("SELECT sum(length(value)) FROM preferences WHERE key LIKE 'test.%'") != kRows * kValueBytes)
        return -1;
    if (raw.scalar("SELECT count(*) FROM sqlite_master WHERE name = 'parties_schema'") == 0)
        return schema::kLegacy.version;
    return int(raw.scalar("SELECT version FROM parties_schema WHERE id = 1"));
}

std::vector<fs::path> snapshots(const fs::path& backups) {
    std::vector<fs::path> out;
    for (const fs::path& file : files_in(backups))
        if (file.extension() == ".db") out.push_back(file);
    return out;
}

// Opens the database the way the app does, in a child killed at step `kill_at`.
// The exit status of a child that got past its last step.
int run_child(const SettingsPaths& paths, long kill_at, bool& killed) {
    std::fflush(nullptr);
    const pid_t pid = fork();
    if (pid == 0) {
        g_kill_at = kill_at;
        g_steps = 0;
        {
            Settings settings;
            if (settings.open(paths).status != Settings::OpenStatus::Ready) _exit(2);
        }
        step();
        _exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    killed = WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

bool killed_at_every_step_leaves_whole_files() {
    TempDir dir;
    const fs::path legacy = dir.path() / "legacy.db";
    write_legacy_db(legacy);
    bool ok = Check(whole_version(legacy) == schema::kLegacy.version, "the legacy fixture is not whole");

    int before_commit = 0, after_commit = 0;
    long kill_at = 1;
    for (;; ++kill_at) {
        const fs::path run = dir.path() / ("run-" + std::to_string(kill_at));
        fs::create_directories(run);
        const SettingsPaths paths{run / "parties.db", run / "backups"};
        fs::copy_file(legacy, paths.db);

        bool killed = false;
        const int exit_code = run_child(paths, kill_at, killed);
        if (!killed) {
            ok &= Check(exit_code == 0, "a child that was not killed did not open the database");
            break;
        }

        const auto copies = snapshots(paths.backups_dir);
        (copies.empty() ? before_commit : after_commit)++;
        for (const fs::path& copy : copies) {
            const bool whole = Check(whole_version(copy) == schema::kLegacy.version,
                                     "a snapshot is not a whole copy of the previous database");
            if (!whole) std::fprintf(stderr, "  killed at step %ld: %s\n", kill_at, copy.filename().c_str());
            ok &= whole;
        }
        const int live = whole_version(paths.db);
        const bool live_whole = Check(live >= schema::kLegacy.version && live <= schema::kCurrent.version,
                                      "the live database is not whole at a committed version");
        if (!live_whole) std::fprintf(stderr, "  killed at step %ld: live version %d\n", kill_at, live);
        ok &= live_whole;

        Settings next_start;
        ok &= Check(next_start.open(paths).status == Settings::OpenStatus::Ready, "the next start cannot open the database");
        next_start.close();
        ok &= Check(whole_version(paths.db) == schema::kCurrent.version, "the next start did not migrate to the current version");
        ok &= Check(!snapshots(paths.backups_dir).empty(), "no pre-migration snapshot after the next start");
        if (kill_at > 5000) {
            ok &= Check(false, "more than 5000 steps");
            break;
        }
    }
    std::printf("killed at %ld steps: %d before the snapshot was committed, %d after\n", kill_at - 1, before_commit,
                after_commit);
    ok &= Check(before_commit > 0 && after_commit > 0, "the kills did not land on both sides of the snapshot's rename");
    return ok;
}

bool commit_never_replaces_an_existing_file() {
    TempDir dir;
    const fs::path final_path = dir.path() / "live.db";
    const fs::path partial = parties::client::partial_path(final_path);
    { std::ofstream(final_path, std::ios::binary) << "previous"; }
    { std::ofstream(partial, std::ios::binary) << "new"; }
    std::string error;
    bool ok = Check(!parties::client::commit_file(partial, final_path, error), "commit replaced an existing file");
    ok &= Check(!error.empty(), "a refused commit gave no reason");
    ok &= Check(read_bytes(final_path) == std::vector<char>{'p', 'r', 'e', 'v', 'i', 'o', 'u', 's'},
                "a refused commit changed the existing file");
    ok &= Check(fs::exists(partial), "a refused commit lost the new file");

    fs::remove(final_path);
    error.clear();
    ok &= Check(parties::client::commit_file(partial, final_path, error), "commit to a free name failed");
    ok &= Check(read_bytes(final_path) == std::vector<char>{'n', 'e', 'w'}, "the committed file has other bytes");
    ok &= Check(!fs::exists(partial), "the partial file is still there after its commit");
    return ok;
}

} // namespace

int main() {
    install_counting_vfs();
    bool success = true;
    success &= commit_never_replaces_an_existing_file();
    success &= killed_at_every_step_leaves_whole_files();
    std::printf(success ? "data posix commit tests passed\n" : "data posix commit tests FAILED\n");
    return success ? 0 : 1;
}
