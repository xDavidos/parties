#include <client/update_view.h>

#include <string>

namespace parties::client {

namespace {

using update::Channel;
using update::link::ChannelChoice;
using update::link::DowngradeEffect;
using update::link::Phase;
using update::link::Status;
using update::link::StatusNotice;

enum class Pill { None, Available, Downloading, Ready };

Pill pill_for(const Status& s) {
    switch (s.phase) {
    case Phase::Downloading:    return Pill::Downloading;
    case Phase::ReadyToRestart: return Pill::Ready;
    case Phase::Idle:
    case Phase::Checking:       return s.available && *s.available > s.current ? Pill::Available : Pill::None;
    default:                    return Pill::None;
    }
}

// Parties keeps the installed version because the chosen channel has nothing
// as new yet: the launcher pinned it, or offers only an older build.
bool held(const Status& s) {
    if (s.phase == Phase::Downloading || s.phase == Phase::ReadyToRestart) return false;
    return s.pinned_until || s.downgrade;
}

bool switch_offered(const Status& s) {
    return held(s) && s.downgrade && s.downgrade->data_effect != DowngradeEffect::Impossible;
}

std::string channel_title(Channel channel) { return channel == Channel::Beta ? "Beta" : "Stable"; }

std::string status_line(const Status& s) {
    switch (s.phase) {
    case Phase::Checking:
        return "Checking for updates";
    case Phase::Downloading:
        return s.percent ? "Downloading the update (" + std::to_string(*s.percent) + "%)" : "Downloading the update";
    case Phase::ReadyToRestart:
        return s.available ? "Version " + s.available->to_string() + " is ready. It installs when you restart."
                           : "An update is ready. It installs when you restart.";
    case Phase::FeedStale:
        return "Updates are paused because the update feed is out of date.";
    case Phase::Error:
        return "The last update check failed. Parties will try again later.";
    case Phase::Idle:
    case Phase::Pinned:
        break;
    }
    if (pill_for(s) == Pill::Available) return "Version " + s.available->to_string() + " is available";
    return "Up to date";
}

std::string hold_text(const Status& s) {
    if (!held(s)) return {};
    const std::string channel = channel_title(s.channel);
    const std::string kept = s.pinned_until.value_or(s.current).to_string();
    const std::string wait = " Parties stays on " + kept + " until " + channel + " catches up.";
    if (s.downgrade)
        return channel + "'s newest version is " + s.downgrade->version.to_string() + ", older than the " + kept +
               " you have." + wait;
    return channel + " has no version as new as " + kept + " yet." + wait;
}

std::string switch_note(const Status& s) {
    if (!switch_offered(s)) return {};
    if (s.downgrade->data_effect == DowngradeEffect::RestoresBackup)
        return "Switching now restores an earlier backup of your data. Changes made since that backup stay saved "
               "in a separate backup file and are not shown.";
    return "Your data stays as it is.";
}

std::string notice_text(const Status& s) {
    if (!s.notice) return {};
    return "Parties " + s.notice->from.to_string() + " didn't start correctly, so you're back on " +
           s.notice->to.to_string() + ". Your data is safe.";
}

std::string notice_detail(const Status& s) {
    if (!s.notice || s.notice->kind != StatusNotice::Kind::DataRestored || !s.notice->preserved) return {};
    return "Changes made while " + s.notice->from.to_string() + " ran are saved in the backup file " +
           *s.notice->preserved + ".";
}

} // namespace

void show_update_status(LobbyModel& model, const Status& status) {
    const Pill pill = pill_for(status);
    model.update_available = pill != Pill::None;
    model.update_downloading = pill == Pill::Downloading;
    model.update_ready = pill == Pill::Ready;
    model.update_version = status.available ? "v" + status.available->to_string() : std::string();
    model.update_percent = pill == Pill::Downloading ? status.percent.value_or(0) : -1;

    model.update_managed = true;
    model.update_current = status.current.to_string();
    model.update_channel = status.channel == Channel::Beta ? 1 : 0;
    model.update_status_line = status_line(status);
    model.update_hold = hold_text(status);
    model.update_can_switch = switch_offered(status);
    model.update_switch_note = switch_note(status);
    model.update_notice = notice_text(status);
    model.update_notice_detail = notice_detail(status);
}

std::optional<update::link::Request> update_request(const Status& status, UpdateAction action) {
    switch (action) {
    case UpdateAction::ClickPill:
        if (pill_for(status) == Pill::Available) return update::link::CheckNow{};
        if (pill_for(status) == Pill::Ready) return update::link::RestartToUpdate{};
        return std::nullopt;
    case UpdateAction::ChooseStable:
    case UpdateAction::ChooseBeta: {
        const Channel picked = action == UpdateAction::ChooseBeta ? Channel::Beta : Channel::Stable;
        if (picked == status.channel) return std::nullopt;
        // Waiting is the safe default: a channel whose newest build is older
        // never replaces the installed one unless the user asks for it.
        return update::link::SetChannel{picked, ChannelChoice::Wait};
    }
    case UpdateAction::SwitchNow:
        if (!switch_offered(status)) return std::nullopt;
        return update::link::SetChannel{status.channel, ChannelChoice::DowngradeNow};
    case UpdateAction::DismissNotice:
        if (!status.notice) return std::nullopt;
        return update::link::AcknowledgeNotice{};
    }
    return std::nullopt;
}

} // namespace parties::client
