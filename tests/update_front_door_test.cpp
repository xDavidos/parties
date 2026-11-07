#include <parties/update/front_door.h>
#include <parties/update/front_door_policy.h>
#include <parties/update/front_start.h>

#include <climits>
#include <cstdio>
#include <string>
#include <vector>

using namespace parties::update;
using namespace parties::update::front_door;
namespace start = parties::update::front_start;
namespace boot = parties::update::boot;

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        std::fprintf(stderr, "update_front_door_test: %s\n", what.c_str());
        ++failures;
    }
}

// process-contract.md, "Exit classification".
ExitKind contract_kind(const FrontOutcome& o) {
    if (!o.reported_code) {
        if (o.killed_by_launcher) return ExitKind::KilledHung;
        return o.session_ending ? ExitKind::Clean : ExitKind::Abnormal;
    }
    switch (*o.reported_code) {
    case 0: return ExitKind::Clean;
    case 2: return ExitKind::InitFailed;
    case 3: return ExitKind::DataUnavailable;
    case 4: return ExitKind::DataTooNew;
    default: return ExitKind::Abnormal;
    }
}

void test_classify_exhaustive() {
    std::vector<std::optional<int32_t>> codes{std::nullopt, INT32_MIN, INT32_MAX};
    for (int32_t c = -256; c <= 511; ++c) codes.push_back(c);
    int cases = 0;
    for (const auto& code : codes)
        for (bool killed : {false, true})
            for (bool ending : {false, true}) {
                FrontOutcome o{code, killed, ending};
                ExitKind kind = classify_exit(o);
                std::string label = "code=" + (code ? std::to_string(*code) : std::string("none")) +
                                    " killed=" + std::to_string(killed) + " ending=" + std::to_string(ending);
                check(kind == contract_kind(o), "classify " + label);
                check(kind != ExitKind::Terminated, "Terminated is never produced: " + label);
                ++cases;
            }
    std::printf("classify: %d outcomes\n", cases);

    check(classify_exit({std::nullopt, false, false}) == ExitKind::Abnormal, "a crash with no report is blamed");
    check(classify_exit({1, false, false}) == ExitKind::Abnormal, "exit 1 is Abnormal, not Terminated");
    check(classify_exit({0, true, false}) == ExitKind::Clean, "the grace kill after exiting{0} keeps Clean");
    check(classify_exit({3, true, false}) == ExitKind::DataUnavailable, "the grace kill keeps the reported code");
    check(classify_exit({std::nullopt, true, true}) == ExitKind::KilledHung, "a launcher kill beats session end");
}

constexpr FrontState kStates[]{FrontState::Waiting, FrontState::Assigned, FrontState::Recovering,
                               FrontState::Leaving};

std::vector<std::vector<FrontView>> front_lists(size_t max_len) {
    std::vector<std::vector<FrontView>> lists{{}};
    std::vector<std::vector<FrontView>> frontier{{}};
    for (size_t len = 1; len <= max_len; ++len) {
        std::vector<std::vector<FrontView>> next;
        for (const auto& base : frontier)
            for (FrontState s : kStates) {
                auto list = base;
                list.push_back({int32_t(100 + len), s});
                next.push_back(list);
            }
        lists.insert(lists.end(), next.begin(), next.end());
        frontier = std::move(next);
    }
    return lists;
}

const FrontView* first_in(const std::vector<FrontView>& fronts, FrontState s) {
    for (const auto& f : fronts)
        if (f.state == s) return &f;
    return nullptr;
}

void test_route_exhaustive() {
    int cases = 0;
    for (const auto& fronts : front_lists(3))
        for (int flags = 0; flags < 16; ++flags) {
            RouteInput in{fronts, bool(flags & 1), bool(flags & 2), bool(flags & 4), bool(flags & 8)};
            Route r = route_front(in);
            std::string label = "fronts=" + std::to_string(fronts.size()) + " flags=" + std::to_string(flags) + " [";
            for (const auto& f : fronts) label += std::to_string(int(f.state));
            label += "]";

            const FrontView* owner = first_in(fronts, FrontState::Assigned);
            if (!owner) owner = first_in(fronts, FrontState::Recovering);
            if (!owner) owner = first_in(fronts, FrontState::Waiting);
            if (owner) {
                check(r == Route{SendActivateOther{owner->pid}}, "an owner takes every newcomer " + label);
            } else if (in.pending_spawn) {
                check(r == Route{DeliverPendingSpawn{}}, "a pending spawn gets the newcomer " + label);
            } else if (in.recovery_pending) {
                check(r == Route{DeliverPendingRecovery{}}, "a pending recovery gets the newcomer " + label);
            } else if (in.newcomer_seed_pending) {
                check(r == Route{SendQuitForInstall{}}, "a newer seed quits an idle launcher " + label);
            } else if (in.newcomer_recover) {
                check(r == Route{OpenRequestedRecovery{}}, "recover:true opens recovery " + label);
            } else {
                check(r == Route{StartForFront{}}, "an idle launcher starts the app " + label);
            }
            ++cases;
        }
    std::printf("route: %d inputs\n", cases);

    RouteInput second_open{{{7, FrontState::Assigned}}, false, false, false, false};
    check(route_front(second_open) == Route{SendActivateOther{7}}, "a second open gets activate_other");
    second_open.newcomer_seed_pending = true;
    check(route_front(second_open) == Route{SendActivateOther{7}}, "a newer seed never ends an app session");
    RouteInput order{{{1, FrontState::Waiting}, {2, FrontState::Recovering}, {3, FrontState::Assigned}}};
    check(route_front(order) == Route{SendActivateOther{3}}, "the assigned front outranks earlier arrivals");
    RouteInput leaving{{{1, FrontState::Leaving}}, true, false, true, true};
    check(route_front(leaving) == Route{DeliverPendingSpawn{}}, "a leaving front owns nothing");
}

template <class M, class Parse>
bool round_trips(const M& message, Parse parse) {
    std::string line = write(message);
    if (line.empty() || line.back() != '\n') return false;
    line.pop_back();
    if (line.find('\n') != std::string::npos) return false;
    auto parsed = parse(line);
    return parsed && *parsed == message;
}

Sha256 sha(char fill) { return *Sha256::from_hex(std::string(64, fill)); }

void test_codec() {
    auto host = [](std::string_view l) { return parse_host_message(l); };
    auto launcher = [](std::string_view l) { return parse_launcher_message(l); };

    Front front{4242, 1, "/Applications/Parties.app", true, false, {"--flag", "a \"quoted\"\nline", ""}};
    check(round_trips(HostMessage{front}, host), "front round-trips");
    check(round_trips(HostMessage{Front{}}, host), "an empty front round-trips");
    check(round_trips(HostMessage{Attach{UINT64_MAX}}, host), "attach round-trips");
    check(round_trips(HostMessage{Loaded{"builds\\1.2.3.4-abcdef12"}}, host), "loaded round-trips");
    check(round_trips(HostMessage{Corrupt{"builds\\x"}}, host), "corrupt round-trips");
    check(round_trips(HostMessage{Exiting{-6}}, host), "exiting round-trips");

    Assign assign{"builds\\1.2.3.4-abcdef12", sha('a'), "/Users/u/Library/Application Support/Parties",
                  "/Users/u/Library/Application Support/Parties/backups", 9};
    check(round_trips(LauncherMessage{assign}, launcher), "assign round-trips");
    check(round_trips(LauncherMessage{ActivateOther{}}, launcher), "activate_other round-trips");
    check(round_trips(LauncherMessage{Activate{}}, launcher), "activate round-trips");
    check(round_trips(LauncherMessage{QuitForInstall{}}, launcher), "quit_for_install round-trips");
    Recovery recovery{"good_failed_to_start", "/Users/u/Library/Application Support/Parties",
                      {{"start", "Start Parties 1.2.3.4", ""}, {"restore:pre-migration-g2", "Restore a backup", "\"x\"\n"}}};
    check(round_trips(LauncherMessage{recovery}, launcher), "recovery round-trips");
    check(round_trips(LauncherMessage{Recovery{}}, launcher), "a recovery without options round-trips");
    check(round_trips(HostMessage{RecoveryChoice{"previous"}}, host), "recovery_choice round-trips");
    check(!parse_host_message("{\"type\":\"recovery_choice\",\"id\":\"\"}"), "an empty choice");
    check(!parse_launcher_message("{\"type\":\"recovery\",\"reason\":\"requested\",\"data_dir\":\"/d\","
                                  "\"options\":[{\"id\":\"\",\"label\":\"x\",\"detail\":\"\"}]}"),
          "an option without an id");
    check(!parse_launcher_message("{\"type\":\"recovery\",\"reason\":\"requested\",\"options\":[]}"),
          "a recovery without data_dir");

    check(write(HostMessage{Exiting{3}}) == "{\"type\":\"exiting\",\"code\":3}\n", "exiting wire shape");
    check(write(HostMessage{Loaded{"builds\\b"}}) == "{\"type\":\"loaded\",\"build\":\"builds\\\\b\"}\n",
          "loaded wire shape");
    check(write(LauncherMessage{ActivateOther{}}) == "{\"type\":\"activate_other\"}\n", "activate_other wire shape");

    check(parse_host_message("{\"type\":\"exiting\",\"code\":0,\"extra\":[1]}") == HostMessage{Exiting{0}},
          "unknown fields are ignored");
    check(!parse_host_message("{\"type\":\"exiting\"}"), "exiting without a code");
    check(!parse_host_message("{\"type\":\"exiting\",\"code\":\"0\"}"), "a string code");
    check(!parse_host_message("{\"type\":\"exiting\",\"code\":4294967296}"), "a code outside int32");
    check(!parse_host_message("{\"type\":\"hello\"}"), "an unknown type");
    check(!parse_host_message("{\"code\":0}"), "no type");
    check(!parse_host_message("not json"), "malformed");
    check(!parse_host_message("{\"type\":\"loaded\",\"build\":\"builds\\\\..\\\\x\"}"), "a build that climbs");
    check(!parse_host_message("{\"type\":\"loaded\",\"build\":\"/tmp/x\"}"), "an absolute build");
    check(!parse_host_message("{\"type\":\"corrupt\",\"build\":\"builds\\\\\"}"), "an empty build name");
    check(!parse_host_message("{\"type\":\"front\",\"pid\":1,\"host_abi\":1,\"host\":\"/a\",\"recover\":false,"
                              "\"seed_pending\":false,\"argv\":[1]}"),
          "a non-string argument");
    check(!parse_host_message("{\"type\":\"front\",\"pid\":1,\"host_abi\":1,\"host\":\"/a\",\"recover\":false,"
                              "\"argv\":[]}"),
          "front without seed_pending");

    std::string assign_line = write(LauncherMessage{assign});
    auto with = [&](std::string_view from, std::string_view to) {
        std::string s = assign_line;
        s.replace(s.find(from), from.size(), to);
        return s;
    };
    check(!parse_launcher_message(with(sha('a').to_hex(), "AA")), "a short digest");
    check(!parse_launcher_message(with("builds\\\\1.2.3.4-abcdef12", "builds\\\\.hidden")), "a hidden build");
    check(!parse_launcher_message(with("\"ticket\":9", "\"ticket\":-9")), "a negative ticket");
}

// process-contract.md, "Front start", spelled out independently of plan_front.
start::FrontPlan contract_plan(const start::FrontFacts& f) {
    bool pending = false;
    if (f.bundle_seed && f.bundle_seed != f.installed_seed)
        pending = !(f.record.seed == *f.bundle_seed && f.record.install_failures >= 2);
    if (f.launcher_reachable) return pending ? start::FrontPlan::ConnectSeedPending : start::FrontPlan::Connect;
    if (pending) return start::FrontPlan::InstallSeed;
    if (!f.has_contract) return start::FrontPlan::SafeMode;
    return f.boot_round <= 2 ? start::FrontPlan::ChooseLauncher : start::FrontPlan::SafeMode;
}

void test_plan_front_exhaustive() {
    const std::vector<std::optional<std::string>> seeds{std::nullopt, "a", "b"};
    int cases = 0;
    for (int flags = 0; flags < 4; ++flags)
        for (const auto& bundle : seeds)
            for (const auto& installed : seeds)
                for (const auto& counted : seeds)
                    for (uint8_t failures : {0, 1, 2, 3, 255})
                        for (uint8_t round : {0, 1, 2, 3}) {
                            start::FrontFacts f{bool(flags & 1), bool(flags & 2), bundle, installed,
                                                {counted.value_or(""), failures}, round};
                            std::string label = "flags=" + std::to_string(flags) + " bundle=" + bundle.value_or("-") +
                                                " installed=" + installed.value_or("-") +
                                                " counted=" + counted.value_or("-") +
                                                " failures=" + std::to_string(failures) +
                                                " round=" + std::to_string(round);
                            check(start::plan_front(f) == contract_plan(f), "plan_front " + label);
                            ++cases;
                        }
    std::printf("plan_front: %d facts\n", cases);

    start::FrontFacts first_run{false, false, "a", std::nullopt, {}, 0};
    check(start::plan_front(first_run) == start::FrontPlan::InstallSeed, "a first run installs the seed");
    first_run.record = start::record_seed_failure(start::record_seed_failure({}, "a"), "a");
    check(start::plan_front(first_run) == start::FrontPlan::SafeMode, "two failed installs and nothing installed");
    const start::HostRecord gave_up = start::record_seed_failure(start::record_seed_failure({}, "b"), "b");
    start::FrontFacts broken_seed{false, true, "b", "a", gave_up, 0};
    check(start::plan_front(broken_seed) == start::FrontPlan::ChooseLauncher,
          "a broken seed never blocks the good build");
    start::FrontFacts newer_dmg{false, true, "c", "a", gave_up, 0};
    check(start::plan_front(newer_dmg) == start::FrontPlan::InstallSeed, "another seed starts its own count");
    check(start::record_seed_failure(gave_up, "c") == start::HostRecord{"c", 1},
          "a failure of another seed restarts the count");
    start::FrontFacts dev_build{false, false, std::nullopt, std::nullopt, {}, 0};
    check(start::plan_front(dev_build) == start::FrontPlan::SafeMode, "nothing installed and no seed");
    start::FrontFacts relaunched{false, true, "a", "a", {}, 3};
    check(start::plan_front(relaunched) == start::FrontPlan::SafeMode, "a third lost launcher ends in safe mode");
}

void test_placement_and_records() {
    using start::Placement;
    const std::string translocated = "/private/var/folders/x/T/AppTranslocation/1A2B/d/Parties.app";
    check(start::classify_placement("/Applications/Parties.app", false) == Placement::Installed, "installed");
    check(start::classify_placement("/Volumes/Parties/Parties.app", true) == Placement::ReadOnlyVolume,
          "a mounted DMG");
    check(start::classify_placement("/Volumes/Parties/Parties.app", false) == Placement::Installed,
          "a writable volume is installed");
    check(start::classify_placement(translocated, true) == Placement::Translocated, "translocated");
    check(start::classify_placement(translocated, false) == Placement::Translocated,
          "translocation wins over the volume flag");
    check(start::classify_placement("/Users/u/AppTranslocationNotes/Parties.app", false) == Placement::Installed,
          "only the whole path component counts");

    const start::HostRecord record{std::string(64, 'f'), 2};
    check(start::parse_host_record(start::write_host_record(record)) == record, "host.json round-trips");
    check(start::parse_host_record(start::write_host_record({})) == start::HostRecord{},
          "an empty host.json round-trips");
    check(!start::parse_host_record("{\"seed\":\"a\"}"), "host.json without counters");
    auto huge = start::parse_host_record("{\"seed\":\"a\",\"install_failures\":900,\"move_declined\":false}");
    check(huge && huge->install_failures == 255, "a huge counter saturates");
    check(start::parse_installed_seed("{\"seed\":\"1.0.0.0-abcdef12\",\"entry_sha256\":\"ab\"}") == "ab",
          "seed.json names the entry it processed");
    check(!start::parse_installed_seed("{\"seed\":\"1.0.0.0-abcdef12\"}"), "a seed.json without an entry digest");

    Recovery recovery{"repairing", "/d", {{"previous", "Start the previous version (1.0.0.0)", "Starts on a backup."}}};
    start::Alert alert = start::recovery_alert(recovery);
    check(alert.options.size() == 3 && alert.options[0].id == "previous" && alert.options[1].id == start::kOpenData &&
              alert.options.back().id == start::kQuit,
          "recovery offers the launcher's options, then the data folder and Quit");
    check(alert.message.find("Starts on a backup.") != std::string::npos, "an option's detail is in the message");
    check(start::safe_mode_alert(true).options.front().id == start::kStartLastGood,
          "safe mode offers the last good app first");
    for (const auto& o : start::safe_mode_alert(false).options)
        check(o.id != start::kStartLastGood, "no last good app to start without a contract");
    for (Placement where : {Placement::ReadOnlyVolume, Placement::Translocated}) {
        const Recovery offer = start::move_offer(where);
        check(parse_launcher_message(write(LauncherMessage{offer})) == LauncherMessage{offer}, "a move offer round-trips");
        const start::Alert move = start::recovery_alert(offer);
        check(move.title.starts_with("Move Parties") && move.options.size() == 2 &&
                  move.options.front().id == start::kMove && move.options.back().id == start::kKeep,
              "a move offer shows only Move, the default, and Not Now");
        check(move.message.find(where == Placement::ReadOnlyVolume ? "disk image" : "temporary location") !=
                  std::string::npos,
              "the move offer says where Parties runs from");
    }
}

std::vector<const char*> argv_of(const std::vector<std::string>& args) {
    std::vector<const char*> out;
    for (const auto& a : args) out.push_back(a.c_str());
    return out;
}

void test_host_args() {
    const std::vector<std::string> launcher{"--role=launcher", "--install-root=/r", "--data-root=/d", "--build=builds\\x",
                                            "--boot-session=7", "--boot-ok-fd=3", "--recover", "-psn_0_1"};
    for (bool testing : {false, true}) {
        auto a = start::parse_host_args(argv_of(launcher), testing, "/default-r", "/default-d");
        check(a.launcher && a.install_root == "/r" && a.data_root == "/d" && a.build == "builds\\x" && a.recover,
              "the launcher role takes the roots its front passes");
        check(a.forwarded.empty(), "unknown flags are dropped");
    }
    const std::vector<std::string> front{"--install-root=/r", "--data-root=/d", "--test-namespace=n", "--boot-round=2",
                                         "--after=99", "--ticket=4", "--safe-mode", "--", "--install-root=/user", "x"};
    auto retail = start::parse_host_args(argv_of(front), false, "/default-r", "/default-d");
    check(retail.install_root == "/default-r" && retail.data_root == "/default-d",
          "a retail front ignores roots on its command line");
    check(retail.test_namespace.empty() && retail.testing.empty(), "a retail front takes no test flags");
    check(retail.boot_round == 2 && retail.after == 99 && retail.ticketed && retail.safe_mode, "front flags");
    check(retail.forwarded == std::vector<std::string>{"--install-root=/user", "x"}, "the user's arguments after --");
    auto testing = start::parse_host_args(argv_of(front), true, "/default-r", "/default-d");
    check(testing.install_root == "/r" && testing.data_root == "/d" && testing.test_namespace == "n",
          "a testing front takes its roots and namespace");
    check(testing.testing == std::vector<std::string>{"--install-root=/r", "--data-root=/d", "--test-namespace=n"},
          "the testing flags it passes on");
    check(start::parse_host_args(argv_of({"--boot-round=x"}), false, "/r", "/d").boot_round == 0,
          "an unreadable round is round 0");

    const auto relaunch = start::relaunch_arguments(testing, {"--boot-round=3"}, 42);
    check(relaunch == std::vector<std::string>{"--boot-round=3", "--after=42", "--install-root=/r", "--data-root=/d",
                                               "--test-namespace=n", "--", "--install-root=/user", "x"},
          "a relaunch keeps the testing flags and the user's arguments");
    const auto spawn = start::launcher_arguments(testing, "/A/Parties", {"--install-seed=/A"});
    check(spawn == std::vector<std::string>{"/A/Parties", "--role=launcher", "--install-root=/r", "--data-root=/d",
                                            "--install-seed=/A", "--install-root=/r", "--data-root=/d",
                                            "--test-namespace=n"},
          "a launcher gets the roots, its role, then the testing flags");
}

boot::BootContract contract_with(bool previous) {
    boot::BootContract c{{"builds\\good", sha('1')}, std::nullopt, {"builds\\good", sha('a')}, "/d"};
    if (previous) c.previous_good = boot::LauncherRef{"builds\\prev", sha('2')};
    return c;
}

// The stub's loop in updater/boot/main.cpp run(), replayed over scripted
// launcher outcomes: the dirs it starts, then whether it ends in recovery.
std::vector<std::string> stub_runs(const boot::BootContract& c, const std::vector<char>& outcomes) {
    boot::BootRecord record;
    std::vector<Sha256> unusable;
    std::vector<std::string> runs;
    size_t next = 0;
    for (int failures = 0;;) {
        auto d = boot::choose(c, record, std::nullopt, unusable);
        const auto* run = std::get_if<boot::RunLauncher>(&d.choice);
        if (!run || next == outcomes.size()) break;
        runs.push_back(run->dir);
        const char outcome = outcomes[next++];
        if (outcome == 'u') {
            unusable.push_back(run->sha256);
            continue;
        }
        if (outcome == 'o') return runs;
        record = boot::record_boot_failure(d.next_record);
        if (++failures > 2) break;
    }
    runs.push_back("recovery");
    return runs;
}

std::vector<std::string> choice_runs(const boot::BootContract& c, const std::vector<char>& outcomes) {
    start::LauncherChoice choice(c, {}, std::nullopt);
    std::vector<std::string> runs;
    size_t next = 0;
    while (auto run = choice.next()) {
        if (next == outcomes.size()) break;
        runs.push_back(run->dir);
        const char outcome = outcomes[next++];
        if (outcome == 'u') choice.unusable();
        else if (outcome == 'o') return runs;
        else choice.failed();
    }
    runs.push_back("recovery");
    return runs;
}

void test_launcher_choice() {
    int cases = 0;
    // Every sequence of up to six outcomes: f failed, u unusable, o listened.
    std::vector<std::vector<char>> sequences{{}};
    for (size_t len = 0; len < 6; ++len) {
        const size_t from = sequences.size();
        for (size_t i = 0; i < from; ++i)
            if (sequences[i].size() == len)
                for (char o : {'f', 'u', 'o'}) {
                    auto s = sequences[i];
                    s.push_back(o);
                    sequences.push_back(s);
                }
    }
    for (bool previous : {false, true})
        for (const auto& outcomes : sequences) {
            const auto c = contract_with(previous);
            check(choice_runs(c, outcomes) == stub_runs(c, outcomes),
                  "LauncherChoice decides as Parties.exe: " + std::string(outcomes.begin(), outcomes.end()) +
                      (previous ? " with previous_good" : ""));
            ++cases;
        }
    std::printf("launcher choice: %d sequences\n", cases);

    const auto c = contract_with(true);
    check(choice_runs(c, {'f', 'f', 'o'}) == std::vector<std::string>{"builds\\good", "builds\\good", "builds\\prev"},
          "two failed boots of good fall back to previous_good");
    check(choice_runs(c, {'u', 'o'}) == std::vector<std::string>{"builds\\good", "builds\\prev"},
          "an unusable good is passed over at once");
    check(choice_runs(c, {'u', 'u'}) == std::vector<std::string>{"builds\\good", "builds\\prev", "recovery"},
          "both unusable: safe mode");
    start::LauncherChoice failing(c, {}, std::nullopt);
    for (int i = 0; i < 3; ++i) {
        failing.next();
        failing.failed();
    }
    check(!failing.next() && !failing.record().attempt, "after three failed boots no attempt is left open");
    check(start::pinned_launcher(c, "builds\\prev") == sha('2') && start::pinned_launcher(c, "builds\\good") == sha('1') &&
              !start::pinned_launcher(c, "builds\\other"),
          "only good and previous_good launchers are pinned");
}

} // namespace

int main() {
    test_classify_exhaustive();
    test_route_exhaustive();
    test_codec();
    test_plan_front_exhaustive();
    test_placement_and_records();
    test_host_args();
    test_launcher_choice();
    if (failures) {
        std::fprintf(stderr, "update_front_door_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("update_front_door_test: ok\n");
    return 0;
}
