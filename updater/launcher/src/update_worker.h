#pragma once

// Runs one feed job at a time off the supervisor thread and is the sole writer of
// update\trust.json. Results come back as state machine events, so every decision
// about what runs stays in step().

#include "build_store.h"
#include "data_guard.h"
#include "executor.h"
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

class UpdateWorker {
public:
    UpdateWorker(std::unique_ptr<UpdateSource> source, Keyring keys, fs::path trust_file, BuildStore builds);
    ~UpdateWorker();

    bool available() const { return source_ != nullptr; }
    bool busy() const;
    bool start(JobRequest request);
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
