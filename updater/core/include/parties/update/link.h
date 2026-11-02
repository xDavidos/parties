#pragma once

// The launcher <-> app link: one named pipe carrying newline-delimited UTF-8
// JSON objects in both directions (contracts.md, "Launcher to app"). Unknown
// message types and unknown fields are ignored, so either side may add them
// without a protocol bump. Both sides link this codec; it depends on RapidJSON
// only, so the app does not pull in the rest of the update core.

#include <parties/update/link_protocol.h>
#include <parties/update/manifest.h>
#include <parties/update/version.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>

namespace parties::update::link {

// --- App to launcher ---

struct Hello {
    uint32_t link_protocol = kLinkProtocol;
    uint32_t min_launcher_link = kMinLauncherLink;
    Version version;
    friend bool operator==(const Hello&, const Hello&) = default;
};

// Sent once, after ui/lobby.rml is loaded and the first frame is presented.
struct Healthy {
    uint32_t data_schema = 1;
    friend bool operator==(const Healthy&, const Healthy&) = default;
};

enum class ChannelChoice : uint8_t { Follow, Wait, DowngradeNow };

struct RestartToUpdate { friend bool operator==(const RestartToUpdate&, const RestartToUpdate&) = default; };
struct CheckNow { friend bool operator==(const CheckNow&, const CheckNow&) = default; };
struct AcknowledgeNotice { friend bool operator==(const AcknowledgeNotice&, const AcknowledgeNotice&) = default; };
struct SetChannel {
    Channel channel = Channel::Stable;
    ChannelChoice choice = ChannelChoice::Follow;
    friend bool operator==(const SetChannel&, const SetChannel&) = default;
};
using Request = std::variant<RestartToUpdate, CheckNow, AcknowledgeNotice, SetChannel>;

using AppMessage = std::variant<Hello, Healthy, Request>;

// --- Launcher to app ---

struct Welcome {
    uint32_t link_protocol = kLinkProtocol;
    friend bool operator==(const Welcome&, const Welcome&) = default;
};

enum class Phase : uint8_t { Idle, Checking, Downloading, ReadyToRestart, Pinned, FeedStale, Error };
enum class DowngradeEffect : uint8_t { None, RestoresBackup, Impossible };

struct DowngradeOffer {
    Version version;
    DowngradeEffect data_effect = DowngradeEffect::None;
    // RestoresBackup: when the snapshot it would restore was taken.
    std::optional<std::chrono::sys_seconds> backup_taken_at;
    friend bool operator==(const DowngradeOffer&, const DowngradeOffer&) = default;
};

// Withdrawn: the version the app runs was revoked. RestoreFailed: a restore
// could not start and the data was left as it was. For both, from == to.
struct StatusNotice {
    enum class Kind : uint8_t { RolledBack, DataRestored, Withdrawn, RestoreFailed } kind = Kind::RolledBack;
    Version from;
    Version to;
    std::optional<std::string> preserved;  // backup file name
    friend bool operator==(const StatusNotice&, const StatusNotice&) = default;
};

// Sent right after welcome and whenever any field changes.
struct Status {
    Phase phase = Phase::Idle;
    Channel channel = Channel::Stable;
    Version current;
    std::optional<Version> available;
    std::optional<uint8_t> percent;
    std::optional<Version> pinned_until;
    std::optional<DowngradeOffer> downgrade;
    std::optional<StatusNotice> notice;
    friend bool operator==(const Status&, const Status&) = default;
};

// The app shuts down through its normal path and exits 0.
struct ExitRequested { friend bool operator==(const ExitRequested&, const ExitRequested&) = default; };

using LauncherMessage = std::variant<Welcome, Status, ExitRequested>;

// The protocol both sides use after hello/welcome, or nullopt when the app
// cannot talk to this launcher (welcome's K below the app's minimum).
std::optional<uint32_t> negotiate(const Hello& hello, uint32_t launcher_protocol);

// Each writer returns one line including the trailing '\n'.
std::string write(const AppMessage& message);
std::string write(const LauncherMessage& message);

// nullopt for malformed JSON, a missing "type", an unknown type, or a known
// type whose fields are unusable. Callers skip such lines.
std::optional<AppMessage> parse_app_message(std::string_view line);
std::optional<LauncherMessage> parse_launcher_message(std::string_view line);

// Splits a byte stream into lines. A line longer than kMaxLine is dropped
// whole, so a misbehaving peer cannot make the reader grow without bound.
class LineReader {
public:
    static constexpr size_t kMaxLine = 64 * 1024;

    void append(std::span<const char> bytes);
    std::optional<std::string> next();

private:
    std::string buffer_;
    size_t partial_ = 0;  // bytes after the last '\n' in buffer_
    bool discarding_ = false;
};

} // namespace parties::update::link
