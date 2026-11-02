// Offline tests for update and channel-switch decisions.

#include <parties/update/update_policy.h>

#include <parties/crypto.h>

#include <chrono>
#include <iostream>
#include <string>

using namespace parties::update;
using namespace std::chrono;

namespace {

bool fail(const std::string& msg) {
    std::cerr << "update_policy_test: " << msg << "\n";
    return false;
}

Version v(const char* text) { return *Version::parse(text); }

BuildEntry entry(const char* version, uint32_t schema, uint32_t floor, uint32_t min_link = 1, uint32_t link = 3) {
    BuildEntry b;
    b.version = v(version);
    b.data_schema = schema;
    b.data_compat_floor = floor;
    b.link_protocol = link;
    b.min_supported_launcher_link = min_link;
    // Distinct digests per version, so each version is its own build.
    size_t k = b.version.parts[1] * 7 + b.version.parts[2];
    for (auto name : {kLauncherFile, kAppFile}) {
        char fill = "0123456789abcdef"[k++ % 16];
        b.files.push_back({std::string(name), 100, *Sha256::from_hex(std::string(64, fill)),
                           {"https://github.com/emcifuntik/parties/releases/download/x"}});
    }
    return b;
}

const SigningKey kKey = [] {
    SigningKey k{"test"};
    if (!parties::random_bytes(k.secret_key.data(), 32) || !parties::derive_pubkey(k.secret_key, k.public_key))
        std::abort();
    return k;
}();

VerifiedManifest manifest(Channel channel, std::vector<BuildEntry> builds, std::vector<Version> revoked = {}) {
    ManifestPayload p;
    p.channel = channel;
    p.serial = 1;
    p.issued_at = sys_days{2026y / October / 31};
    p.expires_at = p.issued_at + days{45};
    p.builds = std::move(builds);
    p.revoked = std::move(revoked);
    std::string text = write_payload(p);
    auto env = sign_manifest({reinterpret_cast<const uint8_t*>(text.data()), text.size()}, std::vector{kKey});
    PinnedKey pin{kKey.key_id, kKey.public_key};
    auto m = verify_manifest({reinterpret_cast<const uint8_t*>(env->data()), env->size()}, {&pin, 1}, channel);
    if (!m) std::abort();
    return *m;
}

SupervisorState installed(const BuildEntry& good, Channel channel = Channel::Stable) {
    return initial_state(BuildRecord::from_entry(good), channel);
}

// Released builds: 1.5.2 (schema 2), 1.6.0 (schema 3, additive), 1.6.1 (schema 3),
// 1.7.0 (schema 4, raises the floor to 4, needs launcher protocol 3).
const BuildEntry k152 = entry("1.5.2.0", 2, 1);
const BuildEntry k160 = entry("1.6.0.0", 3, 1);
const BuildEntry k161 = entry("1.6.1.0", 3, 1);
const BuildEntry k170 = entry("1.7.0.0", 4, 4, 3);

constexpr DataInfo kData2{2, 1};
constexpr DataInfo kData3{3, 1};
constexpr DataInfo kData3Floor3{3, 3};
constexpr uint32_t kProtocol = 2;

template <class T>
const T* as(const UpdateDecision& d) { return std::get_if<T>(&d.plan); }

bool stages(const UpdateDecision& d, const BuildEntry& e, StageReason reason, const std::string& what) {
    auto* s = as<StageBuild>(d);
    if (!s) return fail(what + ": expected StageBuild, got plan #" + std::to_string(d.plan.index()));
    if (s->entry.version != e.version || s->reason != reason) return fail(what + ": staged " + s->entry.version.to_string());
    return true;
}

bool test_follow() {
    bool ok = true;
    auto s = installed(k152);
    auto m = manifest(Channel::Stable, {k152, k160});
    ok &= stages(plan_update(s, m, kProtocol, kData2), k160, StageReason::Newer, "newer build on the channel");
    if (!as<NothingToDo>(plan_update(installed(k160), m, kProtocol, kData3))) ok &= fail("up to date must do nothing");
    SupervisorState staged = s;
    staged.builds.push_back(BuildRecord::from_entry(k160));
    staged.activation = Staged{k160.id(), std::nullopt};
    if (!as<NothingToDo>(plan_update(staged, m, kProtocol, kData2))) ok &= fail("an already staged build is staged again");
    auto newer = manifest(Channel::Stable, {k152, k160, k161});
    ok &= stages(plan_update(staged, newer, kProtocol, kData2), k161, StageReason::Newer,
                 "a newer build replaces the staged one");
    SupervisorState busy = staged;
    busy.activation = Probation{k160.id(), k152.id(), std::nullopt, 0, false};
    auto* blocked = as<Blocked>(plan_update(busy, newer, kProtocol, kData2));
    if (!blocked || blocked->reason != BlockReason::Busy) ok &= fail("nothing is staged during probation");
    return ok;
}

bool test_switch_to_older_channel_pins() {
    bool ok = true;
    auto s = installed(k160, Channel::Beta);
    auto stable = manifest(Channel::Stable, {k152});
    auto plan = plan_channel_switch(s, stable, kProtocol, kData3);
    auto* pinned = std::get_if<StayPinned>(&plan);
    if (!pinned || pinned->until != k160.version) return fail("switching to an older channel must pin at the installed build");
    if (!pinned->downgrade || pinned->downgrade->entry.version != k152.version ||
        pinned->downgrade->data_effect != DataEffect::ReadsAsIs)
        ok &= fail("an additive schema makes the downgrade compatible");
    if (intent_for(plan) != ChannelIntent{PinnedUntilAtLeast{k160.version}}) ok &= fail("pinned intent");

    s.channel = Channel::Stable;
    s.intent = intent_for(plan);
    auto* report = as<ReportPinned>(plan_update(s, stable, kProtocol, kData3));
    if (!report || report->until != k160.version || report->channel_newest != k152.version)
        ok &= fail("a pinned user is told what they wait for, and nothing is staged");

    // Stable catches up with the promoted, byte-identical build: the pin lifts, nothing downloads.
    auto caught_up = plan_update(s, manifest(Channel::Stable, {k152, k160}), kProtocol, kData3);
    if (!as<NothingToDo>(caught_up) || caught_up.new_intent != ChannelIntent{Follow{}})
        ok &= fail("the pin lifts when the channel reaches it");
    auto passed = plan_update(s, manifest(Channel::Stable, {k152, k160, k161}), kProtocol, kData3);
    ok &= stages(passed, k161, StageReason::Newer, "a pinned user follows once the channel passes the pin");
    if (passed.new_intent != ChannelIntent{Follow{}}) ok &= fail("pin lift alongside staging");

    if (!std::holds_alternative<FollowNow>(plan_channel_switch(installed(k152), manifest(Channel::Beta, {k152, k160}),
                                                               kProtocol, kData2)))
        ok &= fail("switching to a newer channel follows at once");
    return ok;
}

bool test_downgrade_offers() {
    bool ok = true;
    auto s = installed(k170, Channel::Beta);
    auto stable = manifest(Channel::Stable, {k152, k161});
    auto incompatible = std::get<StayPinned>(plan_channel_switch(s, stable, 3, DataInfo{4, 4}));
    if (!incompatible.downgrade || incompatible.downgrade->data_effect != DataEffect::NeedsRestore)
        ok &= fail("a raised floor makes the downgrade a backup restore");
    auto compatible = std::get<StayPinned>(plan_channel_switch(installed(k161), manifest(Channel::Stable, {k152}),
                                                               kProtocol, kData3));
    if (!compatible.downgrade || compatible.downgrade->data_effect != DataEffect::ReadsAsIs)
        ok &= fail("floor 1 data is readable by the older build");
    auto none = std::get<StayPinned>(plan_channel_switch(installed(k161), manifest(Channel::Stable, {}), kProtocol, kData3));
    if (none.downgrade) ok &= fail("an empty channel offers nothing");

    // Following a channel that is older than the install offers, never stages.
    auto offer = plan_update(installed(k161), manifest(Channel::Stable, {k152}), kProtocol, kData3Floor3);
    auto* o = as<OfferDowngrade>(offer);
    if (!o || o->offer.data_effect != DataEffect::NeedsRestore) ok &= fail("an older channel is only an offer");
    return ok;
}

bool test_revoked_good_moves() {
    bool ok = true;
    auto s = installed(k160);
    ok &= stages(plan_update(s, manifest(Channel::Stable, {k152, k160}, {k160.version}), kProtocol, kData3), k152,
                 StageReason::LeaveRevoked, "revoked good moves to the newest compatible older build");
    ok &= stages(plan_update(s, manifest(Channel::Stable, {k152, k160, k161}, {k160.version}), kProtocol, kData3),
                 k161, StageReason::LeaveRevoked, "revoked good prefers a newer fix");
    auto* none = as<Blocked>(plan_update(s, manifest(Channel::Stable, {k152, k160}, {k160.version}), kProtocol,
                                         kData3Floor3));
    if (!none || none->reason != BlockReason::NoCompatibleBuild)
        ok &= fail("no compatible build must block instead of breaking the data");
    SupervisorState pinned = s;
    pinned.intent = PinnedUntilAtLeast{v("1.9.0.0")};
    ok &= stages(plan_update(pinned, manifest(Channel::Stable, {k152, k160}, {k160.version}), kProtocol, kData3), k152,
                 StageReason::LeaveRevoked, "revocation overrides a pin");
    ok &= stages(plan_update(installed(k152), manifest(Channel::Stable, {k152, k160, k161}, {k161.version}), kProtocol,
                             kData2),
                 k160, StageReason::Newer, "a revoked newer build is skipped");
    return ok;
}

bool test_launcher_protocol_gate() {
    bool ok = true;
    auto s = installed(k161);
    auto m = manifest(Channel::Stable, {k161, k170});
    auto* blocked = as<Blocked>(plan_update(s, m, kProtocol, kData3));
    if (!blocked || blocked->reason != BlockReason::LauncherTooOld || blocked->waiting_for != k170.version)
        ok &= fail("a build the running launcher cannot host must not be staged");
    ok &= stages(plan_update(s, m, 3, kData3), k170, StageReason::Newer, "a capable launcher stages it");
    auto between = entry("1.6.5.0", 3, 1);
    ok &= stages(plan_update(s, manifest(Channel::Stable, {k161, between, k170}), kProtocol, kData3), between,
                 StageReason::Newer, "the newest hostable build is staged instead");
    return ok;
}

bool test_quarantine_until_newer() {
    bool ok = true;
    SupervisorState s = installed(k152);
    BuildRecord failed = BuildRecord::from_entry(k160);
    failed.verdict = Quarantined{FailureReason::CrashedBeforeHealthy};
    s.builds.push_back(failed);
    if (!as<NothingToDo>(plan_update(s, manifest(Channel::Stable, {k152, k160}), kProtocol, kData2)))
        ok &= fail("a rolled-back build is re-offered");
    ok &= stages(plan_update(s, manifest(Channel::Stable, {k152, k160, k161}), kProtocol, kData2), k161,
                 StageReason::Newer, "a newer build lifts the quarantine");
    return ok;
}

bool test_data_gate_on_newer_builds() {
    // Data from a newer beta whose floor no stable build reaches yet.
    auto s = installed(k161);
    auto m = manifest(Channel::Stable, {k152, k161, entry("1.6.2.0", 3, 1)});
    auto* nothing = as<NothingToDo>(plan_update(s, m, kProtocol, DataInfo{5, 5}));
    if (!nothing) return fail("a build that cannot open the data must not be staged");
    return true;
}

} // namespace

int main() {
    bool ok = true;
    ok &= test_follow();
    ok &= test_switch_to_older_channel_pins();
    ok &= test_downgrade_offers();
    ok &= test_revoked_good_moves();
    ok &= test_launcher_protocol_gate();
    ok &= test_quarantine_until_newer();
    ok &= test_data_gate_on_newer_builds();
    if (ok) std::cout << "update_policy_test: all passed\n";
    return ok ? 0 : 1;
}
