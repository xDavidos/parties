#pragma once

// What to download, if anything, given what is installed, what the data is,
// and an accepted manifest for the user's channel. Channel switches and
// downgrades are decided here and nowhere else. Nothing here stages by itself:
// the shell downloads, seals, and reports BuildSealed to the state machine.

#include <parties/update/data_compat.h>
#include <parties/update/manifest.h>
#include <parties/update/supervisor_state.h>

#include <cstdint>
#include <optional>
#include <span>
#include <variant>

namespace parties::update {

enum class DataEffect : uint8_t {
    ReadsAsIs,
    MigratesForward,
    NeedsRestore,  // only by restoring a dated backup the user picks
};

struct DowngradeOffer {
    BuildEntry entry;
    DataEffect data_effect;
};

struct NothingToDo {};
enum class StageReason : uint8_t { Newer, LeaveRevoked };
struct StageBuild {
    BuildEntry entry;
    StageReason reason;
};
// Never staged automatically; the user must accept it.
struct OfferDowngrade {
    DowngradeOffer offer;
};
struct ReportPinned {
    Version until;
    std::optional<Version> channel_newest;
    std::optional<DowngradeOffer> downgrade;  // what "switch now" would install
};
enum class BlockReason : uint8_t {
    Busy,               // a candidate is on probation or data is being restored
    LauncherTooOld,     // the newer build cannot talk to the running launcher
    NoCompatibleBuild,  // good is revoked and no other build can run here
};
struct Blocked {
    BlockReason reason;
    std::optional<Version> waiting_for;
};
using UpdatePlan = std::variant<NothingToDo, StageBuild, OfferDowngrade, ReportPinned, Blocked>;

struct UpdateDecision {
    UpdatePlan plan;
    std::optional<ChannelIntent> new_intent;  // set when the channel caught up with a pin
};

// launcher_protocol: the link protocol of the running (good build's) launcher.
// A candidate always runs under it, so a build whose
// min_supported_launcher_link is above it is never staged.
UpdateDecision plan_update(const SupervisorState& state, const VerifiedManifest& manifest,
                           uint32_t launcher_protocol, const std::optional<DataInfo>& live_db);

struct FollowNow {};
// The default when the target channel's newest build is older than good.
struct StayPinned {
    Version until;
    std::optional<DowngradeOffer> downgrade;  // what an explicit "switch now" would install
};
using ChannelSwitchPlan = std::variant<FollowNow, StayPinned>;

ChannelSwitchPlan plan_channel_switch(const SupervisorState& state, const VerifiedManifest& target,
                                      uint32_t launcher_protocol, const std::optional<DataInfo>& live_db);

// The intent to commit (through ChannelIntentSet) when the user switches
// without taking the downgrade.
ChannelIntent intent_for(const ChannelSwitchPlan& plan);

// The backup an accepted DataEffect::NeedsRestore downgrade puts in place:
// the newest one `entry` can open, so the least recent data is set aside.
std::optional<Snapshot> restore_point(const BuildEntry& entry, std::span<const Snapshot> newest_first);

} // namespace parties::update
