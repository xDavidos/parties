// Offline tests for the boot stub's launcher choice and its state files.

#include <parties/update/boot.h>

#include <iostream>
#include <string>

using namespace parties::update;
using namespace parties::update::boot;

namespace {

bool fail(const std::string& msg) {
    std::cerr << "update_boot_test: " << msg << "\n";
    return false;
}

Sha256 sha(char fill) { return *Sha256::from_hex(std::string(64, fill)); }

const LauncherRef kNew{"builds\\1.6.0.0-aaaaaaaa", sha('a')};
const LauncherRef kOld{"builds\\1.5.2.0-bbbbbbbb", sha('b')};
const AppRef kApp{"builds\\1.6.0.0-aaaaaaaa", sha('c')};

BootContract contract(bool with_previous = true) {
    return {kNew, with_previous ? std::optional(kOld) : std::nullopt, kApp, "C:\\Users\\u\\AppData\\Local\\Parties\\data"};
}

LauncherHealth health(const LauncherRef& ref, uint8_t failed, uint8_t clean = 0, bool confirmed = false) {
    return {ref.sha256, failed, clean, confirmed};
}

const LauncherHealth* find(const BootRecord& rec, const LauncherRef& ref) {
    for (const auto& h : rec.launchers)
        if (h.launcher == ref.sha256) return &h;
    return nullptr;
}

const RunLauncher* runs(const Decision& d) { return std::get_if<RunLauncher>(&d.choice); }

bool expect_runs(const Decision& d, const LauncherRef& ref, const std::string& what) {
    auto* r = runs(d);
    if (!r) return fail(what + ": expected a launcher, got stub recovery");
    if (r->dir != ref.dir || r->sha256 != ref.sha256) return fail(what + ": ran " + r->dir);
    if (!d.next_record.attempt || d.next_record.attempt->session != r->session ||
        d.next_record.attempt->launcher != ref.sha256)
        return fail(what + ": the attempt was not recorded before running");
    return true;
}

bool expect_recovery(const Decision& d, const std::string& what) {
    if (!std::holds_alternative<ShowStubRecovery>(d.choice)) return fail(what + ": expected stub recovery");
    if (d.next_record.attempt) return fail(what + ": recovery must not leave an attempt pending");
    return true;
}

bool test_table() {
    bool ok = true;
    ok &= expect_recovery(choose(std::nullopt, {}, std::nullopt), "no contract");
    ok &= expect_runs(choose(contract(), {}, std::nullopt), kNew, "fresh good launcher");
    ok &= expect_runs(choose(contract(), {0, {}, {health(kNew, 1)}}, std::nullopt), kNew, "one failure keeps good");
    ok &= expect_runs(choose(contract(), {0, {}, {health(kNew, 2)}}, std::nullopt), kOld,
                      "two failed boots fall back to previous_good");
    ok &= expect_runs(choose(contract(), {0, {}, {health(kNew, 7, 2, true)}}, std::nullopt), kNew,
                      "a confirmed launcher is never abandoned for failures");
    ok &= expect_recovery(choose(contract(false), {0, {}, {health(kNew, 2)}}, std::nullopt),
                          "no previous_good to fall back to");
    ok &= expect_recovery(choose(contract(), {0, {}, {health(kNew, 2), health(kOld, 2)}}, std::nullopt),
                          "both launchers failed");
    Sha256 corrupt[] = {kNew.sha256};
    ok &= expect_runs(choose(contract(), {0, {}, {health(kNew, 0, 5, true)}}, std::nullopt, corrupt), kOld,
                      "a corrupt good launcher is skipped even when confirmed");
    Sha256 both[] = {kNew.sha256, kOld.sha256};
    ok &= expect_recovery(choose(contract(), {}, std::nullopt, both), "every launcher corrupt");

    auto d = choose(contract(), {41, {}, {}}, std::nullopt);
    if (!runs(d) || runs(d)->session != 42 || d.next_record.last_session != 42)
        ok &= fail("session ids must increase from last_session");
    return ok;
}

BootRecord pending(const LauncherRef& ref, uint64_t session, std::vector<LauncherHealth> launchers = {}) {
    return {session, Attempt{session, ref.sha256}, std::move(launchers)};
}

bool test_session_folding() {
    bool ok = true;
    {
        auto d = choose(contract(), pending(kNew, 5), SessionReport{5, kNew.sha256, true});
        auto* h = find(d.next_record, kNew);
        if (!h || h->clean_sessions != 1 || h->failed_boots != 0 || h->confirmed)
            ok &= fail("one clean session counts once and does not confirm");
        auto d2 = choose(contract(), d.next_record, SessionReport{6, kNew.sha256, true});
        h = find(d2.next_record, kNew);
        if (!h || !h->confirmed) ok &= fail("two clean sessions confirm the launcher");
    }
    {
        auto d = choose(contract(), pending(kNew, 5), std::nullopt);
        auto* h = find(d.next_record, kNew);
        if (!h || h->failed_boots != 1) ok &= fail("a session with no report is a failure");
    }
    {
        auto d = choose(contract(), pending(kNew, 5), SessionReport{5, kNew.sha256, false});
        auto* h = find(d.next_record, kNew);
        if (!h || h->failed_boots != 1) ok &= fail("an unclean session is a failure");
    }
    {
        auto d = choose(contract(), pending(kNew, 5), SessionReport{4, kNew.sha256, true});
        auto* h = find(d.next_record, kNew);
        if (!h || h->failed_boots != 1 || h->clean_sessions != 0)
            ok &= fail("a clean report from an older session must not count");
    }
    {
        auto d = choose(contract(), pending(kNew, 5), SessionReport{5, kOld.sha256, true});
        auto* h = find(d.next_record, kNew);
        if (!h || h->failed_boots != 1) ok &= fail("a report from another launcher must not count");
    }
    {
        auto d = choose(contract(), pending(kNew, 5, {health(kNew, 0, 2, true)}), SessionReport{5, kNew.sha256, false});
        auto* h = find(d.next_record, kNew);
        if (!h || h->failed_boots != 0) ok &= fail("a confirmed launcher is not blamed for an unclean session");
    }
    {
        // Folding happens once: the next choose records a new attempt, so the
        // same report no longer matches it.
        auto d = choose(contract(), pending(kNew, 5), SessionReport{5, kNew.sha256, true});
        auto again = choose(contract(), d.next_record, SessionReport{5, kNew.sha256, true});
        auto* h = find(again.next_record, kNew);
        if (!h || h->clean_sessions != 1) ok &= fail("a report folded twice counted twice");
    }
    return ok;
}

bool test_new_launcher_lifecycle() {
    bool ok = true;
    // A broken launcher: two stub runs where it never signals boot_ok.
    BootRecord rec;
    auto d1 = choose(contract(), rec, std::nullopt);
    ok &= expect_runs(d1, kNew, "first boot tries the new launcher");
    rec = record_boot_failure(d1.next_record);
    auto d2 = choose(contract(), rec, std::nullopt);
    ok &= expect_runs(d2, kNew, "one failure retries the new launcher");
    rec = record_boot_failure(d2.next_record);
    auto d3 = choose(contract(), rec, std::nullopt);
    ok &= expect_runs(d3, kOld, "second failure falls back in the same stub run");
    auto* h = find(d3.next_record, kNew);
    if (!h || h->failed_boots != 2) ok &= fail("failures were not persisted");
    auto d4 = choose(contract(), d3.next_record, SessionReport{d3.next_record.last_session, kOld.sha256, true});
    ok &= expect_runs(d4, kOld, "the broken launcher stays skipped");
    if (record_boot_failure(BootRecord{}) != BootRecord{}) ok &= fail("failure without an attempt changed the record");
    return ok;
}

bool test_pruning() {
    LauncherHealth stale{sha('d'), 2, 0, false};
    auto d = choose(contract(), {0, {}, {stale, health(kNew, 1)}}, std::nullopt);
    if (find(d.next_record, LauncherRef{"", sha('d')})) return fail("health of a launcher no longer in the contract kept");
    if (!find(d.next_record, kNew)) return fail("health of the good launcher dropped");
    return true;
}

// Every combination of small inputs; the properties hold for all of them.
bool test_exhaustive_properties() {
    std::vector<std::optional<BootContract>> contracts = {std::nullopt, contract(false), contract(true)};
    BootContract same = contract(true);
    same.previous_good = kNew;
    contracts.push_back(same);
    std::vector<LauncherHealth> states;
    for (uint8_t f : {0, 1, 2, 3})
        for (uint8_t c : {0, 1, 2})
            for (bool conf : {false, true}) states.push_back({{}, f, c, conf});
    std::vector<std::optional<SessionReport>> reports = {std::nullopt, SessionReport{9, kNew.sha256, true},
                                                         SessionReport{9, kNew.sha256, false},
                                                         SessionReport{8, kNew.sha256, true}};
    int cases = 0;
    for (const auto& c : contracts)
        for (const auto& hn : states)
            for (const auto& ho : states)
                for (bool has_attempt : {false, true})
                    for (const auto& report : reports)
                        for (int skip = 0; skip < 3; ++skip) {
                            LauncherHealth a = hn, b = ho;
                            a.launcher = kNew.sha256;
                            b.launcher = kOld.sha256;
                            BootRecord rec{9, has_attempt ? std::optional(Attempt{9, kNew.sha256}) : std::nullopt,
                                           {a, b}};
                            std::vector<Sha256> unusable;
                            if (skip == 1) unusable = {kNew.sha256};
                            if (skip == 2) unusable = {kNew.sha256, kOld.sha256};
                            auto d = choose(c, rec, report, unusable);
                            ++cases;
                            if (auto* r = runs(d)) {
                                bool in_contract = c && (r->sha256 == c->good.sha256 ||
                                                         (c->previous_good && r->sha256 == c->previous_good->sha256));
                                if (!in_contract) return fail("ran a launcher outside the contract");
                                if (std::ranges::find(unusable, r->sha256) != unusable.end())
                                    return fail("ran an unusable launcher");
                                auto* h = find(d.next_record, LauncherRef{"", r->sha256});
                                if (h && !h->confirmed && h->failed_boots >= kMaxFailedBoots)
                                    return fail("ran an unconfirmed launcher after two failures");
                                if (r->session != rec.last_session + 1) return fail("session id not fresh");
                                if (c && r->sha256 != c->good.sha256) {
                                    auto* g = find(d.next_record, c->good);
                                    bool good_ok = std::ranges::find(unusable, c->good.sha256) == unusable.end() &&
                                                   (!g || g->confirmed || g->failed_boots < kMaxFailedBoots);
                                    if (good_ok) return fail("skipped an eligible good launcher");
                                }
                            } else if (c && d.next_record.attempt) {
                                return fail("recovery left an attempt pending");
                            }
                            for (const auto& h : d.next_record.launchers) {
                                auto* before = find(rec, LauncherRef{"", h.launcher});
                                if (before && before->confirmed && !h.confirmed) return fail("confirmation was lost");
                                if (h.confirmed && h.clean_sessions < kCleanSessionsToConfirm &&
                                    !(before && before->confirmed))
                                    return fail("confirmed without two clean sessions");
                            }
                        }
    std::cout << "update_boot_test: exhaustive choose cases " << cases << "\n";
    return true;
}

bool test_codecs_round_trip() {
    bool ok = true;
    for (const auto& c : {contract(true), contract(false)}) {
        auto parsed = parse_boot_contract(write_boot_contract(c));
        if (!parsed || *parsed != c) ok &= fail("boot contract round trip");
    }
    BootRecord rec{12, Attempt{12, sha('a')}, {health(kNew, 1, 0), health(kOld, 0, 3, true)}};
    auto parsed_rec = parse_boot_record(write_boot_record(rec));
    if (!parsed_rec || *parsed_rec != rec) ok &= fail("boot record round trip");
    BootRecord empty;
    if (parse_boot_record(write_boot_record(empty)) != empty) ok &= fail("empty boot record round trip");
    SessionReport s{12, sha('a'), true};
    if (parse_session_report(write_session_report(s)) != s) ok &= fail("session report round trip");
    return ok;
}

bool test_parse_robustness() {
    bool ok = true;
    std::string c = write_boot_contract(contract());
    std::string r = write_boot_record({3, Attempt{3, sha('a')}, {health(kNew, 1)}});
    std::string s = write_session_report({3, sha('a'), false});
    for (size_t n = 0; n < c.size(); ++n)
        if (parse_boot_contract(c.substr(0, n))) ok &= fail("truncated contract parsed at " + std::to_string(n));
    for (size_t n = 0; n < r.size(); ++n)
        if (parse_boot_record(r.substr(0, n))) ok &= fail("truncated record parsed at " + std::to_string(n));
    for (size_t n = 0; n < s.size(); ++n)
        if (parse_session_report(s.substr(0, n))) ok &= fail("truncated session parsed at " + std::to_string(n));
    for (const char* garbage : {"", "null", "[]", "42", "\"x\"", "{", "{}", "\xff\xfe{", "{\"good\":1}"}) {
        if (parse_boot_contract(garbage)) ok &= fail(std::string("garbage contract parsed: ") + garbage);
        if (parse_boot_record(garbage)) ok &= fail(std::string("garbage record parsed: ") + garbage);
        if (parse_session_report(garbage)) ok &= fail(std::string("garbage session parsed: ") + garbage);
    }
    auto replace = [](std::string text, const std::string& from, const std::string& to) {
        text.replace(text.find(from), from.size(), to);
        return text;
    };
    for (const char* dir : {"..\\..\\Windows", "builds\\..\\..\\evil", "C:\\evil", "builds\\", "builds/x",
                            "builds\\a\\b", "\\\\server\\share"}) {
        std::string escaped;
        for (char ch : std::string(dir)) {
            if (ch == '\\') escaped += "\\\\";
            else escaped += ch;
        }
        std::string bad = replace(c, "builds\\\\1.6.0.0-aaaaaaaa", escaped);
        if (parse_boot_contract(bad)) ok &= fail(std::string("contract with dir '") + dir + "' parsed");
    }
    if (parse_boot_contract(replace(c, std::string(64, 'a'), std::string(64, 'A'))))
        ok &= fail("uppercase digest parsed");
    if (parse_boot_record(replace(r, "\"failed_boots\":1", "\"failed_boots\":-1"))) ok &= fail("negative count parsed");
    std::string future = replace(c, "\"format\":1", "\"format\":2,\"added_later\":{\"x\":[1,2]}");
    if (parse_boot_contract(future) != contract()) ok &= fail("added fields or a newer format broke the contract");
    std::string huge = replace(r, "\"failed_boots\":1", "\"failed_boots\":100000");
    auto clamped = parse_boot_record(huge);
    if (!clamped || clamped->launchers[0].failed_boots != 255) ok &= fail("large counts must clamp, not wrap");
    return ok;
}

} // namespace

int main() {
    bool ok = true;
    ok &= test_table();
    ok &= test_session_folding();
    ok &= test_new_launcher_lifecycle();
    ok &= test_pruning();
    ok &= test_exhaustive_properties();
    ok &= test_codecs_round_trip();
    ok &= test_parse_robustness();
    if (ok) std::cout << "update_boot_test: all passed\n";
    return ok ? 0 : 1;
}
