#include "update_worker.h"

#include "file_io.h"
#include "log.h"

#include <parties/update/state_codec.h>
#include <parties/update/update_policy.h>

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

} // namespace

UpdateWorker::UpdateWorker(std::unique_ptr<UpdateSource> source, Keyring keys, fs::path trust_file,
                           BuildStore builds)
    : source_(std::move(source)), keys_(keys), trust_file_(std::move(trust_file)), builds_(std::move(builds)),
      event_(CreateEventW(nullptr, FALSE, FALSE, nullptr)) {}

UpdateWorker::~UpdateWorker() {
    stop();
    CloseHandle(event_);
}

void UpdateWorker::stop() {
    cancel_ = true;
    if (thread_.joinable()) thread_.join();
    cancel_ = false;
}

bool UpdateWorker::busy() const {
    std::lock_guard lock(mutex_);
    return running_;
}

bool UpdateWorker::start(CheckRequest request) {
    if (!source_) return false;
    {
        std::lock_guard lock(mutex_);
        if (running_) return false;
        running_ = true;
        percent_.reset();
    }
    if (thread_.joinable()) thread_.join();
    thread_ = std::thread([this, request = std::move(request)] {
        CheckOutcome outcome = run(request);
        {
            std::lock_guard lock(mutex_);
            result_ = std::move(outcome);
            running_ = false;
        }
        SetEvent(event_);
    });
    return true;
}

std::optional<CheckOutcome> UpdateWorker::take() {
    std::lock_guard lock(mutex_);
    return std::exchange(result_, std::nullopt);
}

std::optional<uint8_t> UpdateWorker::percent() const {
    std::lock_guard lock(mutex_);
    return percent_;
}

CheckOutcome UpdateWorker::run(const CheckRequest& request) {
    CheckOutcome out;
    const Channel channel = request.state.channel;
    auto error = [&](std::string why) {
        log("update check failed: {}", why);
        out.view.phase = link::Phase::Error;
        return out;
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
            out.view.phase = link::Phase::FeedStale;
            return out;
        }
        return error("manifest replayed or equivocating (" + std::to_string(static_cast<int>(rejected->reason)) + ")");
    }
    trust.of(channel) = std::get<Accepted>(decision).next;
    std::error_code ec;
    fs::create_directories(trust_file_.parent_path(), ec);
    std::string write_error;
    if (!durable_replace(trust_file_, write_trust(trust), write_error)) return error(write_error);

    out.events.push_back(ManifestSeen{manifest->payload().revoked});
    UpdateDecision plan = plan_update(request.state, *manifest, kLinkProtocol, request.live_db);
    if (plan.new_intent) out.events.push_back(ChannelIntentSet{channel, *plan.new_intent});

    if (auto* stage = std::get_if<StageBuild>(&plan.plan)) {
        log("staging {}", stage->entry.version.to_string());
        uint64_t total = 0;
        for (const FileEntry& f : stage->entry.files) total += f.size;
        auto progress = [&](uint64_t done) {
            uint8_t pct = total ? static_cast<uint8_t>(std::min<uint64_t>(100, done * 100 / total)) : 100;
            std::lock_guard lock(mutex_);
            if (percent_ != pct) {
                percent_ = pct;
                SetEvent(event_);
            }
        };
        auto sealed = builds_.seal(stage->entry, *source_, progress, cancel_);
        if (!sealed) return error("staging " + stage->entry.version.to_string() + " failed: " + sealed.error());
        out.events.push_back(BuildSealed{*sealed, std::nullopt});
    } else if (auto* pinned = std::get_if<ReportPinned>(&plan.plan)) {
        out.view.phase = link::Phase::Pinned;
        out.view.pinned_until = pinned->until;
    } else if (auto* offer = std::get_if<OfferDowngrade>(&plan.plan)) {
        out.view.downgrade = link::DowngradeOffer{offer->offer.entry.version, wire_effect(offer->offer.data_effect)};
    }
    return out;
}

} // namespace parties::update::launcher
