// End to end: PartiesLauncher.exe supervising fake builds (update_fake_app.exe)
// in a throwaway install root, data root and update source: a local folder,
// or the same folder served over HTTP on 127.0.0.1 by FeedServer. Each
// scenario adopts build A, publishes build B in a signed manifest, and runs the
// launcher the way the boot stub does until the outcome settles. With
// kill points on, every scenario is replayed once per effect index n with the
// launcher terminated right before its n-th effect, then restarted, and must
// reach the same outcome without losing the identity or any preserved copy.
//
// usage: update_e2e_test --launcher <exe> --fake-app <exe> --manifest-tool <exe> [--only <name>] [--no-kill]
//        update_e2e_test --launcher <exe> --real-client <exe>

#include "build_info.h"
#include "file_io.h"

#include <client/settings_schema.h>
#include <parties/crypto.h>
#include <parties/update/boot.h>
#include <parties/update/manifest.h>
#include <parties/update/state_codec.h>

#include <sqlite3.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <format>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#pragma comment(lib, "ws2_32.lib")

namespace fs = std::filesystem;
using namespace parties::update;
using namespace parties::update::launcher;
using namespace std::chrono_literals;

namespace {

// Matches the harness key pinned in testing launchers (updater/launcher/src/testing.cpp).
constexpr std::string_view kTestSeed = "f769e71268cc70210831656bcc920641326a5bd43fe4a015881f3515a8f908a8";
constexpr DWORD kKilled = 0xDEAD;
constexpr const char* kTiming = "10,200,600,30";

fs::path g_launcher, g_fake_app, g_manifest_tool;
bool g_verbose = false;
int g_failures = 0;

void fail(const std::string& scenario, const std::string& what) {
    std::cerr << "FAILED [" << scenario << "]: " << what << "\n";
    ++g_failures;
}

std::wstring widen(std::string_view s) { return {s.begin(), s.end()}; }

SigningKey test_key() {
    SigningKey k;
    k.key_id = "47ac18c9b12dc1be";  // what parties_manifest_tool calls this key
    auto seed = Sha256::from_hex(kTestSeed);
    k.secret_key = seed->bytes;
    if (!parties::derive_pubkey(k.secret_key, k.public_key)) std::abort();
    return k;
}

class FeedServer {
public:
    explicit FeedServer(fs::path root) : root_(std::move(root)) {
        listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        int len = sizeof addr;
        if (listener_ == INVALID_SOCKET || bind(listener_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 ||
            listen(listener_, 16) != 0 || getsockname(listener_, reinterpret_cast<sockaddr*>(&addr), &len) != 0)
            std::abort();
        port_ = ntohs(addr.sin_port);
        accept_thread_ = std::thread([this] { accept_loop(); });
    }
    ~FeedServer() {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        released_.notify_all();
        closesocket(listener_);
        accept_thread_.join();
        for (std::thread& t : connections_) t.join();
    }

    std::wstring base() const { return L"http://127.0.0.1:" + std::to_wstring(port_) + L"/"; }
    void tamper(const std::string& path) {
        std::lock_guard lock(mutex_);
        tampered_.insert(path);
    }
    void stall(const std::string& path) {
        std::lock_guard lock(mutex_);
        stalled_path_ = path;
        stalled_ = false;
    }
    bool stalled() const {
        std::lock_guard lock(mutex_);
        return stalled_;
    }
    void release() {
        {
            std::lock_guard lock(mutex_);
            stalled_path_.clear();
            stalled_ = false;
        }
        released_.notify_all();
    }

private:
    void accept_loop() {
        for (;;) {
            SOCKET s = accept(listener_, nullptr, nullptr);
            if (s == INVALID_SOCKET) return;
            std::lock_guard lock(mutex_);
            if (stopping_) {
                closesocket(s);
                return;
            }
            connections_.emplace_back([this, s] { serve(s); });
        }
    }

    static void send_all(SOCKET s, std::string_view bytes) {
        while (!bytes.empty()) {
            int n = send(s, bytes.data(), static_cast<int>(std::min<size_t>(bytes.size(), 1 << 16)), 0);
            if (n <= 0) return;
            bytes.remove_prefix(static_cast<size_t>(n));
        }
    }

    void serve(SOCKET s) {
        std::string request;
        char buffer[4096];
        while (request.find("\r\n\r\n") == std::string::npos && request.size() < 16384) {
            int n = recv(s, buffer, sizeof buffer, 0);
            if (n <= 0) break;
            request.append(buffer, static_cast<size_t>(n));
        }
        std::string path;
        if (request.starts_with("GET /")) path = request.substr(5, request.find(' ', 5) - 5);
        auto body = path.find("..") == std::string::npos ? read_file(root_ / path) : std::nullopt;
        if (!body) {
            send_all(s, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
            closesocket(s);
            return;
        }
        std::unique_lock lock(mutex_);
        if (tampered_.contains(path) && !body->empty()) (*body)[body->size() / 2] ^= 0x5a;
        bool stall = !stalled_path_.empty() && path == stalled_path_;
        lock.unlock();
        send_all(s, "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body->size()) +
                        "\r\nConnection: close\r\n\r\n");
        size_t sent = 0;
        if (stall) {
            sent = body->size() / 2;
            send_all(s, std::string_view(*body).substr(0, sent));
            lock.lock();
            stalled_ = true;
            released_.wait(lock, [&] { return stopping_ || stalled_path_.empty(); });
            if (stopping_) sent = body->size();
            lock.unlock();
        }
        send_all(s, std::string_view(*body).substr(sent));
        shutdown(s, SD_SEND);
        closesocket(s);
    }

    fs::path root_;
    SOCKET listener_ = INVALID_SOCKET;
    uint16_t port_ = 0;
    std::thread accept_thread_;
    mutable std::mutex mutex_;
    std::condition_variable released_;
    std::vector<std::thread> connections_;
    std::set<std::string> tampered_;
    std::string stalled_path_;
    bool stalled_ = false;
    bool stopping_ = false;
};

class TempRoot {
public:
    explicit TempRoot(const std::string& name) {
        static int counter = 0;
        path_ = fs::temp_directory_path() /
                ("parties-e2e-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(counter++) + "-" + name);
        fs::remove_all(path_);
        fs::create_directories(path_);
    }
    ~TempRoot() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

struct Release {
    BuildEntry entry;
    fs::path dir;
};

FileEntry file_entry(const fs::path& file) {
    auto digest = digest_file(file);
    if (!digest) std::abort();
    return {file.filename().string(), digest->size, digest->sha256,
            {"https://example.invalid/files/" + digest->sha256.to_hex()}};
}

Release make_release(const fs::path& root, const fs::path& app_exe, const std::string& version, uint32_t schema,
                     uint32_t floor, bool fake) {
    Release r;
    r.dir = root / "releases" / version;
    fs::create_directories(r.dir);
    fs::copy_file(g_launcher, r.dir / kLauncherFile);
    fs::copy_file(app_exe, r.dir / kAppFile);
    r.entry.version = *Version::parse(version);
    r.entry.data_schema = schema;
    r.entry.data_compat_floor = floor;
    r.entry.files = {file_entry(r.dir / kLauncherFile), file_entry(r.dir / kAppFile)};
    if (fake) {
        std::ofstream(r.dir / "fake-build.json") << "{\"version\":\"" << version << "\",\"data_schema\":" << schema
                                                 << ",\"data_compat_floor\":" << floor << "}";
        r.entry.files.push_back(file_entry(r.dir / "fake-build.json"));
    }
    return r;
}

fs::path install(const fs::path& install_root, const Release& r) {
    fs::path dir = install_root / "builds" / r.entry.id().dir_name();
    fs::create_directories(dir);
    for (const FileEntry& f : r.entry.files) fs::copy_file(r.dir / f.name, dir / f.name);
    std::string error;
    if (!durable_replace(dir / kBuildInfoFile, write_build_info(r.entry), error)) std::abort();
    return dir;
}

void sign_and_publish(const fs::path& source, Channel channel, const std::string& payload) {
    SigningKey key = test_key();
    auto envelope = sign_manifest({reinterpret_cast<const uint8_t*>(payload.data()), payload.size()}, {&key, 1});
    std::string error;
    const fs::path file = source / (std::string(channel_name(channel)) + ".manifest.json");
    if (!envelope || !durable_replace(file, *envelope, error)) std::abort();
}

struct Publish {
    Channel channel = Channel::Stable;
    std::vector<Version> revoked;
    bool expired = false;
};

void publish(const fs::path& source, const std::vector<const Release*>& releases, uint64_t serial,
             const Publish& how = {}) {
    fs::create_directories(source / "files");
    ManifestPayload p;
    p.channel = how.channel;
    p.serial = serial;
    p.issued_at = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()) -
                  (how.expired ? std::chrono::days(50) : std::chrono::days(0)) - 60s;
    p.expires_at = p.issued_at + std::chrono::days(45);
    p.revoked = how.revoked;
    for (const Release* r : releases) {
        p.builds.push_back(r->entry);
        for (const FileEntry& f : r->entry.files)
            fs::copy_file(r->dir / f.name, source / "files" / f.sha256.to_hex(), fs::copy_options::skip_existing);
    }
    std::ranges::sort(p.builds, [](const BuildEntry& a, const BuildEntry& b) { return a.version > b.version; });
    sign_and_publish(source, how.channel, write_payload(p));
}

class Db {
public:
    explicit Db(const fs::path& path) {
        auto u8 = path.u8string();
        std::string uri = "file:" + std::string(u8.begin(), u8.end()) + "?mode=ro";
        std::ranges::replace(uri, '\\', '/');
        if (sqlite3_open_v2(uri.c_str(), &db_, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, nullptr) != SQLITE_OK) {
            sqlite3_close(db_);
            db_ = nullptr;
        }
    }
    ~Db() { sqlite3_close(db_); }
    bool ok() const { return db_ != nullptr; }
    long long scalar(const std::string& sql) const {
        sqlite3_stmt* stmt = nullptr;
        long long v = -1;
        if (db_ && sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK &&
            sqlite3_step(stmt) == SQLITE_ROW)
            v = sqlite3_column_int64(stmt, 0);
        sqlite3_finalize(stmt);
        return v;
    }

private:
    sqlite3* db_ = nullptr;
};

struct World {
    std::string name;
    TempRoot root;
    fs::path install_root, data_root, source, counter;
    Release a, b;
    bool with_b = true;
    size_t log_mark = 0;
    std::unique_ptr<FeedServer> server;

    World(std::string n, const std::string& b_version, uint32_t b_schema, uint32_t b_floor, bool publish_b)
        : name(std::move(n)), root(name) {
        install_root = root.path() / "install";
        data_root = root.path() / "data-root";
        source = root.path() / "source";
        counter = root.path() / "kill-counter";
        a = make_release(root.path(), g_fake_app, "1.0.0.0", 1, 1, true);
        b = make_release(root.path(), g_fake_app, b_version, b_schema, b_floor, true);
        install(install_root, a);
        with_b = publish_b;
        if (publish_b) publish(source, {&a, &b}, 1);
        else publish(source, {&a}, 1);
    }

    fs::path launcher_in(const Release& r) const { return install_root / "builds" / r.entry.id().dir_name() / kLauncherFile; }
    fs::path db() const { return data_root / "data" / "parties_client.db"; }
    fs::path backups() const { return data_root / "backups"; }
    fs::path log_file() const { return data_root / "logs" / "launcher.log"; }
    fs::path status_log() const { return root.path() / "status.log"; }
    fs::path build_dir(const Release& r) const { return install_root / "builds" / r.entry.id().dir_name(); }
    std::string statuses() const { return read_file(status_log()).value_or(""); }
    std::string full_log() const { return read_file(log_file()).value_or(""); }
    bool leftovers() const {
        std::error_code ec;
        for (const auto& item : fs::directory_iterator(install_root / "builds", ec))
            if (item.path().filename().string().starts_with(".staging-")) return true;
        for (const auto& item : fs::directory_iterator(install_root / "downloads", ec))
            if (item.path().extension() == ".partial") return true;
        return false;
    }

    std::optional<SupervisorState> state() const {
        auto text = read_file(install_root / "state" / "supervisor.json");
        if (!text) return std::nullopt;
        auto s = parse_state(*text);
        return s ? std::optional(*s) : std::nullopt;
    }
    std::optional<boot::SessionReport> session() const {
        auto text = read_file(install_root / "state" / "launcher-session.json");
        return text ? boot::parse_session_report(*text) : std::nullopt;
    }
    std::string last_run_log() const {
        auto text = read_file(log_file()).value_or("");
        return log_mark <= text.size() ? text.substr(log_mark) : text;
    }
};

// What revoke.yml does: the real manifest tool revokes `version` from what is
// published, and the harness signs the payloads it writes.
void revoke_with_tool(World& w, const Version& version, uint64_t serial) {
    const fs::path work = w.root.path() / "revoke";
    fs::create_directories(work / "ring");
    const SigningKey key = test_key();
    std::string pub;
    for (uint8_t b : key.public_key) pub += std::format("{:02x}", b);
    std::ofstream(work / "ring" / "harness.pub") << pub;
    std::wstring cmd = L"\"" + g_manifest_tool.wstring() + L"\" revoke --version " + widen(version.to_string()) +
                       L" --serial " + std::to_wstring(serial) + L" --expires-days 45 --out-dir \"" + work.wstring() +
                       L"\" --keyring \"" + (work / "ring").wstring() + L"\"";
    for (Channel c : {Channel::Beta, Channel::Stable}) {
        const fs::path published = w.source / (std::string(channel_name(c)) + ".manifest.json");
        cmd += L" --" + widen(channel_name(c)) + L" \"" + (fs::exists(published) ? published.wstring() : L"none") + L"\"";
    }
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    DWORD code = 1;
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, work.c_str(), &si, &pi)) {
        WaitForSingleObject(pi.hProcess, INFINITE);
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    if (code != 0) return fail(w.name, "parties_manifest_tool revoke exited " + std::to_string(code));
    for (Channel c : {Channel::Beta, Channel::Stable}) {
        auto payload = read_file(work / (std::string(channel_name(c)) + ".payload.json"));
        if (!payload) return fail(w.name, std::string("the tool wrote no ") + std::string(channel_name(c)) + " payload");
        sign_and_publish(w.source, c, *payload);
    }
}

struct RunResult {
    DWORD exit_code = 0;
    bool boot_ok = false;
    bool timed_out = false;
};

RunResult run_launcher(World& w, const std::map<std::string, std::string>& extra_env,
                       const std::vector<std::wstring>& forwarded = {}, DWORD timeout_ms = 60000,
                       const fs::path& launcher = {}, const std::function<bool()>& kill_when = {},
                       const std::wstring& launcher_flags = {}) {
    w.log_mark = read_file(w.log_file()).value_or("").size();
    std::map<std::wstring, std::wstring> env;
    for (wchar_t* block = GetEnvironmentStringsW(), *p = block; *p; p += wcslen(p) + 1) {
        std::wstring kv = p;
        size_t eq = kv.find(L'=', 1);
        if (eq != std::wstring::npos) env[kv.substr(0, eq)] = kv.substr(eq + 1);
    }
    env[L"PARTIES_FAKE_STATUS_LOG"] = w.status_log().wstring();
    for (const auto& [k, v] : extra_env) env[widen(k)] = widen(v);
    std::wstring block;
    for (const auto& [k, v] : env) block += k + L"=" + v + L'\0';
    block += L'\0';

    SECURITY_ATTRIBUTES inherit{sizeof inherit, nullptr, TRUE};
    HANDLE boot_ok = CreateEventW(&inherit, TRUE, FALSE, nullptr);
    fs::path exe = launcher.empty() ? w.launcher_in(w.a) : launcher;
    std::wstring cmd = L"\"" + exe.wstring() + L"\" \"--install-root=" + w.install_root.wstring() +
                       L"\" \"--data-root=" + w.data_root.wstring() +
                       (w.server ? L"\" \"--feed-base=" + w.server->base() : L"\" \"--feed-dir=" + w.source.wstring()) +
                       L"\" --boot-session=42 --boot-ok-event=" +
                       std::to_wstring(reinterpret_cast<uintptr_t>(boot_ok)) +
                       L" --test-namespace=PartiesE2E" + std::to_wstring(GetCurrentProcessId()) + launcher_flags + L" --";
    for (const std::wstring& f : forwarded) cmd += L" " + f;
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    RunResult result;
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_UNICODE_ENVIRONMENT, block.data(),
                        w.install_root.c_str(), &si, &pi)) {
        fail(w.name, "cannot start the launcher");
        CloseHandle(boot_ok);
        return result;
    }
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (WaitForSingleObject(pi.hProcess, 20) != WAIT_OBJECT_0) {
        if (kill_when && kill_when()) {
            TerminateProcess(pi.hProcess, kKilled);
            WaitForSingleObject(pi.hProcess, INFINITE);
            break;
        }
        if (std::chrono::steady_clock::now() > deadline) {
            result.timed_out = true;
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, INFINITE);
        }
    }
    GetExitCodeProcess(pi.hProcess, &result.exit_code);
    result.boot_ok = WaitForSingleObject(boot_ok, 0) == WAIT_OBJECT_0;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(boot_ok);
    return result;
}

bool end_session(const World& w) {
    struct Search {
        std::wstring root;
        HWND found = nullptr;
    } search{w.install_root.wstring()};
    EnumWindows(
        [](HWND hwnd, LPARAM lp) -> BOOL {
            auto* s = reinterpret_cast<Search*>(lp);
            wchar_t cls[64];
            if (!GetClassNameW(hwnd, cls, 64) || std::wstring_view(cls) != L"PartiesLauncherSession") return TRUE;
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            wchar_t image[4096];
            DWORD n = 4096;
            bool ours = p && QueryFullProcessImageNameW(p, 0, image, &n) && n > s->root.size() &&
                        _wcsnicmp(image, s->root.c_str(), s->root.size()) == 0;
            if (p) CloseHandle(p);
            if (ours) s->found = hwnd;
            return !ours;
        },
        reinterpret_cast<LPARAM>(&search));
    DWORD_PTR ignored = 0;
    return search.found &&
           SendMessageTimeoutW(search.found, WM_ENDSESSION, TRUE, 0, SMTO_ABORTIFHUNG, 5000, &ignored) != 0;
}

std::string stalled_app_file(const World& w) { return "files/" + w.b.entry.file(kAppFile)->sha256.to_hex(); }

RunResult run_short_session(World& w, const std::function<void()>& headless) {
    w.server->stall(stalled_app_file(w));
    const size_t mark = w.full_log().size();
    return run_launcher(w, {{"PARTIES_TEST_TIMING", kTiming}, {"PARTIES_FAKE_RUN_MS", "50"}}, {}, 60000, {}, [&] {
        if (w.full_log().find("app exited", mark) != std::string::npos && w.server->stalled()) headless();
        return false;
    });
}

const BuildRecord* record(const SupervisorState& s, const Release& r) { return s.find(r.entry.id()); }

bool staged(const SupervisorState& s, const Release& r) {
    auto* st = std::get_if<Staged>(&s.activation);
    return st && st->build == r.entry.id();
}

bool settled(const SupervisorState& s) { return std::holds_alternative<Idle>(s.operation); }

bool good_is_b(const World& w) {
    auto s = w.state();
    return s && settled(*s) && s->good == w.b.entry.id();
}

void prove_b(World& w) {
    for (int i = 0; i < 6 && !good_is_b(w); ++i)
        run_launcher(w, {{"PARTIES_TEST_TIMING", kTiming}, {"PARTIES_FAKE_RUN_MS", "700"}});
    auto s = w.state();
    if (!s || s->good != w.b.entry.id()) fail(w.name, "B did not become good");
}

bool rolled_back_to_a(const World& w, const SupervisorState& s, FailureReason reason) {
    const BuildRecord* b = record(s, w.b);
    return settled(s) && std::holds_alternative<Steady>(s.activation) && s.good == w.a.entry.id() && b &&
           b->verdict == Verdict{Quarantined{reason}} && s.notice && s.notice->kind == Notice::Kind::RolledBack;
}

void check_identity_everywhere(const World& w, const std::string& label) {
    Db live(w.db());
    if (live.scalar("SELECT count(*) FROM identity") != 1) fail(label, "the live database lost its identity row");
    std::error_code ec;
    for (const auto& item : fs::directory_iterator(w.backups(), ec)) {
        if (item.path().extension() != ".db") continue;
        Db copy(item.path());
        if (copy.scalar("SELECT count(*) FROM identity") != 1)
            fail(label, "backup " + item.path().filename().string() + " has no identity row");
    }
}

long long markers(const fs::path& db, const std::string& build) {
    Db d(db);
    return d.scalar("SELECT count(*) FROM markers WHERE build = '" + build + "'");
}

std::optional<fs::path> find_backup(const World& w, std::string_view prefix) {
    std::error_code ec;
    for (const auto& item : fs::directory_iterator(w.backups(), ec))
        if (item.path().filename().string().starts_with(prefix) && item.path().extension() == ".db") return item.path();
    return std::nullopt;
}

struct Scenario {
    std::string name;
    std::string b_fault;
    uint32_t b_schema = 1;
    uint32_t b_floor = 1;
    bool publish_b = true;
    std::function<void(World&)> prepare;
    std::function<bool(const World&)> done;
    std::function<void(const World&, const std::string&)> verify;
    bool http = false;
    bool kill_points = true;
    std::map<std::string, std::string> env{};
    std::wstring launcher_flags{}; // before "--", e.g. " --recover"
};

enum class Outcome { Converged, KillNotReached, Failed };

Outcome run_scenario(const Scenario& sc, std::optional<int> kill_at) {
    const std::string label = sc.name + (kill_at ? " kill@" + std::to_string(*kill_at) : "");
    World w(sc.name, "2.0.0.0", sc.b_schema, sc.b_floor, sc.publish_b);
    if (sc.http) w.server = std::make_unique<FeedServer>(w.source);
    std::map<std::string, std::string> env{
        {"PARTIES_TEST_TIMING", kTiming},
        {"PARTIES_TEST_RECOVERY", "quit"},
        {"PARTIES_FAKE_RUN_MS", "700"},
        {"PARTIES_FAKE_FAULT", sc.b_fault.empty() ? "" : "2.0.0.0:" + sc.b_fault},
    };
    for (const auto& [k, v] : sc.env) env[k] = v;
    if (sc.prepare) sc.prepare(w);

    bool killed = false;
    for (int run = 0; run < 8; ++run) {
        auto run_env = env;
        if (kill_at && !killed) {
            run_env["PARTIES_KILL_AT"] = std::to_string(*kill_at);
            run_env["PARTIES_KILL_COUNTER"] = w.counter.string();
        }
        RunResult r = run_launcher(w, run_env, {}, 60000, {}, {}, sc.launcher_flags);
        if (g_verbose) std::cout << "--- " << label << " run " << run << " exit " << r.exit_code << "\n" << w.last_run_log();
        if (r.timed_out) {
            fail(label, "the launcher did not exit within 60 s");
            return Outcome::Failed;
        }
        if (r.exit_code == kKilled) {
            killed = true;
            auto session = w.session();
            if (!session || session->ended_clean) fail(label, "a killed launcher left a clean session report");
            continue;
        }
        if (r.exit_code != 0) {
            fail(label, "the launcher exited with " + std::to_string(r.exit_code));
            return Outcome::Failed;
        }
        auto session = w.session();
        if (!session || !session->ended_clean || session->session != 42)
            fail(label, "an orderly exit did not leave a clean session report");
        if (sc.done(w)) {
            if (kill_at && !killed) return Outcome::KillNotReached;
            sc.verify(w, label);
            check_identity_everywhere(w, label);
            return Outcome::Converged;
        }
    }
    auto text = read_file(w.install_root / "state" / "supervisor.json");
    fail(label, "did not settle after 8 launcher runs; state:\n" + text.value_or("(none)") + "\nlast log:\n" +
                    w.last_run_log());
    return Outcome::Failed;
}

std::vector<Scenario> scenarios() {
    std::vector<Scenario> out;

    out.push_back({"update-ok", "", 1, 1, true, nullptr,
                   [](const World& w) {
                       auto s = w.state();
                       return s && settled(*s) && s->good == w.b.entry.id();
                   },
                   [](const World& w, const std::string& label) {
                       auto s = w.state();
                       if (s->previous_good != w.a.entry.id()) fail(label, "A is not kept as previous_good");
                       if (!std::holds_alternative<Steady>(s->activation)) fail(label, "not steady after confirm");
                       if (markers(w.db(), "2.0.0.0") < 1) fail(label, "B never ran");
                       auto contract = boot::parse_boot_contract(
                           read_file(w.install_root / "state" / "boot-contract.json").value_or(""));
                       if (!contract || contract->good.dir != build_dir(w.b.entry.id()))
                           fail(label, "boot-contract.json does not point at B");
                       if (w.leftovers()) fail(label, "a partial download or staging directory was left behind");
                   },
                   true});

    auto rollback_scenario = [](std::string name, std::string fault, FailureReason reason) {
        return Scenario{name, fault, 1, 1, true, nullptr,
                        [reason](const World& w) {
                            auto s = w.state();
                            return s && rolled_back_to_a(w, *s, reason);
                        },
                        [](const World& w, const std::string& label) {
                            Db live(w.db());
                            if (live.scalar("SELECT version FROM parties_schema") != 1)
                                fail(label, "the live database schema changed");
                            if (markers(w.db(), "2.0.0.0") != 0) fail(label, "B wrote to the live database");
                            if (markers(w.db(), "1.0.0.0") < 1) fail(label, "A never ran");
                        }};
    };
    out.push_back(rollback_scenario("crash-before-health", "crash-before-health", FailureReason::CrashedBeforeHealthy));
    out.push_back(rollback_scenario("hang-before-health", "hang-before-health", FailureReason::HungBeforeHealthy));
    out.push_back(rollback_scenario("init-failed", "exit-init-failed", FailureReason::InitFailed));

    out.push_back({"crash-after-health", "crash-after-health", 1, 1, true, nullptr,
                   [](const World& w) {
                       auto s = w.state();
                       // A kill during B's second run leaves that outcome unobserved, which
                       // is the second strike as well.
                       return s && (rolled_back_to_a(w, *s, FailureReason::CrashedRepeatedlyAfterHealthy) ||
                                    rolled_back_to_a(w, *s, FailureReason::UnknownOutcomeRepeated));
                   },
                   [](const World& w, const std::string& label) {
                       if (markers(w.db(), "2.0.0.0") < 1) fail(label, "B never ran");
                   }});

    out.push_back({"migrate-then-crash", "migrate-then-crash", 3, 3, true, nullptr,
                   [](const World& w) {
                       auto s = w.state();
                       return s && rolled_back_to_a(w, *s, FailureReason::CrashedBeforeHealthy) &&
                              markers(w.db(), "1.0.0.0") >= 1 && w.last_run_log().find("app exited with code 0x0") !=
                                                                    std::string::npos;
                   },
                   [](const World& w, const std::string& label) {
                       auto s = w.state();
                       Db live(w.db());
                       if (live.scalar("SELECT version FROM parties_schema") != 1 ||
                           live.scalar("SELECT compat_floor FROM parties_schema") != 1)
                           fail(label, "the live database was not restored to schema 1");
                       if (markers(w.db(), "2.0.0.0") != 0) fail(label, "B's rows are still in the live database");
                       auto preserved = find_backup(w, "rolled-back-");
                       if (!preserved) return fail(label, "the migrated database was not preserved");
                       if (!s->notice || !s->notice->preserved ||
                           preserved->stem().string() != s->notice->preserved->stem)
                           fail(label, "the notice does not name the preserved copy");
                       Db kept(*preserved);
                       if (kept.scalar("SELECT version FROM parties_schema") != 3 ||
                           kept.scalar("SELECT compat_floor FROM parties_schema") != 3)
                           fail(label, "the preserved copy is not the migrated database");
                       if (kept.scalar("SELECT count(*) FROM markers WHERE build = '2.0.0.0'") < 1)
                           fail(label, "the preserved copy lost B's marker row");
                       if (w.statuses().find("\"preserved\":\"backups\\\\" + preserved->filename().string() + "\"") ==
                           std::string::npos)
                           fail(label, "the notice does not give the preserved copy relative to the data root");
                       if (!find_backup(w, "pre-")) fail(label, "no pre-candidate snapshot");
                   }});

    // Setup staged B before A ever opened the data, so B starts with no database and no
    // pre-candidate snapshot. B creates its own database, whose floor A cannot read, and
    // crashes. The rollback sets B's database aside so A starts on fresh data.
    out.push_back({"fresh-data-then-crash", "migrate-then-crash", 3, 3, true,
                   [](World& w) {
                       // Stopped right after B is staged, as the setup of a newer version leaves it.
                       run_launcher(w, {{"PARTIES_TEST_TIMING", kTiming}, {"PARTIES_FAKE_RUN_MS", "700"},
                                        {"PARTIES_KILL_AT", "6"}});
                       if (!w.state() || !staged(*w.state(), w.b)) fail(w.name, "B was not staged");
                       std::error_code ec;
                       for (const char* suffix : {"", "-wal", "-shm"})
                           fs::remove(fs::path(w.db().wstring() + widen(suffix)), ec);
                       fs::remove_all(w.backups(), ec);
                   },
                   [](const World& w) {
                       auto s = w.state();
                       return s && rolled_back_to_a(w, *s, FailureReason::CrashedBeforeHealthy) &&
                              markers(w.db(), "1.0.0.0") >= 1;
                   },
                   [](const World& w, const std::string& label) {
                       auto s = w.state();
                       Db live(w.db());
                       if (live.scalar("SELECT version FROM parties_schema") != 1)
                           fail(label, "A does not run on a database of its own schema");
                       if (markers(w.db(), "2.0.0.0") != 0) fail(label, "B's rows are in the live database");
                       auto preserved = find_backup(w, "rolled-back-");
                       if (!preserved) return fail(label, "B's database was not kept");
                       if (!s->notice || !s->notice->preserved || preserved->stem().string() != s->notice->preserved->stem)
                           fail(label, "the notice does not name the kept copy");
                       Db kept(*preserved);
                       if (kept.scalar("SELECT compat_floor FROM parties_schema") != 3 ||
                           kept.scalar("SELECT count(*) FROM markers WHERE build = '2.0.0.0'") < 1)
                           fail(label, "the kept copy is not B's database");
                       if (w.full_log().find("recovery shown") != std::string::npos)
                           fail(label, "recovery was shown after an automatic rollback");
                   }});

    // An earlier B left data A cannot open, then A was installed over it and B staged again.
    // B is trusted at once, since a rollback to A could only end in Recovery, so B's crash
    // shows "failed to start" instead of a rollback onto data A cannot open.
    out.push_back({"newer-data-then-crash", "migrate-then-crash", 3, 3, true,
                   [](World& w) {
                       run_launcher(w, {{"PARTIES_TEST_TIMING", kTiming}, {"PARTIES_FAKE_RUN_MS", "700"},
                                        {"PARTIES_KILL_AT", "6"}});
                       if (!w.state() || !staged(*w.state(), w.b)) fail(w.name, "B was not staged");
                       std::error_code ec;
                       for (const char* suffix : {"", "-wal", "-shm"})
                           fs::remove(fs::path(w.db().wstring() + widen(suffix)), ec);
                       fs::remove_all(w.backups(), ec);
                       sqlite3* db = nullptr;
                       auto u8 = w.db().u8string();
                       sqlite3_open(std::string(u8.begin(), u8.end()).c_str(), &db);
                       sqlite3_exec(db,
                                    "CREATE TABLE identity (id INTEGER PRIMARY KEY CHECK (id = 1), public_key TEXT NOT NULL);"
                                    "INSERT INTO identity VALUES (1, 'fake-identity-key');"
                                    "CREATE TABLE markers (n INTEGER PRIMARY KEY, build TEXT NOT NULL, opened_at INTEGER NOT NULL);"
                                    "INSERT INTO markers (build, opened_at) VALUES ('2.0.0.0', 3);"
                                    "CREATE TABLE parties_schema (id INTEGER PRIMARY KEY CHECK (id = 1), version INTEGER NOT NULL,"
                                    " compat_floor INTEGER NOT NULL);"
                                    "INSERT INTO parties_schema VALUES (1, 3, 3);",
                                    nullptr, nullptr, nullptr);
                       sqlite3_close(db);
                   },
                   [](const World& w) {
                       auto s = w.state();
                       return s && settled(*s) && std::holds_alternative<Steady>(s->activation);
                   },
                   [](const World& w, const std::string& label) {
                       auto s = w.state();
                       if (s->good != w.b.entry.id()) fail(label, "B is not good");
                       if (s->previous_good != w.a.entry.id()) fail(label, "A is not kept as previous_good");
                       if (record(*s, w.b)->quarantined()) fail(label, "B was rolled back");
                       if (Db(w.db()).scalar("SELECT compat_floor FROM parties_schema") != 3)
                           fail(label, "the live data was replaced");
                       if (label.find("kill@") == std::string::npos && markers(w.db(), "2.0.0.0") < 2)
                           fail(label, "B did not run on the live data");
                       if (w.full_log().find("recovery shown: data_incompatible") != std::string::npos)
                           fail(label, "Recovery said the data is from a newer version");
                   }});

    // B (schema 3, floor 3) is proven, so A cannot read the live data. Recovery's "Start the
    // previous version" starts A on the newest backup A can open, keeping B's database.
    out.push_back({"recovery-start-previous", "", 3, 3, true,
                   [](World& w) {
                       for (int i = 0; i < 6 && !good_is_b(w); ++i)
                           run_launcher(w, {{"PARTIES_TEST_TIMING", kTiming}, {"PARTIES_FAKE_RUN_MS", "700"}});
                       if (!good_is_b(w)) fail(w.name, "B did not become good");
                   },
                   [](const World& w) {
                       auto s = w.state();
                       return s && settled(*s) && std::holds_alternative<Steady>(s->activation) &&
                              s->good == w.a.entry.id() && Db(w.db()).scalar("SELECT version FROM parties_schema") == 1;
                   },
                   [](const World& w, const std::string& label) {
                       if (markers(w.db(), "2.0.0.0") != 0) fail(label, "B's rows are in the restored database");
                       if (label.find("kill@") == std::string::npos && markers(w.db(), "1.0.0.0") < 2)
                           fail(label, "A did not start on the restored data");
                       auto kept = find_backup(w, "pre-rollback-");
                       if (!kept) return fail(label, "B's database was not kept");
                       Db copy(*kept);
                       if (copy.scalar("SELECT compat_floor FROM parties_schema") != 3 ||
                           copy.scalar("SELECT count(*) FROM markers WHERE build = '2.0.0.0'") < 1)
                           fail(label, "the kept copy is not B's database");
                       if (w.full_log().find("recovery shown: data_incompatible") != std::string::npos)
                           fail(label, "Start the previous version ended in Recovery");
                   },
                   false, true, {{"PARTIES_TEST_RECOVERY", "previous"}}, L" --recover"});

    out.push_back({"exit-data-unavailable", "exit-data-unavailable", 1, 1, true, nullptr,
                   [](const World& w) {
                       auto s = w.state();
                       return s && settled(*s) && std::holds_alternative<Probation>(s->activation) &&
                              w.last_run_log().find("recovery shown: data_unavailable") != std::string::npos;
                   },
                   [](const World& w, const std::string& label) {
                       auto s = w.state();
                       const BuildRecord* b = record(*s, w.b);
                       if (!b || b->quarantined()) fail(label, "B was blamed for unavailable data");
                       if (s->good != w.a.entry.id()) fail(label, "good changed");
                   }});

    auto corrupt_a = [](World& w) {
        RunResult r = run_launcher(w, {{"PARTIES_TEST_TIMING", kTiming}, {"PARTIES_FAKE_RUN_MS", "300"}});
        if (r.exit_code != 0 || markers(w.db(), "1.0.0.0") != 1) fail(w.name, "the clean first run did not start A");
        std::ofstream(w.build_dir(w.a) / kAppFile, std::ios::binary | std::ios::app).put('\0');
    };
    out.push_back({"corrupt-good-exe", "", 1, 1, false, corrupt_a,
                   [](const World& w) {
                       auto s = w.state();
                       return s && settled(*s) && markers(w.db(), "1.0.0.0") >= 2;
                   },
                   [](const World& w, const std::string& label) {
                       auto s = w.state();
                       if (s->good != w.a.entry.id()) fail(label, "good changed");
                       if (digest_file(w.build_dir(w.a) / kAppFile)->sha256 != w.a.entry.file(kAppFile)->sha256)
                           fail(label, "the executable was not repaired");
                       if (w.full_log().find("recovery shown") != std::string::npos)
                           fail(label, "recovery was shown although the feed lists the build");
                   },
                   true});

    out.push_back({"corrupt-good-unlisted", "", 1, 1, false,
                   [corrupt_a](World& w) {
                       corrupt_a(w);
                       publish(w.source, {&w.b}, 2);
                   },
                   [](const World& w) {
                       auto s = w.state();
                       return s && settled(*s) &&
                              w.last_run_log().find("recovery shown: repairing") != std::string::npos;
                   },
                   [](const World& w, const std::string& label) {
                       if (markers(w.db(), "1.0.0.0") != 1) fail(label, "the corrupt executable was started");
                       if (w.state()->good != w.a.entry.id()) fail(label, "good changed");
                   },
                   false, false});

    out.push_back({"tampered-file", "", 1, 1, true,
                   [](World& w) { w.server->tamper("files/" + w.b.entry.file(kAppFile)->sha256.to_hex()); },
                   [](const World& w) {
                       return w.last_run_log().find("SHA-256 does not match the manifest") != std::string::npos;
                   },
                   [](const World& w, const std::string& label) {
                       auto s = w.state();
                       if (!s || s->good != w.a.entry.id() || s->find(w.b.entry.id()))
                           fail(label, "a tampered build reached the state");
                       if (fs::exists(w.build_dir(w.b))) fail(label, "a tampered build was sealed");
                       if (w.leftovers()) fail(label, "the rejected download was left on disk");
                       if (markers(w.db(), "1.0.0.0") < 1) fail(label, "A stopped working");
                   },
                   true, false});

    out.push_back({"replayed-manifest", "", 1, 1, true,
                   [](World& w) {
                       publish(w.source, {&w.a}, 5);
                       RunResult r = run_launcher(w, {{"PARTIES_TEST_TIMING", kTiming}, {"PARTIES_FAKE_RUN_MS", "300"}});
                       if (r.exit_code != 0) fail(w.name, "the first run failed");
                       publish(w.source, {&w.a, &w.b}, 3);
                   },
                   [](const World& w) { return w.last_run_log().find("replayed") != std::string::npos; },
                   [](const World& w, const std::string& label) {
                       auto trust = parse_trust(read_file(w.data_root / "update" / "trust.json").value_or(""));
                       if (!trust || trust->stable.serial != 5) fail(label, "the ratchet moved backwards");
                       if (w.state()->find(w.b.entry.id()) || fs::exists(w.build_dir(w.b)))
                           fail(label, "a replayed manifest staged a build");
                   },
                   true, false});

    out.push_back({"stale-feed", "", 1, 1, true,
                   [](World& w) { publish(w.source, {&w.a, &w.b}, 2, {.expired = true}); },
                   [](const World& w) { return w.last_run_log().find("update feed is stale") != std::string::npos; },
                   [](const World& w, const std::string& label) {
                       if (w.statuses().find("\"phase\":\"feed_stale\"") == std::string::npos)
                           fail(label, "the app was not told the feed is stale");
                       if (w.state()->find(w.b.entry.id())) fail(label, "a stale manifest staged a build");
                       if (markers(w.db(), "1.0.0.0") < 1) fail(label, "A stopped working");
                   },
                   true, false, {{"PARTIES_FAKE_FAULT", "1.0.0.0:run-until-checked"}}});

    out.push_back({"good-revoked", "", 1, 1, true,
                   [](World& w) {
                       Release d = make_release(w.root.path(), g_fake_app, "0.9.0.0", 1, 1, true);
                       w.b.entry.link_protocol = w.b.entry.min_supported_launcher_link = 99;
                       publish(w.source, {&w.b, &d}, 2, {.revoked = {w.a.entry.version}});
                   },
                   [](const World& w) {
                       auto s = w.state();
                       return s && settled(*s) && s->good.version == *Version::parse("0.9.0.0");
                   },
                   [](const World& w, const std::string& label) {
                       auto s = w.state();
                       if (s->find(w.b.entry.id()) || fs::exists(w.build_dir(w.b))) fail(label, "B was staged");
                       if (markers(w.db(), "0.9.0.0") < 1) fail(label, "D never ran");
                   },
                   true, false});

    // B is proven (A is previous_good), then B is revoked the way revoke.yml does it:
    // B dropped from builds and listed in revoked[]. The newest build left is A.
    out.push_back({"adv-revoked-to-previous", "", 1, 1, true,
                   [](World& w) {
                       prove_b(w);
                       publish(w.source, {&w.a}, 3, {.revoked = {w.b.entry.version}});
                   },
                   [](const World& w) {
                       auto s = w.state();
                       return s && settled(*s) && s->good == w.a.entry.id();
                   },
                   [](const World& w, const std::string& label) {
                       auto s = w.state();
                       const BuildRecord* b = record(*s, w.b);
                       if (b && !b->quarantined()) fail(label, "the revoked B can still be rolled back to");
                   },
                   false, false});

    // B (schema 3) is proven and then revoked. A cannot read B's data, so leaving B would
    // restore a backup: Parties keeps B and offers the move instead of making it.
    out.push_back({"adv-revoked-needs-restore", "", 3, 3, true,
                   [](World& w) {
                       prove_b(w);
                       publish(w.source, {&w.a}, 3, {.revoked = {w.b.entry.version}});
                   },
                   [](const World& w) {
                       auto s = w.state();
                       return s && settled(*s) && w.statuses().find("\"kind\":\"withdrawn\"") != std::string::npos;
                   },
                   [](const World& w, const std::string& label) {
                       auto s = w.state();
                       if (s->good != w.b.entry.id() || !std::holds_alternative<Steady>(s->activation))
                           fail(label, "Parties left B without the user's choice");
                       if (Db(w.db()).scalar("SELECT version FROM parties_schema") != 3)
                           fail(label, "the live database was restored without the user's choice");
                       if (w.statuses().find("\"downgrade\":{\"version\":\"1.0.0.0\",\"data_effect\":\"restores_backup\","
                                             "\"backup_taken_at\"") == std::string::npos)
                           fail(label, "the app was not offered A with the date of the backup it restores");
                   },
                   false, false, {{"PARTIES_FAKE_FAULT", "2.0.0.0:run-until-checked"}}});

    // A was installed by a setup whose build is only on Beta, and the install follows Stable,
    // which lists only an older D. The operator revokes A once, with the real manifest tool.
    out.push_back({"adv-beta-only-revoked", "", 1, 1, false,
                   [](World& w) {
                       Release d = make_release(w.root.path(), g_fake_app, "0.9.0.0", 1, 1, true);
                       publish(w.source, {&d}, 2);
                       publish(w.source, {&w.a}, 2, {.channel = Channel::Beta});
                       revoke_with_tool(w, w.a.entry.version, 3);
                   },
                   [](const World& w) {
                       auto s = w.state();
                       return s && settled(*s) && s->good != w.a.entry.id();
                   },
                   [](const World& w, const std::string& label) {
                       if (w.state()->good.version != *Version::parse("0.9.0.0")) fail(label, "the install did not move to D");
                   },
                   false, false});

    // B is downloaded and staged while A runs; then B is revoked before B ever starts. The next
    // start runs B before any check can tell it is revoked, so B may run that once; the
    // revocation then blocks its confirmation and its exit rolls back to A.
    out.push_back({"adv-revoked-staged", "", 1, 1, true,
                   [](World& w) {
                       // Ends session 1 right after B is staged (before the restart's AppExited commit),
                       // standing in for a user who closes the app without clicking "Restart to update".
                       run_launcher(w, {{"PARTIES_TEST_TIMING", kTiming}, {"PARTIES_FAKE_RUN_MS", "700"},
                                        {"PARTIES_KILL_AT", "6"}});
                       auto s = w.state();
                       auto* st = s ? std::get_if<Staged>(&s->activation) : nullptr;
                       if (!st || st->build != w.b.entry.id())
                           fail(w.name, "B was not staged: " +
                                            read_file(w.install_root / "state" / "supervisor.json").value_or("") +
                                            "\nLOG:\n" + w.last_run_log());
                       publish(w.source, {&w.a}, 3, {.revoked = {w.b.entry.version}});
                   },
                   [](const World& w) {
                       auto s = w.state();
                       return s && settled(*s) && std::holds_alternative<Steady>(s->activation);
                   },
                   [](const World& w, const std::string& label) {
                       auto s = w.state();
                       if (s->good == w.b.entry.id()) fail(label, "the revoked build B became good");
                       if (markers(w.db(), "2.0.0.0") > 1) fail(label, "the revoked build B was started again");
                       const BuildRecord* b = record(*s, w.b);
                       if (b && b->verdict != Verdict{Quarantined{FailureReason::Revoked}})
                           fail(label, "B is kept without being quarantined as revoked");
                   },
                   false, false});

    // B is good on Stable. The app switches to Beta (only A, older), takes "switch now",
    // then immediately picks Stable again. The user's last choice is Stable, keep B.
    out.push_back({"adv-downgrade-then-stable", "downgrade-then-stable", 1, 1, true,
                   [](World& w) {
                       prove_b(w);
                       publish(w.source, {&w.a}, 1, {.channel = Channel::Beta});
                   },
                   [](const World& w) {
                       auto s = w.state();
                       return s && settled(*s) && markers(w.db(), "2.0.0.0") >= 3;
                   },
                   [](const World& w, const std::string& label) {
                       auto s = w.state();
                       if (s->channel != Channel::Stable) fail(label, "the channel is not the user's last choice");
                       if (s->good != w.b.entry.id() || !std::holds_alternative<Steady>(s->activation))
                           fail(label, "a downgrade the user withdrew was applied");
                   },
                   false, false});

    out.push_back({"downgrade-now", "wait-then-downgrade", 3, 3, true,
                   [](World& w) {
                       prove_b(w);
                       publish(w.source, {&w.a}, 1, {.channel = Channel::Beta});
                   },
                   [](const World& w) {
                       auto s = w.state();
                       return s && settled(*s) && s->good == w.a.entry.id() && s->channel == Channel::Beta;
                   },
                   [](const World& w, const std::string& label) {
                       auto s = w.state();
                       if (s->previous_good != w.b.entry.id()) fail(label, "B is not kept as previous_good");
                       Db live(w.db());
                       if (live.scalar("SELECT version FROM parties_schema") != 1)
                           fail(label, "the live database was not restored to schema 1");
                       if (markers(w.db(), "2.0.0.0") != 0) fail(label, "B's rows are in the restored database");
                       auto kept = find_backup(w, "pre-downgrade-");
                       if (!kept) return fail(label, "the live database was not preserved");
                       Db copy(*kept);
                       if (copy.scalar("SELECT version FROM parties_schema") != 3 ||
                           copy.scalar("SELECT count(*) FROM markers WHERE build = '2.0.0.0'") < 1)
                           fail(label, "the preserved copy is not B's database");
                       if (w.statuses().find("\"data_effect\":\"restores_backup\",\"backup_taken_at\"") ==
                           std::string::npos)
                           fail(label, "the offer did not name the backup's date");
                   },
                   true});

    out.push_back({"launcher-too-old", "", 1, 1, true,
                   [](World& w) {
                       w.b.entry.link_protocol = w.b.entry.min_supported_launcher_link = 99;
                       publish(w.source, {&w.a, &w.b}, 2);
                   },
                   [](const World& w) { return markers(w.db(), "1.0.0.0") >= 1; },
                   [](const World& w, const std::string& label) {
                       if (w.statuses().find("\"phase\":\"idle\",\"channel\":\"stable\",\"current\":\"1.0.0.0\","
                                             "\"available\":\"2.0.0.0\"") == std::string::npos)
                           fail(label, "the app was not told about the newer build it cannot take yet");
                       if (w.state()->find(w.b.entry.id())) fail(label, "a build this launcher cannot run was staged");
                   },
                   true, false, {{"PARTIES_FAKE_FAULT", "1.0.0.0:run-until-checked"}}});

    out.push_back({"retention", "", 1, 1, false,
                   [](World& w) {
                       RunResult r = run_launcher(w, {{"PARTIES_TEST_TIMING", kTiming}, {"PARTIES_FAKE_RUN_MS", "300"}});
                       if (r.exit_code != 0) fail(w.name, "the first run failed");
                       fs::create_directories(w.backups());
                       sqlite3* db = nullptr;
                       sqlite3_open(w.db().string().c_str(), &db);
                       auto now = fs::file_time_type::clock::now();
                       auto copy = [&](const std::string& stem, int minutes_ago) {
                           fs::path dest = w.backups() / (stem + ".db");
                           std::string sql = "VACUUM INTO '" + dest.string() + "'";
                           if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr) != SQLITE_OK) std::abort();
                           fs::last_write_time(dest, now - std::chrono::minutes(minutes_ago));
                       };
                       for (int g = 1; g <= 13; ++g) copy("pre-" + w.a.entry.id().dir_name() + "-g" + std::to_string(g), 100 - g);
                       copy("pre-migration-v1-20200101T000000Z", 1000);
                       for (const char* stem : {"rolled-back-x", "legacy-20200101T000000Z-0", "pre-uninstall-20200101T000000Z",
                                                "pre-restore-y", "pre-downgrade-z"})
                           copy(stem, 5000);
                       sqlite3_close(db);
                   },
                   [](const World&) { return true; },
                   [](const World& w, const std::string& label) {
                       for (const char* stem : {"rolled-back-x", "legacy-20200101T000000Z-0", "pre-uninstall-20200101T000000Z",
                                                "pre-restore-y", "pre-downgrade-z"})
                           if (!fs::exists(w.backups() / (std::string(stem) + ".db")))
                               fail(label, std::string("retention deleted the preserved ") + stem);
                       int automatic = 0;
                       for (int g = 1; g <= 13; ++g)
                           if (fs::exists(w.backups() / ("pre-" + w.a.entry.id().dir_name() + "-g" + std::to_string(g) + ".db"))) {
                               ++automatic;
                               if (g <= 3) fail(label, "an older automatic snapshot outlived a newer one");
                           }
                       if (fs::exists(w.backups() / "pre-migration-v1-20200101T000000Z.db"))
                           fail(label, "the oldest automatic snapshot was kept");
                       if (automatic != 10) fail(label, "retention did not keep exactly the 10 newest automatic snapshots");
                   },
                   false, false});

    out.push_back({"kill-during-download", "", 1, 1, true,
                   [](World& w) {
                       const std::string app = stalled_app_file(w);
                       w.server->stall(app);
                       auto started = std::chrono::steady_clock::now();
                       RunResult capped = run_launcher(w, {{"PARTIES_TEST_TIMING", kTiming},
                                                           {"PARTIES_FAKE_RUN_MS", "700"},
                                                           {"PARTIES_TEST_JOB_CAP_MS", "1000"}});
                       if (capped.exit_code != 0 || std::chrono::steady_clock::now() - started > 10s ||
                           w.last_run_log().find("giving up on the update job") == std::string::npos)
                           fail(w.name, "the cap did not end a stalled download after the app exited");
                       w.server->stall(app);
                       RunResult killed = run_launcher(w, {{"PARTIES_TEST_TIMING", kTiming}, {"PARTIES_FAKE_RUN_MS", "20000"}},
                                                       {}, 60000, {}, [&] { return w.server->stalled(); });
                       if (killed.exit_code != kKilled) fail(w.name, "the launcher was not killed mid-download");
                       if (fs::exists(w.build_dir(w.b))) fail(w.name, "a half-downloaded build was sealed");
                       if (!w.leftovers()) fail(w.name, "the kill did not land mid-download");
                       if (w.statuses().find("\"phase\":\"downloading\",\"channel\":\"stable\",\"current\":\"1.0.0.0\","
                                             "\"available\":\"2.0.0.0\"") == std::string::npos)
                           fail(w.name, "the app was not told which version is downloading");
                       w.server->release();
                   },
                   good_is_b,
                   [](const World& w, const std::string& label) {
                       if (w.leftovers()) fail(label, "the interrupted download was never cleaned up");
                   },
                   true, false});

    out.push_back({"download-after-exit", "", 1, 1, true,
                   [](World& w) {
                       std::optional<std::chrono::steady_clock::time_point> released;
                       RunResult r = run_short_session(w, [&] {
                           if (released) return;
                           w.server->release();
                           released = std::chrono::steady_clock::now();
                       });
                       if (!released) return fail(w.name, "the launcher exited with the app while B was downloading");
                       if (r.timed_out || r.exit_code != 0 || std::chrono::steady_clock::now() - *released > 10s)
                           fail(w.name, "the launcher did not exit on its own once the download finished");
                       auto s = w.state();
                       if (!s || !staged(*s, w.b)) fail(w.name, "B was not staged after the app exited");
                       std::string log = w.last_run_log();
                       size_t exited = log.find("app exited"), sealed = log.find("sealed " + w.b.entry.id().dir_name());
                       if (sealed == std::string::npos || sealed < exited)
                           fail(w.name, "B was not sealed after the app exited");
                   },
                   good_is_b,
                   [](const World& w, const std::string& label) {
                       if (markers(w.db(), "2.0.0.0") < 1) fail(label, "B never ran");
                       if (w.leftovers()) fail(label, "a partial download or staging directory was left behind");
                   },
                   true, false});

    out.push_back({"relaunch-during-download", "", 1, 1, true,
                   [](World& w) {
                       const size_t mark = w.full_log().size();
                       auto since = [&](std::string_view what) {
                           std::string log = w.full_log().substr(mark);
                           size_t n = 0;
                           for (size_t at = log.find(what); at != std::string::npos; at = log.find(what, at + 1)) ++n;
                           return n;
                       };
                       std::optional<RunResult> relaunch;
                       bool released = false;
                       RunResult r = run_short_session(w, [&] {
                           if (!relaunch) relaunch = run_launcher(w, {{"PARTIES_TEST_TIMING", kTiming}});
                           else if (!released && since("app exited") >= 2) {
                               w.server->release();
                               released = true;
                           }
                       });
                       if (!relaunch) return fail(w.name, "the launcher exited with the app while B was downloading");
                       if (relaunch->exit_code != 0 || !relaunch->boot_ok)
                           fail(w.name, "the relaunch was not handed to the running launcher");
                       if (!released) fail(w.name, "the relaunch did not start the app");
                       if (since("another launcher is running") != 1)
                           fail(w.name, "the relaunch did not find the running launcher");
                       if (r.timed_out || r.exit_code != 0) fail(w.name, "the launcher did not exit on its own");
                       if (markers(w.db(), "1.0.0.0") != 2) fail(w.name, "A did not run once per launch");
                       auto s = w.state();
                       if (!s || !staged(*s, w.b)) fail(w.name, "B was not staged after the relaunched app exited");
                   },
                   good_is_b,
                   [](const World& w, const std::string& label) {
                       if (markers(w.db(), "2.0.0.0") < 1) fail(label, "B never ran");
                   },
                   true, false});

    out.push_back({"shutdown-during-download", "", 1, 1, true,
                   [](World& w) {
                       std::optional<std::chrono::steady_clock::time_point> ended;
                       RunResult r = run_short_session(w, [&] {
                           if (ended) return;
                           if (!end_session(w)) fail(w.name, "cannot reach the launcher's session window");
                           ended = std::chrono::steady_clock::now();
                       });
                       if (!ended) return fail(w.name, "the launcher exited with the app while B was downloading");
                       if (r.timed_out || r.exit_code != 0 || std::chrono::steady_clock::now() - *ended > 10s)
                           fail(w.name, "session end did not stop the download promptly");
                       auto session = w.session();
                       if (!session || !session->ended_clean) fail(w.name, "session end did not leave a clean report");
                       auto s = w.state();
                       if (!s || s->find(w.b.entry.id()) || fs::exists(w.build_dir(w.b)))
                           fail(w.name, "a half-downloaded build reached the state");
                       w.server->release();
                   },
                   good_is_b,
                   [](const World& w, const std::string& label) {
                       if (w.leftovers()) fail(label, "the interrupted download was never cleaned up");
                   },
                   true, false});
    return out;
}

int smoke_real_client(const fs::path& client) {
    const std::string name = "real-client";
    World w(name, "2.0.0.0", 1, 1, false);
    fs::remove(w.source / "stable.manifest.json");
    fs::path dev_db = client.parent_path() / "parties_client.db";
    auto dev_before = digest_file(dev_db);

    Release r = make_release(w.root.path() / "real", client, "0.0.0.1",
                             static_cast<uint32_t>(parties::client::schema::kCurrent.version),
                             static_cast<uint32_t>(parties::client::schema::kCurrent.compat_floor), false);
    fs::path dir = install(w.install_root, r);
    RunResult result = run_launcher(w, {{"PARTIES_TEST_EXIT_AFTER_HEALTHY_MS", "3000"}}, {L"--allow-multiple-instances"},
                                    180000, dir / kLauncherFile);
    std::string log = read_file(w.log_file()).value_or("");
    if (result.timed_out || result.exit_code != 0) fail(name, "the launcher did not exit cleanly");
    if (!result.boot_ok) fail(name, "boot_ok was not signalled");
    if (log.find("app reported healthy") == std::string::npos) fail(name, "the real client never reported healthy");
    if (log.find("app exited with code 0x0") == std::string::npos) fail(name, "the real client did not exit cleanly");
    auto s = w.state();
    if (!s || s->good != r.entry.id() || !settled(*s)) fail(name, "unexpected supervisor state");
    if (!fs::exists(w.db())) fail(name, "the client did not use the launcher's data directory");
    if (digest_file(dev_db) != dev_before) fail(name, "the developer database next to the build changed");
    std::istringstream lines(log);
    for (std::string line; std::getline(lines, line);)
        if (line.find("adopted") != std::string::npos || line.find("started") != std::string::npos ||
            line.find("healthy") != std::string::npos || line.find("exit") != std::string::npos)
            std::cout << "  " << line << "\n";
    std::cout << "  data dir: " << w.db().string() << (fs::exists(w.db()) ? " (created)" : " (missing)") << "\n";
    std::cout << "real-client smoke: " << (g_failures ? "FAILED" : "healthy, clean exit, dev data untouched") << "\n";
    if (g_failures) std::cout << log;
    return g_failures ? 1 : 0;
}

} // namespace

int main(int argc, char** argv) {
    std::optional<std::string> only;
    std::optional<fs::path> real_client;
    bool kill_points = true;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--launcher" && i + 1 < argc) g_launcher = argv[++i];
        else if (a == "--fake-app" && i + 1 < argc) g_fake_app = argv[++i];
        else if (a == "--manifest-tool" && i + 1 < argc) g_manifest_tool = argv[++i];
        else if (a == "--real-client" && i + 1 < argc) real_client = argv[++i];
        else if (a == "--only" && i + 1 < argc) only = argv[++i];
        else if (a == "--no-kill") kill_points = false;
        else if (a == "--verbose") g_verbose = true;
    }
    if (!parties::crypto_init()) return 2;
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 2;
    if (g_launcher.empty() || g_fake_app.empty() || g_manifest_tool.empty()) {
        std::cerr << "usage: update_e2e_test --launcher <exe> --fake-app <exe> --manifest-tool <exe> [--only <name>] "
                     "[--no-kill]\n";
        return 2;
    }
    if (real_client) return smoke_real_client(*real_client);

    std::printf("%-24s %-6s %s\n", "scenario", "plain", "kill points");
    int total_kills = 0;
    for (const Scenario& sc : scenarios()) {
        if (only && *only != sc.name) continue;
        auto started = std::chrono::steady_clock::now();
        int before = g_failures;
        bool plain = run_scenario(sc, std::nullopt) == Outcome::Converged && g_failures == before;
        int kills = 0;
        bool kills_ok = true;
        if (kill_points && sc.kill_points && plain) {
            for (int n = 1; n < 200; ++n) {
                int b = g_failures;
                Outcome o = run_scenario(sc, n);
                if (o == Outcome::KillNotReached) break;
                ++kills;
                if (o != Outcome::Converged || g_failures != b) kills_ok = false;
            }
        }
        total_kills += kills;
        auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        std::printf("%-24s %-6s %s (%d) %.0fs\n", sc.name.c_str(), plain ? "pass" : "FAIL",
                    !kill_points || !sc.kill_points ? "skipped" : kills_ok && plain ? "pass" : "FAIL", kills, seconds);
        std::fflush(stdout);
    }
    std::printf("kill points exercised: %d\n", total_kills);
    return g_failures ? 1 : 0;
}
