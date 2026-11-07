#include <parties/update/front_door.h>
#include <parties/update/front_door_policy.h>

#include <climits>
#include <cstdio>
#include <string>
#include <vector>

using namespace parties::update;
using namespace parties::update::front_door;

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

} // namespace

int main() {
    test_classify_exhaustive();
    test_route_exhaustive();
    test_codec();
    if (failures) {
        std::fprintf(stderr, "update_front_door_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("update_front_door_test: ok\n");
    return 0;
}
