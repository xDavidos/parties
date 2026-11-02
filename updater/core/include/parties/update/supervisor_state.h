#pragma once

// Domain types for the build state machine. Persisted only by the launcher's
// state store (state\supervisor.json) through state_codec.h.

#include <parties/update/boot.h>
#include <parties/update/data_compat.h>
#include <parties/update/manifest.h>
#include <parties/update/version.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace parties::update {

// Why a build is quarantined. Quarantine is lifted only by a newer build in
// the manifest or an explicit retry from recovery.
enum class FailureReason : uint8_t {
    CrashedBeforeHealthy,
    InitFailed,
    HungBeforeHealthy,
    TerminatedBeforeHealthy,
    DataTooNew,
    CrashedRepeatedlyAfterHealthy,
    UnknownOutcomeRepeated,
    Corrupt,
    Revoked,
    UserRolledBack,
};

struct Unproven { friend bool operator==(const Unproven&, const Unproven&) = default; };
struct Proven { friend bool operator==(const Proven&, const Proven&) = default; };
struct Quarantined {
    FailureReason reason;
    friend bool operator==(const Quarantined&, const Quarantined&) = default;
};
using Verdict = std::variant<Unproven, Proven, Quarantined>;

// One sealed directory under builds\. The data and link fields are copied from
// the signed manifest entry at seal time, so decisions never need the network.
struct BuildRecord {
    BuildId id;
    uint32_t data_schema = 1;
    uint32_t data_compat_floor = 1;
    uint32_t link_protocol = 1;
    uint32_t min_supported_launcher_link = 1;
    Sha256 launcher_sha256;
    Sha256 app_sha256;
    Verdict verdict = Unproven{};

    static BuildRecord from_entry(const BuildEntry& entry);
    bool quarantined() const { return std::holds_alternative<Quarantined>(verdict); }
    friend bool operator==(const BuildRecord&, const BuildRecord&) = default;
};

// A database copy under backups\<stem>.db.
struct SnapshotId {
    std::string stem;
    friend bool operator==(const SnapshotId&, const SnapshotId&) = default;
};
struct Snapshot {
    SnapshotId id;
    DataInfo data;  // what the copy says about itself
    friend bool operator==(const Snapshot&, const Snapshot&) = default;
};

// --- What the build selection is doing. Exactly one holds. ---

// Running `good`. Free to stage an update.
struct Steady { friend bool operator==(const Steady&, const Steady&) = default; };

// A sealed build waiting for the next app start. restore_first is an explicit
// user choice to put a dated backup in place before it runs (a downgrade the
// live data cannot support).
struct Staged {
    BuildId build;
    std::optional<Snapshot> restore_first;
    friend bool operator==(const Staged&, const Staged&) = default;
};

// The candidate is being proven while `fallback` (== good) stays selected.
struct Probation {
    BuildId candidate;
    BuildId fallback;
    std::optional<Snapshot> pre_snapshot;  // live data before the candidate first ran
    uint8_t failed_runs = 0;               // runs after health that ended badly or unobserved
    bool restore_on_rollback = false;      // the candidate started on restored data
    friend bool operator==(const Probation&, const Probation&) = default;
};

using Activation = std::variant<Steady, Staged, Probation>;

// --- What the launcher is doing with a process or the data. Exactly one holds. ---

struct Idle { friend bool operator==(const Idle&, const Idle&) = default; };

// Committed before every spawn, so a launcher that dies mid-run finds it on
// Resumed and counts an unobserved candidate run as a failure.
struct Running {
    BuildId build;
    bool healthy = false;
    uint32_t healthy_uptime_s = 0;
    friend bool operator==(const Running&, const Running&) = default;
};

// Committed before a data restore, so a crash mid-restore resumes the restore
// and nothing runs on half-restored data.
struct Restoring {
    Snapshot from;
    SnapshotId preserve_live_as;
    friend bool operator==(const Restoring&, const Restoring&) = default;
};

using Operation = std::variant<Idle, Running, Restoring>;

struct Follow { friend bool operator==(const Follow&, const Follow&) = default; };
// The answer to "switched to a channel whose newest build is older than mine".
struct PinnedUntilAtLeast {
    Version floor;
    friend bool operator==(const PinnedUntilAtLeast&, const PinnedUntilAtLeast&) = default;
};
using ChannelIntent = std::variant<Follow, PinnedUntilAtLeast>;

// Shown once by the app, then acknowledged.
struct Notice {
    enum class Kind : uint8_t { RolledBack, DataRestored } kind;
    Version from;
    Version to;
    std::optional<SnapshotId> preserved;  // where the replaced live data was kept
    friend bool operator==(const Notice&, const Notice&) = default;
};

struct SupervisorState {
    uint64_t generation = 0;  // +1 per commit
    Channel channel = Channel::Stable;
    ChannelIntent intent = Follow{};
    BuildId good;  // always a Proven record
    std::optional<BuildId> previous_good;
    std::vector<BuildRecord> builds;  // good, previous_good, staged, candidate, kept quarantines
    std::optional<Notice> notice;
    Activation activation = Steady{};
    Operation operation = Idle{};

    const BuildRecord* find(const BuildId& id) const;
    friend bool operator==(const SupervisorState&, const SupervisorState&) = default;
};

// The first state after install: `installed` becomes the proven good build.
SupervisorState initial_state(const BuildRecord& installed, Channel channel);

// "builds\<dir_name>", relative to the install root.
std::string build_dir(const BuildId& id);

// What the boot stub reads, derived on every commit and never edited apart.
boot::BootContract derive_boot_contract(const SupervisorState& state, std::string_view data_dir);

} // namespace parties::update
