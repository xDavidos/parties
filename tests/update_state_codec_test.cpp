// Offline tests for the persisted launcher state codecs.

#include <parties/update/boot.h>
#include <parties/update/state_codec.h>

#include <chrono>
#include <iostream>
#include <string>

using namespace parties::update;
using namespace std::chrono;

namespace {

bool fail(const std::string& msg) {
    std::cerr << "update_state_codec_test: " << msg << "\n";
    return false;
}

Sha256 sha(char fill) { return *Sha256::from_hex(std::string(64, fill)); }

BuildRecord rec(const char* version, char fill, Verdict verdict) {
    BuildRecord r;
    r.id = {*Version::parse(version), sha(fill)};
    r.data_schema = 3;
    r.data_compat_floor = 2;
    r.link_protocol = 4;
    r.min_supported_launcher_link = 2;
    r.launcher_sha256 = sha(fill);
    r.app_sha256 = sha(char(fill + 1));
    r.verdict = verdict;
    return r;
}

const BuildRecord kGood = rec("1.5.2.0", 'a', Proven{});
const BuildRecord kPrev = rec("1.5.1.0", 'c', Proven{});
const BuildRecord kCand = rec("1.6.0.0", 'e', Unproven{});

SupervisorState base() {
    SupervisorState s;
    s.generation = 42;
    s.channel = Channel::Beta;
    s.good = kGood.id;
    s.previous_good = kPrev.id;
    s.builds = {kGood, kPrev, kCand};
    return s;
}

std::vector<SupervisorState> samples() {
    std::vector<SupervisorState> out;
    out.push_back(base());
    {
        SupervisorState s = base();
        s.previous_good.reset();
        s.intent = PinnedUntilAtLeast{*Version::parse("1.6.0.0")};
        s.activation = Staged{kCand.id, Snapshot{{"backup-2026-10-12"}, {2, 1}}};
        s.notice = Notice{Notice::Kind::DataRestored, kGood.id.version, kGood.id.version, SnapshotId{"pre-restore-x"}};
        out.push_back(s);
    }
    {
        SupervisorState s = base();
        s.activation = Staged{kCand.id, std::nullopt};
        s.operation = Running{kGood.id, true, 33};
        out.push_back(s);
    }
    {
        SupervisorState s = base();
        s.activation = Probation{kCand.id, kGood.id, Snapshot{{"pre-1.6.0.0-eeeeeeee-g43"}, {2, 1}}, 1, true};
        s.operation = Running{kCand.id, false, 0};
        s.notice = Notice{Notice::Kind::RolledBack, kCand.id.version, kGood.id.version, std::nullopt};
        out.push_back(s);
    }
    {
        SupervisorState s = base();
        s.activation = Probation{kCand.id, kGood.id, std::nullopt, 0, false};
        s.operation = Restoring{Snapshot{{"pre-x"}, {1, 1}}, SnapshotId{"rolled-back-x-g44"}};
        out.push_back(s);
    }
    {
        SupervisorState s = base();
        s.revoked = {{kGood.id.version, kCand.id.version}, {kCand.id.version}};
        out.push_back(s);
    }
    for (FailureReason reason :
         {FailureReason::CrashedBeforeHealthy, FailureReason::InitFailed, FailureReason::HungBeforeHealthy,
          FailureReason::TerminatedBeforeHealthy, FailureReason::DataTooNew,
          FailureReason::CrashedRepeatedlyAfterHealthy, FailureReason::UnknownOutcomeRepeated,
          FailureReason::Corrupt, FailureReason::Revoked, FailureReason::UserRolledBack}) {
        SupervisorState s = base();
        s.builds[2].verdict = Quarantined{reason};
        out.push_back(s);
    }
    return out;
}

std::string replace(std::string text, const std::string& from, const std::string& to) {
    auto pos = text.find(from);
    if (pos == std::string::npos) std::abort();
    return text.replace(pos, from.size(), to);
}

bool is_error(const std::expected<SupervisorState, StateParseError>& r, StateParseError e) {
    return !r && r.error() == e;
}

bool test_round_trip() {
    for (const auto& s : samples()) {
        auto parsed = parse_state(write_state(s));
        if (!parsed) return fail("valid state rejected: " + write_state(s));
        if (*parsed != s) return fail("round trip changed the state: " + write_state(s));
    }
    return true;
}

bool test_unknown_fields_ignored() {
    std::string text = write_state(samples()[3]);
    text = replace(text, "{\"schema\":1,", "{\"schema\":1,\"added_later\":{\"x\":[1,2,{}]},");
    text = replace(text, "\"failed_runs\":1", "\"failed_runs\":1,\"new_counter\":7");
    text = replace(text, "\"data_schema\":3", "\"data_schema\":3,\"notes_url\":\"https://x\"");
    auto parsed = parse_state(text);
    if (!parsed || *parsed != samples()[3]) return fail("unknown fields broke parsing");
    return true;
}

// Launchers before revocations were kept wrote no "revoked" member; schema 1
// stays, because older launchers ignore the member they do not know.
bool test_written_before_revocations() {
    SupervisorState s = base();
    std::string text = replace(write_state(s), "\"revoked\":{\"stable\":[],\"beta\":[]},", "");
    auto parsed = parse_state(text);
    if (!parsed || *parsed != s) return fail("a state without revocations must parse with none");
    SupervisorState listed = base();
    listed.revoked.beta = {kCand.id.version};
    if (parse_state(replace(write_state(listed), "\"1.6.0.0\"]", "\"not-a-version\"]")))
        return fail("a malformed revoked version accepted");
    return true;
}

bool test_schema_versions() {
    std::string text = write_state(base());
    if (!is_error(parse_state(replace(text, "\"schema\":1", "\"schema\":2")), StateParseError::FutureSchema))
        return fail("a future schema must be refused as FutureSchema");
    if (!is_error(parse_state(replace(text, "\"schema\":1", "\"schema\":0")), StateParseError::Malformed))
        return fail("schema 0 accepted");
    if (!is_error(parse_state(replace(text, "\"schema\":1,", "")), StateParseError::Malformed))
        return fail("missing schema accepted");
    return true;
}

bool test_malformed_and_inconsistent() {
    bool ok = true;
    std::string text = write_state(samples()[3]);
    for (size_t n = 0; n < text.size(); ++n)
        if (parse_state(text.substr(0, n))) ok &= fail("truncated state parsed at " + std::to_string(n));
    for (const char* garbage : {"", "null", "[]", "{}", "\xef\xbb\xbf{", "{\"schema\":1}"})
        if (parse_state(garbage)) ok &= fail(std::string("garbage parsed: ") + garbage);

    std::string good_text = write_state(base());
    if (!is_error(parse_state(replace(good_text, "\"kind\":\"proven\"", "\"kind\":\"unproven\"")),
                  StateParseError::Inconsistent))
        ok &= fail("an unproven good build accepted");
    if (!is_error(parse_state(replace(good_text, "\"kind\":\"steady\"", "\"kind\":\"sideways\"")),
                  StateParseError::Malformed))
        ok &= fail("unknown activation kind accepted");
    SupervisorState dangling = base();
    dangling.builds.erase(dangling.builds.begin());
    if (!is_error(parse_state(write_state(dangling)), StateParseError::Inconsistent))
        ok &= fail("good without a record accepted");
    SupervisorState self_candidate = base();
    self_candidate.activation = Probation{kGood.id, kGood.id, std::nullopt, 0, false};
    if (!is_error(parse_state(write_state(self_candidate)), StateParseError::Inconsistent))
        ok &= fail("good as its own candidate accepted");
    SupervisorState wrong_fallback = base();
    wrong_fallback.activation = Probation{kCand.id, kPrev.id, std::nullopt, 0, false};
    if (!is_error(parse_state(write_state(wrong_fallback)), StateParseError::Inconsistent))
        ok &= fail("a fallback other than good accepted");
    SupervisorState unsafe = samples()[4];
    std::get<Restoring>(unsafe.operation).preserve_live_as.stem = "..\\..\\evil";
    if (!is_error(parse_state(write_state(unsafe)), StateParseError::Malformed))
        ok &= fail("a snapshot name with a path accepted");
    return ok;
}

bool test_trust_file() {
    TrustFile t;
    t.beta = {17, sha('9'), sys_days{2026y / October / 31} + 12h};
    auto parsed = parse_trust(write_trust(t));
    if (!parsed || *parsed != t) return fail("trust round trip");
    std::string text = write_trust(t);
    if (parse_trust(replace(text, "\"schema\":1", "\"schema\":9")).error() != StateParseError::FutureSchema)
        return fail("future trust schema accepted");
    auto only_beta = parse_trust(replace(text, "\"stable\"", "\"other\""));
    if (!only_beta || only_beta->stable != TrustState{} || only_beta->beta != t.beta)
        return fail("a missing channel must start from an empty ratchet");
    for (size_t n = 0; n < text.size(); ++n)
        if (parse_trust(text.substr(0, n))) return fail("truncated trust parsed");
    return true;
}

// The launcher derives the contract on every commit; the stub must read it back.
bool test_boot_contract_projection() {
    SupervisorState s = base();
    auto contract = boot::parse_boot_contract(write_boot_contract(derive_boot_contract(s, R"(C:\Users\u\Data)")));
    if (!contract) return fail("derived boot contract does not parse in the stub");
    if (contract->good.dir != R"(builds\1.5.2.0-aaaaaaaa)" || contract->good.sha256 != kGood.launcher_sha256)
        return fail("contract good is not good's launcher");
    if (!contract->previous_good || contract->previous_good->sha256 != kPrev.launcher_sha256)
        return fail("contract previous_good is not previous_good's launcher");
    if (contract->last_good_app.sha256 != kGood.app_sha256 || contract->data_dir != R"(C:\Users\u\Data)")
        return fail("safe mode would not run good's app on the same data");
    // A candidate never appears in the contract: the stub only runs proven launchers.
    s.activation = Probation{kCand.id, kGood.id, std::nullopt, 0, false};
    auto during = derive_boot_contract(s, R"(C:\d)");
    if (during.good.sha256 != kGood.launcher_sha256) return fail("probation changed the stub's launcher");
    return true;
}

} // namespace

int main() {
    bool ok = true;
    ok &= test_round_trip();
    ok &= test_written_before_revocations();
    ok &= test_unknown_fields_ignored();
    ok &= test_schema_versions();
    ok &= test_malformed_and_inconsistent();
    ok &= test_trust_file();
    ok &= test_boot_contract_projection();
    if (ok) std::cout << "update_state_codec_test: all passed\n";
    return ok ? 0 : 1;
}
