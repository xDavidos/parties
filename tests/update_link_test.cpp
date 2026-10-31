// The launcher <-> app link codec: both directions round-trip, unknown types
// and fields are ignored, and version negotiation follows contracts.md.

#include <parties/update/link.h>

#include <iostream>
#include <string>

using namespace parties::update;
using namespace parties::update::link;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::cerr << "update_link_test: " << what << "\n";
        ++failures;
    }
}

Version v(const char* text) { return *Version::parse(text); }

template <class M, class Parse>
bool round_trips(const M& message, Parse parse) {
    std::string line = write(message);
    if (line.empty() || line.back() != '\n') return false;
    line.pop_back();
    auto parsed = parse(line);
    return parsed && *parsed == message;
}

} // namespace

int main() {
    auto app = [](std::string_view l) { return parse_app_message(l); };
    auto launcher = [](std::string_view l) { return parse_launcher_message(l); };

    check(round_trips(AppMessage{Hello{1, 1, v("1.6.0.0")}}, app), "hello round-trips");
    check(round_trips(AppMessage{Healthy{3}}, app), "healthy round-trips");
    check(round_trips(AppMessage{Request{RestartToUpdate{}}}, app), "restart_to_update round-trips");
    check(round_trips(AppMessage{Request{CheckNow{}}}, app), "check_now round-trips");
    check(round_trips(AppMessage{Request{AcknowledgeNotice{}}}, app), "acknowledge_notice round-trips");
    check(round_trips(AppMessage{Request{SetChannel{Channel::Beta, ChannelChoice::DowngradeNow}}}, app),
          "set_channel round-trips");

    Status full;
    full.phase = Phase::ReadyToRestart;
    full.channel = Channel::Beta;
    full.current = v("1.5.2.0");
    full.available = v("1.6.0.0");
    full.percent = 100;
    full.pinned_until = v("1.6.0.0");
    full.downgrade = DowngradeOffer{v("1.5.0.0"), DowngradeEffect::RestoresBackup,
                                    std::chrono::sys_days{std::chrono::year{2026} / 10 / 30} + std::chrono::hours{9}};
    full.notice = StatusNotice{StatusNotice::Kind::RolledBack, v("1.6.0.0"), v("1.5.2.0"), "rolled-back-x"};
    check(round_trips(LauncherMessage{full}, launcher), "full status round-trips");
    check(round_trips(LauncherMessage{Status{}}, launcher), "minimal status round-trips");
    check(write(LauncherMessage{full}).find("\"backup_taken_at\":\"2026-10-30T09:00:00Z\"") != std::string::npos,
          "backup_taken_at is ISO-8601 UTC");
    auto odd = parse_launcher_message(R"({"type":"status","phase":"idle","channel":"stable","current":"1.0.0.0",)"
                                      R"("downgrade":{"version":"0.9.0.0","data_effect":"restores_backup","backup_taken_at":"yesterday"}})");
    check(odd && std::get<Status>(*odd).downgrade && !std::get<Status>(*odd).downgrade->backup_taken_at,
          "an unreadable backup_taken_at drops only that field");
    check(round_trips(LauncherMessage{Welcome{1}}, launcher), "welcome round-trips");
    check(round_trips(LauncherMessage{ExitRequested{}}, launcher), "exit_requested round-trips");

    check(write(LauncherMessage{ExitRequested{}}) == "{\"type\":\"exit_requested\"}\n", "exit_requested wire form");

    auto hello = parse_app_message(R"({"type":"hello","link_protocol":4,"min_launcher_link":1,"version":"2.0.0.0","extra":[1]})");
    check(hello && std::get<Hello>(*hello).link_protocol == 4, "unknown fields are ignored");
    check(!parse_app_message(R"({"type":"telemetry","x":1})"), "unknown app message types are skipped");
    check(!parse_launcher_message(R"({"type":"future_thing"})"), "unknown launcher message types are skipped");
    check(!parse_app_message(R"({"type":"request","kind":"self_destruct"})"), "unknown request kinds are skipped");
    check(!parse_app_message("not json"), "malformed lines are skipped");
    check(!parse_app_message(R"({"link_protocol":1})"), "a message without a type is skipped");
    check(!parse_app_message(R"({"type":"hello","link_protocol":1,"min_launcher_link":1,"version":"v1.0"})"),
          "a hello with a bad version is skipped");

    check(negotiate(Hello{3, 1, v("1.0.0.0")}, 1) == 1u, "welcome is min(app, launcher)");
    check(negotiate(Hello{1, 1, v("1.0.0.0")}, 5) == 1u, "an older app pins the protocol");
    check(!negotiate(Hello{3, 2, v("1.0.0.0")}, 1), "a launcher below the app's minimum is incompatible");

    LineReader reader;
    reader.append(std::string_view("{\"a\":1}\n{\"b\""));
    check(reader.next() == std::string("{\"a\":1}"), "first line splits out");
    check(!reader.next(), "a partial line waits");
    reader.append(std::string_view(":2}\r\n"));
    check(reader.next() == std::string("{\"b\":2}"), "CRLF is accepted");

    std::string huge(LineReader::kMaxLine + 10, 'x');
    reader.append(huge);
    reader.append(std::string_view("tail\n{\"ok\":1}\n"));
    check(reader.next() == std::string("{\"ok\":1}"), "an oversized line is dropped whole");

    LineReader many;
    std::string burst;
    for (int i = 0; i < 2000; ++i) burst += "{\"n\":" + std::to_string(i) + "}\n";
    many.append(burst);
    int count = 0;
    while (many.next()) ++count;
    check(count == 2000, "a burst of short lines is never treated as one long line");

    if (failures) return 1;
    std::cout << "update_link_test: ok\n";
    return 0;
}
