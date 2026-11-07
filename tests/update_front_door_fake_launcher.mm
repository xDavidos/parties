// update_front_door_e2e's launcher. A shell runs the host with
// --role=fake-launcher=<this dylib>, so this process, like the real launcher,
// was never started by LaunchServices. It launches fronts through NSWorkspace,
// plays every fake client scenario, and checks each ExitKind against the table
// in process-contract.md, "Exit classification".

#import <Foundation/Foundation.h>

#include "front_door.h"

#include <parties/data_lock.h>
#include <parties/front_door_socket.h>
#include <parties/host_api.h>
#include <parties/update/link.h>

#include <CommonCrypto/CommonDigest.h>
#include <signal.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace parties::update;
using namespace parties::update::launcher::macos;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

namespace {

template <class... F>
struct overloaded : F... {
    using F::operator()...;
};

struct Scenario {
    const char* name;
    ExitKind expected;
    bool healthy = false;         // reports healthy over the link before it quits
    bool corrupt = false;         // assigned with a digest its file does not have
    bool launcher_kills = false;  // hangs until TerminateApp
};

constexpr Scenario kScenarios[]{
    {"clean", ExitKind::Clean},
    {"healthy_then_quit", ExitKind::Clean, true},
    {"return", ExitKind::Clean},
    {"exit2", ExitKind::InitFailed},
    {"exit3", ExitKind::DataUnavailable},
    {"exit4", ExitKind::DataTooNew},
    {"exit7", ExitKind::Abnormal},
    {"crash", ExitKind::Abnormal},
    {"abort", ExitKind::Abnormal},
    {"stray_exit", ExitKind::Abnormal},
    {"hang", ExitKind::KilledHung, false, false, true},
    {"teardown_hang", ExitKind::Clean},
    {"corrupt", ExitKind::Clean, false, true},
};

struct Options {
    fs::path install_root, data_dir, log_dir, fake_client;
    std::string test_namespace;
    bool launcher_loss = false;
};

int failures = 0;

void expect(bool ok, const std::string& what) {
    std::printf("[e2e] %s %s\n", ok ? "PASS" : "FAIL", what.c_str());
    std::fflush(stdout);
    if (!ok) ++failures;
}

double ms_since(Clock::time_point t) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}

std::string fixed(double ms) {
    return std::to_string(long(ms + 0.5)) + " ms";
}

Sha256 file_sha256(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    Sha256 digest;
    CC_SHA256(bytes.data(), CC_LONG(bytes.size()), digest.bytes.data());
    return digest;
}

Assignment prepare(const Options& o, const std::string& scenario, bool corrupt) {
    const fs::path dir = o.install_root / "builds" / ("fake-" + scenario);
    fs::create_directories(dir);
    fs::copy_file(o.fake_client, dir / "PartiesClient.dylib", fs::copy_options::overwrite_existing);
    std::ofstream(dir / "scenario") << scenario << "\n";
    Sha256 digest = file_sha256(dir / "PartiesClient.dylib");
    if (corrupt) digest.bytes[0] ^= 0xff;
    return {"builds\\fake-" + scenario, digest, o.data_dir.string(), (o.data_dir / "backups").string()};
}

std::optional<double> lock_free_within(const fs::path& data_dir, Clock::time_point from) {
    for (;;) {
        if (DataLock::try_acquire(data_dir)) return ms_since(from);
        if (Clock::now() - from > 1s) return std::nullopt;
        std::this_thread::sleep_for(2ms);
    }
}

std::string front_log(const Options& o, int32_t pid) {
    std::ifstream in(o.log_dir / ("front-" + std::to_string(pid) + ".log"));
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

size_t count(std::string_view text, std::string_view needle) {
    size_t n = 0;
    for (size_t at = text.find(needle); at != std::string_view::npos; at = text.find(needle, at + 1)) ++n;
    return n;
}

std::string shell_quote(const std::string& s) {
    std::string out = "'";
    for (char c : s) out += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return out + "'";
}

void run_open(const std::string& bundle, const std::vector<std::string>& arguments, bool new_instance) {
    std::string command = std::string("/usr/bin/open ") + (new_instance ? "-n " : "") + shell_quote(bundle) + " --args";
    for (const auto& arg : arguments) command += " " + shell_quote(arg);
    std::printf("[e2e] $ %s\n", command.c_str());
    std::fflush(stdout);
    std::system(command.c_str());
}

size_t registrations(const char* when) {
    FILE* pipe = popen("/usr/bin/lsappinfo find bundleid=org.parties.client", "r");
    std::string out;
    char buffer[512];
    while (pipe && std::fgets(buffer, sizeof buffer, pipe)) out += buffer;
    if (pipe) pclose(pipe);
    const size_t n = count(out, "ASN:");
    std::printf("[e2e] LaunchServices registrations of org.parties.client %s: %zu %s\n", when, n, out.c_str());
    return n;
}

struct Observed {
    std::optional<int32_t> pid;
    bool loaded = false, corrupt = false, healthy = false, lock_held = false;
    std::optional<FrontExited> exit;
    Clock::time_point exit_at, kill_at;
};

// Reads events until the app front exits or `stop` says enough.
template <class Stop>
void observe(FrontDoor& door, const Options& o, Observed& seen, std::chrono::seconds limit, Stop stop) {
    const auto start = Clock::now();
    while (!seen.exit && !stop() && Clock::now() - start < limit) {
        auto event = door.next(50ms);
        if (!event) continue;
        std::visit(overloaded{
                       [&](const FrontLoaded& e) {
                           seen.loaded = true;
                           seen.pid = e.pid;
                           seen.lock_held = !DataLock::try_acquire(o.data_dir);
                       },
                       [&](const FrontCorrupt& e) {
                           seen.corrupt = true;
                           seen.pid = e.pid;
                           seen.lock_held = !DataLock::try_acquire(o.data_dir);
                       },
                       [&](const LinkLine& e) {
                           auto message = link::parse_app_message(e.line);
                           if (message && std::holds_alternative<link::Healthy>(*message)) seen.healthy = true;
                       },
                       [&](const FrontExited& e) {
                           if (!e.was_app) return;
                           seen.exit = e;
                           seen.exit_at = Clock::now();
                       },
                       [](const auto&) {},
                   },
                   *event);
    }
}

void play(FrontDoor& door, const Options& o, const Scenario& s) {
    const std::string name = s.name;
    door.deliver(prepare(o, name, s.corrupt));
    Observed seen;
    std::optional<Clock::time_point> loaded_at;
    observe(door, o, seen, 90s, [&] {
        if (seen.loaded && !loaded_at) loaded_at = Clock::now();
        if (s.launcher_kills && loaded_at && seen.kill_at == Clock::time_point{} && Clock::now() - *loaded_at > 3s) {
            seen.kill_at = Clock::now();
            expect(door.kill_app(), name + ": TerminateApp sent SIGKILL");
        }
        return false;
    });

    const std::string got = seen.exit ? exit_kind_name(seen.exit->kind) : "no exit within 90 s";
    expect(seen.exit && seen.exit->kind == s.expected,
           name + ": " + got + " (process-contract.md: " + exit_kind_name(s.expected) + ")");
    if (s.corrupt) expect(seen.corrupt && !seen.loaded, name + ": the front reported corrupt and ran nothing");
    else expect(seen.loaded, name + ": loaded");
    if (s.launcher_kills) expect(seen.lock_held, name + ": the front held data.lock while it ran");
    if (s.healthy) expect(seen.healthy, name + ": healthy arrived over the link");
    if (!seen.exit) return;
    std::printf("[e2e] %s: front pid=%d\n", s.name, seen.exit->pid);

    const bool killed = seen.kill_at != Clock::time_point{};
    if (killed)
        std::printf("[e2e] %s: death seen %s after SIGKILL\n", s.name,
                    fixed(std::chrono::duration<double, std::milli>(seen.exit_at - seen.kill_at).count()).c_str());
    const auto released = lock_free_within(o.data_dir, killed ? seen.kill_at : seen.exit_at);
    expect(released.has_value(), name + ": data.lock free " + (released ? fixed(*released) : std::string("> 1 s")) + " after " +
                                 (killed ? "SIGKILL" : "the death was seen"));
}

// Waits for a front other than the app to connect and leave.
std::optional<FrontExited> newcomer(FrontDoor& door, std::chrono::seconds limit, bool& app_died) {
    const auto start = Clock::now();
    while (Clock::now() - start < limit) {
        auto event = door.next(50ms);
        if (!event) continue;
        if (auto* e = std::get_if<FrontExited>(&*event)) {
            if (!e->was_app) return *e;
            app_died = true;
            return std::nullopt;
        }
    }
    return std::nullopt;
}

void single_instance(FrontDoor& door, const Options& o, const std::string& bundle,
                     const std::vector<std::string>& front_arguments) {
    door.deliver(prepare(o, "wait", false));
    Observed seen;
    observe(door, o, seen, 90s, [&] { return seen.loaded; });
    expect(seen.loaded && seen.pid, "wait: loaded");
    if (!seen.loaded || !seen.pid) return;
    expect(seen.lock_held, "wait: the front held data.lock while it ran");
    const int32_t owner = *seen.pid;
    expect(!front_log(o, owner).empty(),
           "the arguments reached the front: its log is in --log-dir and it found the namespaced socket");
    expect(registrations("while one front runs") == 1, "exactly one LaunchServices registration while a front runs");

    struct Attempt {
        const char* label;
        bool expect_second_front;
        std::function<void()> start;
    };
    const Attempt attempts[]{
        {"open -n", true, [&] { run_open(bundle, front_arguments, true); }},
        {"NSWorkspace createsNewApplicationInstance=YES", true, [&] { launch_front(bundle, front_arguments, true); }},
        {"open", false, [&] { run_open(bundle, front_arguments, false); }},
        {"NSWorkspace createsNewApplicationInstance=NO", false, [&] { launch_front(bundle, front_arguments, false); }},
    };
    for (const Attempt& a : attempts) {
        const size_t activations = count(front_log(o, owner), "[host] activate\n");
        a.start();
        bool app_died = false;
        const auto second = newcomer(door, a.expect_second_front ? 30s : 8s, app_died);
        expect(!app_died, std::string(a.label) + ": the first front kept running");
        if (!a.expect_second_front) {
            expect(!second, std::string(a.label) + ": LaunchServices started no second front while one runs");
            continue;
        }
        expect(second && second->kind == ExitKind::Clean,
               std::string(a.label) + ": a second front connected and exited " +
                   (second ? exit_kind_name(second->kind) : "never"));
        if (!second) continue;
        expect(front_log(o, second->pid).find("[host] activate_other") != std::string::npos,
               std::string(a.label) + ": the second front got activate_other");
        bool told = false;
        for (int i = 0; i < 100 && !told; ++i, std::this_thread::sleep_for(50ms))
            told = count(front_log(o, owner), "[host] activate\n") > activations;
        expect(told, std::string(a.label) + ": the first front was told to activate");
    }

    const auto kill_at = Clock::now();
    kill(owner, SIGKILL);
    observe(door, o, seen, 30s, [] { return false; });
    expect(seen.exit && seen.exit->kind == ExitKind::Abnormal,
           std::string("external SIGKILL (Force Quit): ") + (seen.exit ? exit_kind_name(seen.exit->kind) : "no exit"));
    if (!seen.exit) return;
    std::printf("[e2e] external SIGKILL: death seen %s after the kill\n",
                fixed(std::chrono::duration<double, std::milli>(seen.exit_at - kill_at).count()).c_str());
    const auto released = lock_free_within(o.data_dir, kill_at);
    expect(released.has_value(),
           "external SIGKILL: DataLock::try_acquire succeeds " + (released ? fixed(*released) : std::string("> 1 s")) +
               " after kill -9");
}

std::optional<std::string_view> flag(std::string_view arg, std::string_view name) {
    if (!arg.starts_with(name)) return std::nullopt;
    return arg.substr(name.size());
}

} // namespace

extern "C" const uint32_t parties_fake_launcher_min_host_abi = 1;

extern "C" int parties_fake_launcher_main(const PartiesHostApi* host) {
    Options o;
    for (int i = 0; i < host->argc; ++i) {
        const std::string_view arg = host->argv[i];
        if (auto v = flag(arg, "--install-root=")) o.install_root = *v;
        if (auto v = flag(arg, "--data-dir=")) o.data_dir = *v;
        if (auto v = flag(arg, "--log-dir=")) o.log_dir = *v;
        if (auto v = flag(arg, "--fake-client=")) o.fake_client = *v;
        if (auto v = flag(arg, "--test-namespace=")) o.test_namespace = *v;
        if (arg == "--mode=launcher-loss") o.launcher_loss = true;
    }
    const std::string bundle = host->host_bundle;
    const std::vector<std::string> front_arguments{"--test-namespace=" + o.test_namespace,
                                                   "--install-root=" + o.install_root.string(),
                                                   "--log-dir=" + o.log_dir.string()};
    std::printf("[e2e] fake launcher pid=%d, host bundle %s\n", getpid(), bundle.c_str());
    if (!o.launcher_loss) expect(registrations("before any front") == 0, "this launcher process is not registered");

    auto door = FrontDoor::listen({front_door::socket_path(o.test_namespace), bundle, front_arguments, 2000ms});
    expect(door != nullptr, "the front door listens");
    if (!door) return 1;

    if (o.launcher_loss) {
        door->deliver(prepare(o, "wait", false));
        Observed seen;
        observe(*door, o, seen, 90s, [&] { return seen.loaded; });
        if (!seen.loaded || !seen.pid) return 1;
        std::printf("[e2e] launcher-loss owner pid=%d loaded\n", *seen.pid);
        std::fflush(stdout);
        for (;;) pause();
    }

    for (const Scenario& s : kScenarios) play(*door, o, s);
    single_instance(*door, o, bundle, front_arguments);
    std::printf("[e2e] %s: %d failure(s)\n", failures ? "FAILED" : "passed", failures);
    return failures ? 1 : 0;
}
