#include "update_worker.h"

#include "file_io.h"
#include "log.h"

#include <parties/update/state_codec.h>
#include <parties/update/update_policy.h>

#include <algorithm>
#include <chrono>

#include <windows.h>

namespace parties::update::launcher {

namespace {

link::DowngradeEffect wire_effect(DataEffect effect) {
    switch (effect) {
    case DataEffect::ReadsAsIs:
    case DataEffect::MigratesForward: return link::DowngradeEffect::None;
    case DataEffect::NeedsRestore: return link::DowngradeEffect::RestoresBackup;
    }
    return link::DowngradeEffect::Impossible;
}

std::vector<Snapshot> snapshots(const std::vector<Backup>& backups) {
    std::vector<Snapshot> out;
    for (const Backup& b : backups) out.push_back({b.id, b.data});
    return out;
}

// A downgrade that needs a backup nobody has taken is impossible.
link::DowngradeOffer wire_offer(const DowngradeOffer& offer, const std::vector<Backup>& backups) {
    link::DowngradeOffer out{offer.entry.version, wire_effect(offer.data_effect), std::nullopt};
    if (offer.data_effect != DataEffect::NeedsRestore) return out;
    auto point = restore_point(offer.entry.data_schema, snapshots(backups));
    auto it = point ? std::ranges::find(backups, point->id, &Backup::id) : backups.end();
    if (it == backups.end()) {
        out.data_effect = link::DowngradeEffect::Impossible;
        return out;
    }
    out.backup_taken_at =
        std::chrono::floor<std::chrono::seconds>(std::chrono::clock_cast<std::chrono::system_clock>(it->written));
    return out;
}

} // namespace

UpdateWorker::UpdateWorker(std::unique_ptr<UpdateSource> source, Keyring keys, fs::path trust_file,
                           BuildStore builds)
    : source_(std::move(source)), keys_(keys), trust_file_(std::move(trust_file)), builds_(std::move(builds)),
      event_(CreateEventW(nullptr, FALSE, FALSE, nullptr)) {}

UpdateWorker::~UpdateWorker() {
    stop();
    CloseHandle(event_);
}

void UpdateWorker::cancel() {
    cancel_ = true;
    if (source_) source_->abort();
}

void UpdateWorker::stop() {
    cancel();
    if (thread_.joinable()) thread_.join();
}

bool UpdateWorker::busy() const {
    std::lock_guard lock(mutex_);
    return running_;
}

bool UpdateWorker::start(JobRequest request) {
    if (!source_) return false;
    {
        std::lock_guard lock(mutex_);
        if (running_) return false;
        running_ = true;
        download_.reset();
    }
    if (thread_.joinable()) thread_.join();
    cancel_ = false;
    thread_ = std::thread([this, request = std::move(request)] {
        JobOutcome outcome = run(request);
        {
            std::lock_guard lock(mutex_);
            result_ = std::move(outcome);
            download_.reset();
            running_ = false;
        }
        SetEvent(event_);
    });
    return true;
}

std::optional<JobOutcome> UpdateWorker::take() {
    std::lock_guard lock(mutex_);
    return std::exchange(result_, std::nullopt);
}

std::optional<Download> UpdateWorker::download() const {
    std::lock_guard lock(mutex_);
    return download_;
}

std::expected<VerifiedManifest, link::Phase> UpdateWorker::accepted(Channel channel) {
    auto error = [](std::string why) {
        log("update check failed: {}", why);
        return std::unexpected(link::Phase::Error);
    };
    auto bytes = source_->fetch_manifest(channel, cancel_);
    if (!bytes) return error(bytes.error().detail);
    auto manifest = verify_manifest(*bytes, keys_, channel);
    if (!manifest) return error("manifest rejected (" + std::to_string(static_cast<int>(manifest.error().kind)) +
                                "): " + manifest.error().detail);

    TrustFile trust;
    if (auto text = read_file(trust_file_)) {
        auto parsed = parse_trust(*text);
        if (!parsed) {
            // A newer launcher's ratchet must not be replaced by ours.
            if (parsed.error() == StateParseError::FutureSchema) return error("trust.json is from a newer launcher");
            log("trust.json is unreadable; starting a new ratchet");
        } else {
            trust = *parsed;
        }
    }
    auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    TrustDecision decision = accept(trust.of(channel), *manifest, now);
    if (auto* rejected = std::get_if<Rejected>(&decision)) {
        if (rejected->reason == TrustRejection::Stale) {
            log("update feed is stale");
            return std::unexpected(link::Phase::FeedStale);
        }
        return error("manifest replayed or equivocating (" + std::to_string(static_cast<int>(rejected->reason)) + ")");
    }
    trust.of(channel) = std::get<Accepted>(decision).next;
    std::error_code ec;
    fs::create_directories(trust_file_.parent_path(), ec);
    std::string write_error;
    if (!durable_replace(trust_file_, write_trust(trust), write_error)) return error(write_error);
    return std::move(*manifest);
}

std::expected<BuildRecord, std::string> UpdateWorker::seal(const BuildEntry& entry) {
    log("fetching {}", entry.version.to_string());
    uint64_t total = 0;
    for (const FileEntry& f : entry.files) total += f.size;
    {
        std::lock_guard lock(mutex_);
        download_ = Download{entry.version, 0};
    }
    SetEvent(event_);
    auto progress = [&](uint64_t done) {
        uint8_t pct = total ? static_cast<uint8_t>(std::min<uint64_t>(100, done * 100 / total)) : 100;
        std::lock_guard lock(mutex_);
        if (download_ && download_->percent != pct) {
            download_->percent = pct;
            SetEvent(event_);
        }
    };
    auto sealed = builds_.seal(entry, *source_, progress, cancel_);
    if (!sealed) log("fetching {} failed: {}", entry.version.to_string(), sealed.error());
    return sealed;
}

JobOutcome UpdateWorker::run(const JobRequest& request) {
    const SupervisorState& state = request.state;
    const Channel channel = std::holds_alternative<DowngradeJob>(request.job) ? std::get<DowngradeJob>(request.job).channel
                                                                              : state.channel;
    JobOutcome out{request.job, channel};
    auto manifest = accepted(channel);
    if (!manifest) {
        out.view.phase = manifest.error();
        return out;
    }

    if (auto* repair = std::get_if<RepairJob>(&request.job)) {
        auto builds = manifest->builds();
        auto it = std::ranges::find(builds, repair->build, &BuildEntry::id);
        if (it == builds.end()) {
            log("{} is not in the {} manifest, so it cannot be downloaded again", repair->build.dir_name(),
                channel_name(channel));
            return out;
        }
        auto sealed = seal(*it);
        if (!sealed) return out;
        out.events.push_back(BuildSealed{*sealed, std::nullopt});
        out.ok = true;
        return out;
    }

    if (std::holds_alternative<DowngradeJob>(request.job)) {
        ChannelSwitchPlan plan = plan_channel_switch(state, *manifest, kLinkProtocol, request.live_db);
        auto* pinned = std::get_if<StayPinned>(&plan);
        if (pinned && pinned->downgrade) {
            const DowngradeOffer& offer = *pinned->downgrade;
            std::optional<Snapshot> restore;
            if (offer.data_effect == DataEffect::NeedsRestore) restore = restore_point(offer.entry.data_schema, snapshots(request.backups));
            if (offer.data_effect != DataEffect::NeedsRestore || restore) {
                if (auto sealed = seal(offer.entry)) {
                    log("downgrading to {}{}", offer.entry.version.to_string(),
                        restore ? " on backup " + restore->id.stem : "");
                    out.events.push_back(ChannelIntentSet{channel, Follow{}});
                    out.events.push_back(UserDowngrade{*sealed, restore});
                    out.ok = true;
                    return out;
                }
                out.view.phase = link::Phase::Error;
            } else {
                log("no backup can be restored for {}, so the switch waits instead", offer.entry.version.to_string());
            }
            out.view.downgrade = wire_offer(offer, request.backups);
        }
        out.events.push_back(ChannelIntentSet{channel, intent_for(plan)});
        if (pinned) out.view.pinned_until = pinned->until;
        out.ok = true;
        return out;
    }

    out.events.push_back(ManifestSeen{channel, manifest->payload().revoked});
    UpdateDecision decision = plan_update(state, *manifest, kLinkProtocol, request.live_db);
    if (decision.new_intent) out.events.push_back(ChannelIntentSet{channel, *decision.new_intent});
    out.ok = true;
    if (auto* stage = std::get_if<StageBuild>(&decision.plan)) {
        auto sealed = seal(stage->entry);
        if (!sealed) {
            out.ok = false;
            out.view.phase = link::Phase::Error;
            return out;
        }
        if (stage->reason == StageReason::LeaveRevoked) out.events.push_back(LeaveRevoked{*sealed});
        else out.events.push_back(BuildSealed{*sealed, std::nullopt});
    } else if (auto* report = std::get_if<ReportPinned>(&decision.plan)) {
        out.view.phase = link::Phase::Pinned;
        out.view.pinned_until = report->until;
        if (report->downgrade) out.view.downgrade = wire_offer(*report->downgrade, request.backups);
    } else if (auto* offer = std::get_if<OfferDowngrade>(&decision.plan)) {
        out.view.downgrade = wire_offer(offer->offer, request.backups);
    } else if (auto* blocked = std::get_if<Blocked>(&decision.plan)) {
        out.view.available = blocked->waiting_for;
    }
    return out;
}

} // namespace parties::update::launcher
