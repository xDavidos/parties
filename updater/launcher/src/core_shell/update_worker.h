#pragma once

// Runs one feed job at a time off the supervisor thread and is the sole writer of
// update/trust.json. Results come back as state machine events, so every decision
// about what runs stays in step().

#include "executor.h"
#include "update_source.h"

#include <parties/update/link.h>
#include <parties/update/probation.h>
#include <parties/update/product.h>
#include <parties/update/update_policy.h>

#include <atomic>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <variant>
#include <vector>

namespace parties::update::launcher {

// The platform's BuildStore::seal: fetches, verifies and seals one entry.
using Sealer = std::function<std::expected<BuildRecord, std::string>(
    const BuildEntry& entry, UpdateSource& source, const Progress& progress, const std::atomic<bool>& cancel)>;

struct WorkerConfig {
    const ProductSpec& product;
    Runner runner;
    Keyring keys;
    std::filesystem::path trust_file;
    Sealer seal;
    // Called from the worker thread when an outcome or a progress change is
    // ready; the loop then calls take().
    std::function<void()> wake;
};

class UpdateWorker {
public:
    UpdateWorker(std::unique_ptr<UpdateSource> source, WorkerConfig config);
    ~UpdateWorker();

    bool available() const { return source_ != nullptr; }
    bool busy() const;
    bool start(JobRequest request);
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
    WorkerConfig config_;

    mutable std::mutex mutex_;
    std::thread thread_;
    bool running_ = false;
    std::optional<JobOutcome> result_;
    std::optional<Download> download_;
    std::atomic<bool> cancel_{false};
};

} // namespace parties::update::launcher
