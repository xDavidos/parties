#pragma once

// The macOS front-door protocol between a host process and the launcher
// (process-contract.md, "Front-door protocol"): newline-delimited UTF-8 JSON
// over the launcher's Unix socket, split with link::LineReader. Unknown types
// and fields are ignored. The host and the launcher both link this codec.

#include <parties/update/sha256_digest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace parties::update::front_door {

// Host to launcher. Front is the first line of a front-door connection, and
// Attach is the first line of the link connection the host hands the payload.
struct Front {
    int32_t pid = 0;
    uint32_t host_abi = 0;
    std::string host;  // absolute path of the running bundle
    bool recover = false;
    bool seed_pending = false;
    std::vector<std::string> argv;
    friend bool operator==(const Front&, const Front&) = default;
};
struct Attach {
    uint64_t ticket = 0;
    friend bool operator==(const Attach&, const Attach&) = default;
};
// The payload passed verification and its entry point is about to run.
struct Loaded {
    std::string build;  // "builds\<dir>"
    friend bool operator==(const Loaded&, const Loaded&) = default;
};
// The payload failed its digest check or is missing. Nothing ran.
struct Corrupt {
    std::string build;
    friend bool operator==(const Corrupt&, const Corrupt&) = default;
};
// Written by the host's single exit path just before _exit(code).
struct Exiting {
    int32_t code = 0;
    friend bool operator==(const Exiting&, const Exiting&) = default;
};
// The user's answer to Recovery: one of its option ids, or "quit".
struct RecoveryChoice {
    std::string id;
    friend bool operator==(const RecoveryChoice&, const RecoveryChoice&) = default;
};
using HostMessage = std::variant<Front, Attach, Loaded, Corrupt, Exiting, RecoveryChoice>;

// Launcher to host.
struct Assign {
    std::string build;  // "builds\<dir>", relative to the install root
    Sha256 app_sha256;
    std::string data_dir;
    std::string backups_dir;
    uint64_t ticket = 0;
    friend bool operator==(const Assign&, const Assign&) = default;
};
// Another front owns the session. This front exits 0.
struct ActivateOther {
    friend bool operator==(const ActivateOther&, const ActivateOther&) = default;
};
// To the front that owns the session: bring the app forward.
struct Activate {
    friend bool operator==(const Activate&, const Activate&) = default;
};
// The launcher exits next so the front can install a newer seed.
struct QuitForInstall {
    friend bool operator==(const QuitForInstall&, const QuitForInstall&) = default;
};
struct RecoveryOption {
    std::string id;
    std::string label;
    std::string detail;  // may be empty
    friend bool operator==(const RecoveryOption&, const RecoveryOption&) = default;
};
// The executor of ShowRecovery: the front shows the options, answers with
// recovery_choice and exits without loading a payload.
struct Recovery {
    std::string reason;  // "good_failed_to_start", "data_unavailable", "data_incompatible", "repairing" or "requested"
    std::string data_dir;
    std::vector<RecoveryOption> options;
    friend bool operator==(const Recovery&, const Recovery&) = default;
};
using LauncherMessage = std::variant<Assign, ActivateOther, Activate, QuitForInstall, Recovery>;

// Each writer returns one line including the trailing '\n'.
std::string write(const HostMessage& message);
std::string write(const LauncherMessage& message);

// nullopt for malformed JSON, an unknown type, or a known type with unusable
// fields. A build that is not "builds\<plain name>" is unusable.
std::optional<HostMessage> parse_host_message(std::string_view line);
std::optional<LauncherMessage> parse_launcher_message(std::string_view line);

} // namespace parties::update::front_door
