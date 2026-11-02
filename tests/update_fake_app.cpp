// Stands in for parties_client.exe under the launcher in the e2e harness. It
// speaks the link protocol, opens the database the way Settings does
// (parties_schema, forward-only migration, refusal of incompatible data), and
// records each run as a row in `markers`.
//
// Its version and data schema come from fake-build.json next to the exe, so
// one binary plays every build. Faults are chosen per version by
// PARTIES_FAKE_FAULT=<version>:<fault>[;<version>:<fault>...]. Every status
// the launcher sends is appended to PARTIES_FAKE_STATUS_LOG when it is set.

#define NOMINMAX

#include <parties/update/data_compat.h>
#include <parties/update/link.h>

#include <rapidjson/document.h>
#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <thread>

#include <windows.h>
#include <shellapi.h>

namespace fs = std::filesystem;
using namespace parties::update;

namespace {

constexpr UINT kCrash = 0xC0000005;

struct BuildInfo {
    std::string version;
    uint32_t schema = 1;
    uint32_t floor = 1;
};

std::string env(const char* name) {
    char* value = nullptr;
    size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || !value) return {};
    std::string out(value);
    std::free(value);
    return out;
}

BuildInfo read_build_info(const fs::path& exe_dir) {
    std::ifstream in(exe_dir / "fake-build.json");
    std::stringstream text;
    text << in.rdbuf();
    rapidjson::Document doc;
    doc.Parse(text.str().c_str());
    if (doc.HasParseError() || !doc.IsObject()) ExitProcess(2);
    return {doc["version"].GetString(), doc["data_schema"].GetUint(), doc["data_compat_floor"].GetUint()};
}

std::string fault_for(const std::string& version) {
    std::string spec = env("PARTIES_FAKE_FAULT");
    std::stringstream items(spec);
    std::string item;
    while (std::getline(items, item, ';')) {
        size_t colon = item.find(':');
        if (colon != std::string::npos && item.substr(0, colon) == version) return item.substr(colon + 1);
    }
    return {};
}

[[noreturn]] void die(UINT code) {
    TerminateProcess(GetCurrentProcess(), code);
    ExitProcess(code);
}

class Link {
public:
    explicit Link(const std::wstring& name) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline) {
            pipe_ = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (pipe_ != INVALID_HANDLE_VALUE) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    bool ok() const { return pipe_ != INVALID_HANDLE_VALUE; }

    void send(const link::AppMessage& m) {
        std::string line = link::write(m);
        DWORD written = 0;
        WriteFile(pipe_, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    }

    // Never blocks: reads only what is already in the pipe.
    std::optional<link::LauncherMessage> poll() {
        for (;;) {
            if (auto line = reader_.next()) {
                if (auto m = link::parse_launcher_message(*line)) return m;
                continue;
            }
            DWORD available = 0;
            if (!PeekNamedPipe(pipe_, nullptr, 0, nullptr, &available, nullptr) || available == 0) return std::nullopt;
            char buffer[4096];
            DWORD read = 0;
            if (!ReadFile(pipe_, buffer, std::min<DWORD>(available, sizeof buffer), &read, nullptr)) return std::nullopt;
            reader_.append({buffer, read});
        }
    }

private:
    HANDLE pipe_ = INVALID_HANDLE_VALUE;
    link::LineReader reader_;
};

bool exec(sqlite3* db, const std::string& sql) {
    return sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK;
}

std::optional<DataInfo> read_schema(sqlite3* db) {
    sqlite3_stmt* stmt = nullptr;
    std::optional<DataInfo> info;
    if (sqlite3_prepare_v2(db, "SELECT version, compat_floor FROM parties_schema WHERE id = 1", -1, &stmt, nullptr) ==
            SQLITE_OK &&
        sqlite3_step(stmt) == SQLITE_ROW)
        info = DataInfo{static_cast<uint32_t>(sqlite3_column_int(stmt, 0)),
                        static_cast<uint32_t>(sqlite3_column_int(stmt, 1))};
    sqlite3_finalize(stmt);
    return info;
}

// Returns the schema the database had when opened.
uint32_t open_data(const fs::path& db_path, const BuildInfo& build, sqlite3*& db) {
    bool fresh = !fs::exists(db_path);
    const auto u8 = db_path.u8string();
    if (sqlite3_open(std::string(u8.begin(), u8.end()).c_str(), &db) != SQLITE_OK) die(3);
    exec(db, "PRAGMA journal_mode=WAL");
    if (fresh) {
        bool ok = exec(db, "BEGIN") &&
                  exec(db, "CREATE TABLE identity (id INTEGER PRIMARY KEY CHECK (id = 1), public_key TEXT NOT NULL)") &&
                  exec(db, "INSERT INTO identity VALUES (1, 'fake-identity-key')") &&
                  exec(db, "CREATE TABLE markers (n INTEGER PRIMARY KEY, build TEXT NOT NULL, opened_at INTEGER NOT NULL)") &&
                  exec(db, "CREATE TABLE parties_schema (id INTEGER PRIMARY KEY CHECK (id = 1), version INTEGER NOT NULL, "
                           "compat_floor INTEGER NOT NULL)") &&
                  exec(db, "INSERT INTO parties_schema VALUES (1, " + std::to_string(build.schema) + ", " +
                               std::to_string(build.floor) + ")") &&
                  exec(db, "COMMIT");
        if (!ok) die(3);
        return build.schema;
    }
    std::optional<DataInfo> info = read_schema(db);
    if (!info) die(3);
    switch (compatibility(build.schema, *info)) {
    case Compatibility::Incompatible: die(4);
    case Compatibility::MigratesForward: {
        uint32_t floor = std::max(info->compat_floor, build.floor);
        if (!exec(db, "UPDATE parties_schema SET version = " + std::to_string(build.schema) +
                          ", compat_floor = " + std::to_string(floor) + " WHERE id = 1"))
            die(2);
        break;
    }
    case Compatibility::ReadsAsIs: break;
    }
    return info->version;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring data_dir, pipe;
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a.starts_with(L"--data-dir=")) data_dir = a.substr(11);
        else if (a.starts_with(L"--supervisor-pipe=")) pipe = a.substr(18);
    }
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const BuildInfo build = read_build_info(fs::path(exe).parent_path());
    const std::string fault = fault_for(build.version);
    const int run_ms = std::atoi(env("PARTIES_FAKE_RUN_MS").c_str());

    if (pipe.empty() || data_dir.empty()) return 2;
    Link peer(pipe);
    if (!peer.ok()) return 2;
    peer.send(link::Hello{kLinkProtocol, kMinLauncherLink, *Version::parse(build.version)});
    auto welcome_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    bool welcomed = false;
    while (!welcomed && std::chrono::steady_clock::now() < welcome_deadline) {
        if (auto m = peer.poll()) welcomed = std::holds_alternative<link::Welcome>(*m);
        else std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (!welcomed) return 2;

    if (fault == "exit-init-failed") return 2;
    if (fault == "exit-data-unavailable") return 3;
    if (fault == "crash-before-health") die(kCrash);
    if (fault == "hang-before-health") Sleep(INFINITE);

    sqlite3* db = nullptr;
    uint32_t opened_at = open_data(fs::path(data_dir) / "parties_client.db", build, db);
    exec(db, "INSERT INTO markers (build, opened_at) VALUES ('" + build.version + "', " + std::to_string(opened_at) + ")");
    if (fault == "migrate-then-crash") die(kCrash);

    peer.send(link::Healthy{build.schema});
    if (fault == "crash-after-health") {
        Sleep(100);
        die(kCrash);
    }

    // Switches to Beta and waits, then takes "switch now" once it is offered.
    if (fault == "wait-then-downgrade")
        peer.send(link::Request{link::SetChannel{Channel::Beta, link::ChannelChoice::Wait}});
    const std::string status_log = env("PARTIES_FAKE_STATUS_LOG");
    bool restart_asked = false;
    bool downgrade_asked = false;
    // These faults assert on what the check told the app, so the app stays
    // until the check of its channel answers, however long that takes.
    const bool until_checked = fault == "run-until-checked" || fault == "wait-then-downgrade";
    const Channel channel = fault == "wait-then-downgrade" ? Channel::Beta : Channel::Stable;
    bool checking = false;
    bool answered = false;
    const auto started = std::chrono::steady_clock::now();
    auto running = [&] {
        auto elapsed = std::chrono::steady_clock::now() - started;
        if (until_checked && !answered) return elapsed < std::chrono::seconds(30);
        return elapsed < std::chrono::milliseconds(run_ms > 0 ? run_ms : 1000);
    };
    while (running()) {
        auto m = peer.poll();
        if (!m) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        if (std::holds_alternative<link::ExitRequested>(*m)) break;
        if (std::holds_alternative<link::Status>(*m) && !status_log.empty())
            std::ofstream(status_log, std::ios::app) << link::write(*m);
        if (auto* status = std::get_if<link::Status>(&*m)) {
            if (status->phase == link::Phase::Checking) checking = true;
            else if (checking && status->channel == channel) answered = true;
        }
        if (auto* status = std::get_if<link::Status>(&*m); status && fault == "wait-then-downgrade" &&
                                                          !downgrade_asked && status->phase == link::Phase::Pinned &&
                                                          status->downgrade) {
            downgrade_asked = true;
            peer.send(link::Request{link::SetChannel{Channel::Beta, link::ChannelChoice::DowngradeNow}});
        }
        if (auto* status = std::get_if<link::Status>(&*m); status && !restart_asked &&
                                                          status->phase == link::Phase::ReadyToRestart) {
            restart_asked = true;
            peer.send(link::Request{link::RestartToUpdate{}});
        }
    }
    sqlite3_close(db);
    return 0;
}
