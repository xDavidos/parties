#pragma once

// The launcher's platform surface. SupervisorCore decides what runs and in
// what order; an Executor carries out one effect or reads one fact at a time,
// and an AppSession is the platform's hold on one running app.

#include <parties/update/link.h>
#include <parties/update/probation.h>

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace parties::update::launcher {

using Clock = std::chrono::steady_clock;

struct Backup {
    SnapshotId id;
    DataInfo data;
    std::filesystem::file_time_type written;
};

struct RestoreError {
    std::string why;
    bool live_changed = false;  // false: the live database is exactly as it was
    bool data_busy = false;     // an app process still holds the data: nothing was tried, retry on the next start
};

struct LiveDbError {
    std::string why;
    bool busy = false;  // an app process still holds the data, and may still be changing it
};

struct CheckJob {
    friend bool operator==(const CheckJob&, const CheckJob&) = default;
};
struct RepairJob {
    BuildId build;
    friend bool operator==(const RepairJob&, const RepairJob&) = default;
};
// set_channel with downgrade_now. Its outcome applies only while the channel
// and intent it was asked from still hold, so a later choice withdraws it.
struct DowngradeJob {
    Channel channel;
    Channel asked_on;
    ChannelIntent asked_with;
    friend bool operator==(const DowngradeJob&, const DowngradeJob&) = default;
};
using Job = std::variant<CheckJob, RepairJob, DowngradeJob>;

struct JobRequest {
    Job job;
    SupervisorState state;
    std::optional<DataInfo> live_db;
    std::vector<Backup> backups;
};

struct FeedView {
    link::Phase phase = link::Phase::Idle;
    std::optional<Version> available;  // a newer build this launcher cannot stage yet
    std::optional<Version> pinned_until;
    std::optional<link::DowngradeOffer> downgrade;
};

struct JobOutcome {
    Job job;
    Channel channel;
    bool ok = false;
    std::vector<Event> events;
    FeedView view;
};

struct Download {
    Version version;
    uint8_t percent = 0;
};

class AppSession {
public:
    virtual ~AppSession() = default;

    // The link to the app. False: the message did not go out.
    virtual bool send(const link::LauncherMessage& message) = 0;
    // Kills the app and returns once it is gone.
    virtual void terminate() = 0;
    // Brings the running app forward for a user who started Parties again.
    virtual void activate() = 0;
};

enum class SpawnFailure : uint8_t {
    BuildCorrupt,  // a file of the build is missing or modified
    StartFailed,   // the build is intact and the app did not start
};

class Executor {
public:
    virtual ~Executor() = default;

    virtual Clock::time_point now() const = 0;
    // Runs before every effect, so a testing build can end the process at any one.
    virtual void before_effect() = 0;

    // The effects of probation.h, each idempotent.
    virtual bool commit(const SupervisorState& next, const BuildId& previous_good, std::string& error) = 0;
    virtual bool take_snapshot(const SnapshotId& id, const SupervisorState& committed, std::string& error) = 0;
    virtual std::expected<void, RestoreError> restore_snapshot(const std::optional<SnapshotId>& from,
                                                               const SnapshotId& preserve_live_as) = 0;
    virtual std::expected<std::unique_ptr<AppSession>, SpawnFailure> spawn(const BuildRecord& build) = 0;
    virtual bool delete_build(const BuildId& build, const SupervisorState& committed, std::string& error) = 0;
    virtual void signal_boot_ok() = 0;

    // The user's data, read only while no app runs.
    virtual std::expected<std::optional<DataInfo>, LiveDbError> inspect_live_db() const = 0;
    // Intact backups, newest first.
    virtual std::vector<Backup> list_backups() const = 0;

    // The update feed runs one job at a time off the loop thread. The loop
    // hands each outcome, and each progress wake-up, to SupervisorCore::on_worker.
    virtual bool feed_available() const = 0;
    virtual bool job_busy() const = 0;
    virtual bool start_job(JobRequest request) = 0;
    virtual std::optional<Download> download() const = 0;
    virtual void cancel_job() = 0;
};

} // namespace parties::update::launcher
