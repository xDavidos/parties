// The managed update UI: a launcher status in, what the user sees and the
// link request each action sends out.

#include <client/update_view.h>

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <string>

using namespace parties::client;
using namespace parties::update;
using namespace parties::update::link;
using namespace std::chrono_literals;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::cerr << "update_view_test: " << what << "\n";
        ++failures;
    }
}

Version v(const char* text) { return *Version::parse(text); }

bool contains(const Rml::String& text, const char* part) { return text.find(part) != Rml::String::npos; }

// POSIX-style zone, positive west of UTC: "UTC0", "EST5".
void use_time_zone(const char* tz) {
#ifdef _WIN32
    _putenv_s("TZ", tz);
    _tzset();
#else
    setenv("TZ", tz, 1);
    tzset();
#endif
}

constexpr std::chrono::sys_seconds kBackupTaken{std::chrono::sys_days{std::chrono::year{2026} / 10 / 30} + 14h + 5min};
constexpr std::chrono::sys_seconds kNextDay{std::chrono::sys_days{std::chrono::year{2026} / 10 / 31} + 9h};

Status base() {
    Status s;
    s.current = v("1.2.0.0");
    return s;
}

Status pinned_on_beta(DowngradeEffect effect) {
    Status s = base();
    s.phase = Phase::Pinned;
    s.channel = Channel::Beta;
    s.pinned_until = v("1.2.0.0");
    s.downgrade = DowngradeOffer{v("1.1.0.0"), effect};
    return s;
}

bool sends(const Status& s, UpdateAction action, const Request& expected) {
    auto request = update_request(s, action);
    return request && *request == expected;
}

void channel_choice() {
    const Status stable = base();
    check(sends(stable, UpdateAction::ChooseBeta, SetChannel{Channel::Beta, ChannelChoice::Wait}),
          "choosing Beta on Stable switches and waits for Beta to catch up");
    check(!update_request(stable, UpdateAction::ChooseStable), "choosing the current channel sends nothing");

    const Status beta = pinned_on_beta(DowngradeEffect::None);
    check(sends(beta, UpdateAction::ChooseStable, SetChannel{Channel::Stable, ChannelChoice::Wait}),
          "choosing Stable while held on Beta switches back");
}

void switch_now() {
    for (DowngradeEffect effect : {DowngradeEffect::None, DowngradeEffect::RestoresBackup}) {
        const Status s = pinned_on_beta(effect);
        LobbyModel model;
        show_update_status(model, s);
        check(model.update_can_switch.get(), "a data-compatible or restorable downgrade is offered");
        check(sends(s, UpdateAction::SwitchNow, SetChannel{Channel::Beta, ChannelChoice::DowngradeNow}),
              "Switch now asks for the downgrade on the chosen channel");
        check(contains(model.update_hold.get(), "1.1.0.0") && contains(model.update_hold.get(), "1.2.0.0") &&
                  contains(model.update_hold.get(), "until Beta catches up"),
              "the hold names both versions and the channel");
    }

    LobbyModel restore;
    Status dated = pinned_on_beta(DowngradeEffect::RestoresBackup);
    show_update_status(restore, dated, kNextDay);
    check(restore.update_switch_note.get() ==
              "Switching now restores an earlier backup of your data. Changes made since that backup stay saved "
              "in a separate backup file and are not shown.",
          "a restoring switch without a backup date keeps the general wording");

    dated.downgrade->backup_taken_at = kBackupTaken;
    use_time_zone("UTC0");
    show_update_status(restore, dated, kNextDay);
    check(restore.update_switch_note.get() ==
              "Switching now restores your data from the backup made on Oct 30, 14:05. Changes made since then "
              "stay saved in a separate backup file and are not shown.",
          "a restoring switch names the backup's date and where newer changes are kept");
    use_time_zone("EST5");
    show_update_status(restore, dated, kNextDay);
    check(contains(restore.update_switch_note.get(), "backup made on Oct 30, 09:05."),
          "the backup's date is shown in local time");
    show_update_status(restore, dated, kNextDay + std::chrono::days{70});
    check(contains(restore.update_switch_note.get(), "backup made on Oct 30, 2026, 09:05."),
          "a backup from another year names the year");

    const Status impossible = pinned_on_beta(DowngradeEffect::Impossible);
    LobbyModel model;
    show_update_status(model, impossible);
    check(!model.update_can_switch.get() && model.update_switch_note.get().empty(),
          "an impossible downgrade is never offered");
    check(!model.update_hold.get().empty(), "an impossible downgrade still explains the hold");
    check(!update_request(impossible, UpdateAction::SwitchNow), "an impossible downgrade is never requested");

    Status following_older = base();
    following_older.channel = Channel::Beta;
    following_older.downgrade = DowngradeOffer{v("1.1.0.0"), DowngradeEffect::None};
    show_update_status(model, following_older);
    check(contains(model.update_hold.get(), "Parties stays on 1.2.0.0"),
          "an older channel without a pin is explained the same way");

    Status downloading = pinned_on_beta(DowngradeEffect::None);
    downloading.phase = Phase::Downloading;
    downloading.percent = 10;
    show_update_status(model, downloading);
    check(model.update_hold.get().empty() && !update_request(downloading, UpdateAction::SwitchNow),
          "no hold while a newer build downloads");
}

void pill() {
    LobbyModel model;
    Status idle = base();
    show_update_status(model, idle);
    check(!model.update_available.get() && model.update_status_line.get() == "Up to date",
          "nothing in the titlebar when up to date");

    Status blocked = base();
    blocked.available = v("1.3.0.0");
    show_update_status(model, blocked);
    check(!model.update_available.get() && !update_request(blocked, UpdateAction::ClickPill),
          "a newer build the launcher cannot stage gets no titlebar pill");
    check(contains(model.update_status_line.get(), "Version 1.3.0.0 is out, but Parties can't install it automatically"),
          "Settings says a newer build exists that needs the installer");

    Status same = base();
    same.available = v("1.2.0.0");
    show_update_status(model, same);
    check(!model.update_available.get(), "the installed version is never offered as an update");

    Status downloading = base();
    downloading.phase = Phase::Downloading;
    downloading.available = v("1.3.0.0");
    downloading.percent = 42;
    show_update_status(model, downloading);
    check(model.update_downloading.get() && model.update_percent.get() == 42, "download progress is shown");
    check(model.update_status_line.get() == "Downloading 1.3.0.0 · 42%", "Settings names the build being downloaded");
    check(!update_request(downloading, UpdateAction::ClickPill), "clicking progress sends nothing");
    downloading.available.reset();
    downloading.percent.reset();
    show_update_status(model, downloading);
    check(model.update_status_line.get() == "Downloading the update", "a download without details still reads right");

    Status ready = base();
    ready.phase = Phase::ReadyToRestart;
    ready.available = v("1.3.0.0");
    show_update_status(model, ready);
    check(model.update_ready.get() && model.update_percent.get() == -1, "a staged build asks for a restart");
    check(sends(ready, UpdateAction::ClickPill, RestartToUpdate{}), "the ready pill restarts into the update");

    for (Phase quiet : {Phase::Checking, Phase::Pinned, Phase::FeedStale, Phase::Error}) {
        Status s = base();
        s.phase = quiet;
        show_update_status(model, s);
        check(!model.update_available.get(), "no titlebar alarm for checking, pinned, a stale feed or an error");
    }

    Status stale = base();
    stale.phase = Phase::FeedStale;
    show_update_status(model, stale);
    check(contains(model.update_status_line.get(), "paused"), "a stale feed explains that updates are paused");
}

void notice() {
    LobbyModel model;
    Status rolled = base();
    rolled.notice = StatusNotice{StatusNotice::Kind::RolledBack, v("1.3.0.0"), v("1.2.0.0"), std::nullopt};
    show_update_status(model, rolled);
    check(model.update_notice.get() ==
              "Parties 1.3.0.0 didn't start correctly, so you're back on 1.2.0.0. Your data is safe.",
          "a rollback says what happened and that data is safe");
    check(model.update_notice_detail.get().empty(), "a plain rollback has no backup to point at");
    check(sends(rolled, UpdateAction::DismissNotice, AcknowledgeNotice{}), "dismissing acknowledges the notice");

    Status restored = rolled;
    restored.notice->kind = StatusNotice::Kind::DataRestored;
    restored.notice->preserved = "backups\\rolled-back-1.3.0.0-9f3c2a1b-g12.db";
    show_update_status(model, restored);
    check(model.update_notice_detail.get() ==
              "Changes made while 1.3.0.0 ran are saved in your Parties data folder as "
              "backups\\rolled-back-1.3.0.0-9f3c2a1b-g12.db.",
          "a data restore names the preserved backup file inside the data folder");

    show_update_status(model, base());
    check(model.update_notice.get().empty(), "the notice disappears once the launcher clears it");
    check(!update_request(base(), UpdateAction::DismissNotice), "nothing to acknowledge without a notice");
}

} // namespace

int main() {
    channel_choice();
    switch_now();
    pill();
    notice();
    if (failures == 0) std::cout << "update_view_test: all checks passed\n";
    return failures == 0 ? 0 : 1;
}
