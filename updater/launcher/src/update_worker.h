#pragma once

// Runs one feed job at a time off the supervisor thread: fetch a channel
// manifest from an UpdateSource, verify it against the pinned keyring, ratchet
// update\trust.json (sole writer), decide with update_policy, and seal what the
// decision needs. The results come back as state machine events, so every
// decision about what runs stays in step().

#include "build_store.h"
#include "data_guard.h"
#include "update_source.h"

#include <parties/update/link.h>
#include <parties/update/probation.h>

#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <variant>
#include <vector>

namespace parties::update::launcher {

// Look for an update on the current channel.
struct CheckJob {
    friend bool operator==(const CheckJob&, const CheckJob&) = default;
};
// The Redownload effect: fetch `build` again from the current channel's manifest.
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
    std::vector<Backup> backups;  // newest first: where a downgrade's restore comes from
};

// What the last check found, for the status line.
struct FeedView {
    link::Phase phase = link::Phase::Idle;
    std::optional<Version> available;  // a newer build this launcher cannot stage yet
    std::optional<Version> pinned_until;
    std::optional<link::DowngradeOffer> downgrade;
};

struct JobOutcome {
    Job job;
    Channel channel;  // whose manifest the job read
    bool ok = false;
    std::vector<Event> events;
    FeedView view;
};

struct Download {
    Version version;
    uint8_t percent = 0;
};

class UpdateWorker {
public:
    // source may be null: this build has no update feed.
    UpdateWorker(std::unique_ptr<UpdateSource> source, Keyring keys, fs::path trust_file, BuildStore builds);
    ~UpdateWorker();

    bool available() const { return source_ != nullptr; }
    bool busy() const;
    // Starts a job unless one is running.
    bool start(JobRequest request);
    // Signalled when a job finished or its download progressed.
    void* event() const { return event_; }
    std::optional<JobOutcome> take();
    std::optional<Download> download() const;
    // Ends the running job soon; its outcome still arrives through take().
    void cancel();
    void stop();

private:
    JobOutcome run(const JobRequest& request);
    std::expected<VerifiedManifest, link::Phase> accepted(Channel channel);
    std::expected<BuildRecord, std::string> seal(const BuildEntry& entry);

    std::unique_ptr<UpdateSource> source_;
    Keyring keys_;
    fs::path trust_file_;
    BuildStore builds_;
    void* event_;

    mutable std::mutex mutex_;
    std::thread thread_;
    bool running_ = false;
    std::optional<JobOutcome> result_;
    std::optional<Download> download_;
    std::atomic<bool> cancel_{false};
};

} // namespace parties::update::launcher
