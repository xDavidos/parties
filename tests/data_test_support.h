#pragma once

#include <sqlite3.h>

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <windows.h>

namespace data_test {

namespace fs = std::filesystem;

inline bool Check(bool condition, const char* message) {
    if (!condition) std::fprintf(stderr, "FAILED: %s\n", message);
    return condition;
}

// A fresh directory per test case, removed when the case ends.
class TempDir {
public:
    TempDir() {
        static std::atomic<int> counter{0};
        path_ = fs::temp_directory_path() /
                ("parties-data-test-" + std::to_string(GetCurrentProcessId()) + "-" +
                 std::to_string(counter++));
        fs::remove_all(path_);
        fs::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

inline std::string utf8(const fs::path& p) {
    const auto u8 = p.u8string();
    return {reinterpret_cast<const char*>(u8.data()), u8.size()};
}

// Raw SQLite access, standing in for builds that predate versioning and for
// the launcher, which reads parties_schema directly.
class RawDb {
public:
    explicit RawDb(const fs::path& path) { sqlite3_open(utf8(path).c_str(), &db_); }
    ~RawDb() { sqlite3_close(db_); }
    RawDb(const RawDb&) = delete;
    RawDb& operator=(const RawDb&) = delete;

    bool exec(const std::string& sql) {
        char* err = nullptr;
        const bool ok = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err) == SQLITE_OK;
        if (!ok) std::fprintf(stderr, "SQL error: %s\n", err ? err : "?");
        sqlite3_free(err);
        return ok;
    }
    // First column of the first row, or -1.
    long long scalar(const std::string& sql) {
        sqlite3_stmt* stmt = nullptr;
        long long value = -1;
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK &&
            sqlite3_step(stmt) == SQLITE_ROW)
            value = sqlite3_column_int64(stmt, 0);
        sqlite3_finalize(stmt);
        return value;
    }
    sqlite3* get() { return db_; }

private:
    sqlite3* db_ = nullptr;
};

inline std::vector<char> read_bytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

inline std::vector<fs::path> files_in(const fs::path& dir) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir, ec))
        if (entry.is_regular_file()) out.push_back(entry.path());
    return out;
}

// The schema every build before data versioning created, optionally from
// before saved_servers.password existed.
inline std::string legacy_v1_schema(bool with_password) {
    return std::string(R"SQL(
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
    )SQL") + (with_password ? "        password      TEXT NOT NULL DEFAULT '',\n" : "") + R"SQL(
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
}

// SQL literal for a 32-byte blob whose bytes all equal `byte`.
inline std::string key_blob(unsigned char byte) {
    static const char* hex = "0123456789ABCDEF";
    std::string out = "X'";
    for (int i = 0; i < 32; ++i) {
        out += hex[byte >> 4];
        out += hex[byte & 0xF];
    }
    return out + "'";
}

} // namespace data_test
