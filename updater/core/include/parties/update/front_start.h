#pragma once

// Pure decisions of the macOS host before it loads a payload
// (process-contract.md, "Front start"): where the bundle runs from, whether
// its seed build still has to be installed, what a front does when no
// launcher answers, and what each alert the host draws says. The host
// gathers the facts and acts; boot::choose picks the launcher.

#include <parties/update/boot.h>
#include <parties/update/front_door.h>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace parties::update::front_start {

// The host's command line (process-contract.md, "Launcher argv").
struct HostArgs {
    std::string install_root, data_root;  // absolute
    bool launcher = false;                // --role=launcher
    std::string build;                    // --build=builds\<dir>
    std::optional<std::string> install_seed;  // --install-seed=<this Parties.app>
    bool ticketed = false;                // --ticket=<n>: a front a launcher started
    bool recover = false;                 // --recover
    bool safe_mode = false;               // --safe-mode: the last good app, unsupervised
    int boot_round = 0;                   // --boot-round=<k>: launchers lost before boot_ok so far
    int32_t after = 0;                    // --after=<pid>: the front this one replaces
    int boot_wait_ms = 300'000;           // how long a launcher has to listen, and then to answer
    std::vector<std::string> forwarded;   // after "--"; every other unknown flag is dropped
    // Testing builds only.
    std::string test_namespace;
    std::vector<std::string> testing;  // roots, namespace, logs and feed, passed to every process this one starts
    std::optional<std::string> self_test, fake_launcher, log_dir;
};

// args excludes argv[0]. The launcher role takes the roots its booting front
// passes, and a front takes them only in testing builds.
HostArgs parse_host_args(std::span<const char* const> args, bool testing, std::string default_install_root,
                         std::string default_data_root);

// What a front that replaces this one is started with: extra, then --after
// and the testing flags, then the user's arguments after "--".
std::vector<std::string> relaunch_arguments(const HostArgs& args, std::vector<std::string> extra, int32_t pid);
// A --role=launcher process: the roots, role_args, then the testing flags.
std::vector<std::string> launcher_arguments(const HostArgs& args, std::string executable,
                                            const std::vector<std::string>& role_args);

// The launcher digest the contract pins for a build: only the good and the
// previous good build's launchers ever run.
std::optional<Sha256> pinned_launcher(const boot::BootContract& contract, std::string_view build);

inline constexpr int kMaxReChooses = 2;

// Parties.exe's loop over boot::choose (updater/boot/main.cpp): a launcher
// whose file cannot be used is passed over without counting a failure, and
// after kMaxReChooses failed boots in one start the host shows safe mode.
class LauncherChoice {
public:
    LauncherChoice(std::optional<boot::BootContract> contract, boot::BootRecord record,
                   std::optional<boot::SessionReport> session);
    // The launcher to start, or nullopt for safe mode. Persist record() before acting on either.
    std::optional<boot::RunLauncher> next();
    void unusable();  // the last one's file does not match: choose again from the same inputs
    void failed();    // the last one ended or stalled before it listened
    const boot::BootRecord& record() const { return next_record_; }

private:
    std::optional<boot::BootContract> contract_;
    boot::BootRecord record_, next_record_;
    std::optional<boot::SessionReport> session_;
    std::vector<Sha256> unusable_;
    std::optional<Sha256> last_;
    int failures_ = 0;
};

enum class Placement : uint8_t {
    Installed,
    ReadOnlyVolume,  // statfs MNT_RDONLY: a mounted disk image
    Translocated,    // App Translocation's randomized read-only mount
};

// Sparkle's heuristic (SUHost.m#L174-L189): there is no public API for it.
Placement classify_placement(std::string_view bundle_path, bool volume_read_only);

// state/host.json. The host is its only writer.
struct HostRecord {
    std::string seed;  // the seed (SHA-256 of its build.json) install_failures counts for
    uint8_t install_failures = 0;
    bool move_declined = false;
    friend bool operator==(const HostRecord&, const HostRecord&) = default;
};

std::optional<HostRecord> parse_host_record(std::string_view text);
std::string write_host_record(const HostRecord& record);

// "entry_sha256" of state/seed.json, which the launcher writes once it has
// installed, staged or kept a seed.
std::optional<std::string> parse_installed_seed(std::string_view text);

inline constexpr uint8_t kMaxSeedInstallFailures = 2;
inline constexpr uint8_t kMaxBootRounds = 2;

struct FrontFacts {
    bool launcher_reachable = false;
    bool has_contract = false;                  // state/boot-contract.json parses
    std::optional<std::string> bundle_seed;     // nullopt: the bundle carries no seed
    std::optional<std::string> installed_seed;  // state/seed.json
    HostRecord record;
    uint8_t boot_round = 0;                     // --boot-round=<k> after a launcher was lost
};

enum class FrontPlan : uint8_t {
    Connect,             // send front{seed_pending:false}
    ConnectSeedPending,  // send front{seed_pending:true}: the launcher answers quit_for_install
    InstallSeed,         // run this bundle's seed launcher with --install-seed, then plan again
    ChooseLauncher,      // boot::choose over the contract
    SafeMode,            // no launcher can run
};

// A seed is pending while the launcher has not processed it and fewer than
// kMaxSeedInstallFailures installs of it failed, so a broken seed never
// keeps the installed good build from starting.
FrontPlan plan_front(const FrontFacts& facts);

HostRecord record_seed_failure(HostRecord record, std::string_view seed);

// Option ids the host acts on itself. Every alert can also be closed, which
// answers kQuit.
inline constexpr std::string_view kQuit = "quit";
inline constexpr std::string_view kStartLastGood = "start_last_good";
inline constexpr std::string_view kOpenData = "open_data";
inline constexpr std::string_view kDownload = "download";
inline constexpr std::string_view kMove = "move";
inline constexpr std::string_view kKeep = "keep";
inline constexpr const char* kReleasesUrl = "https://github.com/emcifuntik/parties/releases/latest";

struct Alert {
    std::string title;
    std::string message;
    std::vector<front_door::RecoveryOption> options;  // one button each, the first is the default
};

// The launcher's options, its explanation of the reason, and Open the data folder and Quit.
Alert recovery_alert(const front_door::Recovery& recovery);
// No launcher starts: the twin of the boot stub's dialog.
Alert safe_mode_alert(bool can_start_last_good);
Alert move_alert(Placement placement);

} // namespace parties::update::front_start
