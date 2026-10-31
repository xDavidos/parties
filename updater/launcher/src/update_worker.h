#pragma once

// Runs one update check at a time off the supervisor thread: fetch the
// channel manifest from an UpdateSource, verify it against the pinned keyring,
// ratchet update\trust.json (sole writer), plan with plan_update, and seal the
// planned build. The results come back as state machine events, so every
// decision about what runs stays in step().

#include "build_store.h"
#include "update_source.h"

#include <parties/update/link.h>
#include <parties/update/probation.h>

#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace parties::update::launcher {

struct CheckRequest {
    SupervisorState state;
    std::optional<DataInfo> live_db;
};

// What the last check found, for the status line.
struct FeedView {
    link::Phase phase = link::Phase::Idle;
    std::optional<Version> pinned_until;
    std::optional<link::DowngradeOffer> downgrade;
};

struct CheckOutcome {
    std::vector<Event> events;
    FeedView view;
};

class UpdateWorker {
public:
    // source may be null: this build has no update feed.
    UpdateWorker(std::unique_ptr<UpdateSource> source, Keyring keys, fs::path trust_file, BuildStore builds);
    ~UpdateWorker();

    bool available() const { return source_ != nullptr; }
    bool busy() const;
    // Starts a check unless one is running.
    bool start(CheckRequest request);
    // Signalled when a check finished or its progress changed.
    void* event() const { return event_; }
    std::optional<CheckOutcome> take();
    std::optional<uint8_t> percent() const;
    void stop();

private:
    CheckOutcome run(const CheckRequest& request);

    std::unique_ptr<UpdateSource> source_;
    Keyring keys_;
    fs::path trust_file_;
    BuildStore builds_;
    void* event_;

    mutable std::mutex mutex_;
    std::thread thread_;
    bool running_ = false;
    std::optional<CheckOutcome> result_;
    std::optional<uint8_t> percent_;
    std::atomic<bool> cancel_{false};
};

} // namespace parties::update::launcher
