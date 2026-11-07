#pragma once

// Pure decisions and file shapes for the boot stub (Parties.exe). This header
// and its library must stay free of OpenSSL and every other dependency except
// RapidJSON, because the stub links it and is never auto-updated.
//
// Files, all under the install root's state\ directory. Their shapes are
// frozen: fields may be added, never renamed or removed. Unknown fields and
// the "format" value are ignored on read; anything unreadable is "absent".
//   boot-contract.json     writer: launcher (derived on every commit)
//   launcher-session.json  writer: launcher (ended_clean=false at start, true at orderly exit)
//   boot-record.json       writer: stub only

#include <parties/update/sha256_digest.h>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace parties::update::boot {

inline constexpr uint8_t kMaxFailedBoots = 2;
inline constexpr uint8_t kCleanSessionsToConfirm = 2;

bool is_build_dir(std::string_view dir);

// dir is relative to the install root: "builds\<version>-<sha8>".
struct LauncherRef {
    std::string dir;
    Sha256 sha256;  // of PartiesLauncher.exe
    friend bool operator==(const LauncherRef&, const LauncherRef&) = default;
};

struct AppRef {
    std::string dir;
    Sha256 sha256;  // of parties_client.exe
    friend bool operator==(const AppRef&, const AppRef&) = default;
};

struct BootContract {
    LauncherRef good;
    std::optional<LauncherRef> previous_good;
    AppRef last_good_app;  // what stub safe mode runs when no launcher boots
    std::string data_dir;  // absolute
    friend bool operator==(const BootContract&, const BootContract&) = default;
};

// The launcher echoes the session id the stub passed as --boot-session=<id>.
struct SessionReport {
    uint64_t session = 0;
    Sha256 launcher;
    bool ended_clean = false;
    friend bool operator==(const SessionReport&, const SessionReport&) = default;
};

struct LauncherHealth {
    Sha256 launcher;
    uint8_t failed_boots = 0;  // failed boots and unclean sessions while unconfirmed
    uint8_t clean_sessions = 0;
    bool confirmed = false;
    friend bool operator==(const LauncherHealth&, const LauncherHealth&) = default;
};

// A launch whose session end has not been observed yet.
struct Attempt {
    uint64_t session = 0;
    Sha256 launcher;
    friend bool operator==(const Attempt&, const Attempt&) = default;
};

struct BootRecord {
    uint64_t last_session = 0;
    std::optional<Attempt> attempt;
    std::vector<LauncherHealth> launchers;
    friend bool operator==(const BootRecord&, const BootRecord&) = default;
};

struct RunLauncher {
    std::string dir;
    Sha256 sha256;
    uint64_t session = 0;  // pass as --boot-session=<id>
    friend bool operator==(const RunLauncher&, const RunLauncher&) = default;
};
struct ShowStubRecovery {
    friend bool operator==(const ShowStubRecovery&, const ShowStubRecovery&) = default;
};
using BootChoice = std::variant<RunLauncher, ShowStubRecovery>;

struct Decision {
    BootChoice choice;
    BootRecord next_record;  // persist before acting on choice
};

// Called once at stub start with the launcher's session report, then again
// with nullopt after record_boot_failure. Folds the previous attempt's outcome
// (a clean session counts toward confirmation, anything else is a failure
// while unconfirmed), then runs good's launcher if eligible, else
// previous_good's, else stub recovery. Eligible: confirmed, or fewer than
// kMaxFailedBoots failures, and not listed in `unusable` (hash mismatch or
// missing file found by the stub this run).
// The stub must not call this while a launcher instance is alive, or that
// live session would be folded as a failure.
Decision choose(const std::optional<BootContract>& contract, const BootRecord& record,
                const std::optional<SessionReport>& session,
                std::span<const Sha256> unusable = {});

// The launcher chosen by the last choose() exited or timed out before
// signalling boot_ok.
BootRecord record_boot_failure(const BootRecord& record);

std::optional<BootContract> parse_boot_contract(std::string_view text);
std::string write_boot_contract(const BootContract& contract);
std::optional<BootRecord> parse_boot_record(std::string_view text);
std::string write_boot_record(const BootRecord& record);
std::optional<SessionReport> parse_session_report(std::string_view text);
std::string write_session_report(const SessionReport& report);

} // namespace parties::update::boot
