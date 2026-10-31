#include <parties/update/link.h>

#include "json_util.h"

#include <algorithm>
#include <array>
#include <type_traits>
#include <utility>

namespace parties::update::link {

namespace {

template <class E, size_t N>
using Names = std::array<std::pair<E, std::string_view>, N>;

constexpr Names<ChannelChoice, 3> kChoices{{
    {ChannelChoice::Follow, "follow"},
    {ChannelChoice::Wait, "wait"},
    {ChannelChoice::DowngradeNow, "downgrade_now"},
}};
constexpr Names<Phase, 7> kPhases{{
    {Phase::Idle, "idle"},
    {Phase::Checking, "checking"},
    {Phase::Downloading, "downloading"},
    {Phase::ReadyToRestart, "ready_to_restart"},
    {Phase::Pinned, "pinned"},
    {Phase::FeedStale, "feed_stale"},
    {Phase::Error, "error"},
}};
constexpr Names<DowngradeEffect, 3> kEffects{{
    {DowngradeEffect::None, "none"},
    {DowngradeEffect::RestoresBackup, "restores_backup"},
    {DowngradeEffect::Impossible, "impossible"},
}};
constexpr Names<StatusNotice::Kind, 2> kNoticeKinds{{
    {StatusNotice::Kind::RolledBack, "rolled_back"},
    {StatusNotice::Kind::DataRestored, "data_restored"},
}};

template <class E, size_t N>
std::string_view name_of(const Names<E, N>& names, E value) {
    for (const auto& [v, n] : names)
        if (v == value) return n;
    return names[0].second;
}

template <class E, size_t N>
std::optional<E> value_of(const Names<E, N>& names, std::optional<std::string_view> text) {
    if (!text) return std::nullopt;
    for (const auto& [v, n] : names)
        if (n == *text) return v;
    return std::nullopt;
}

using json::Value;
using json::Writer;

void write_version(Writer& w, std::string_view k, const Version& v) {
    json::key(w, k);
    json::str(w, v.to_string());
}

std::optional<Version> version_at(const Value& obj, const char* name) {
    auto text = json::string(obj, name);
    return text ? Version::parse(*text) : std::nullopt;
}

template <class F>
std::string document(std::string_view type, F&& fields) {
    rapidjson::StringBuffer buffer;
    Writer w(buffer);
    w.StartObject();
    json::key(w, "type");
    json::str(w, type);
    fields(w);
    w.EndObject();
    std::string line(buffer.GetString(), buffer.GetSize());
    line.push_back('\n');
    return line;
}

std::string write_request(const Request& request) {
    return document("request", [&](Writer& w) {
        std::visit(
            [&](const auto& r) {
                using T = std::decay_t<decltype(r)>;
                json::key(w, "kind");
                if constexpr (std::is_same_v<T, RestartToUpdate>) json::str(w, "restart_to_update");
                else if constexpr (std::is_same_v<T, CheckNow>) json::str(w, "check_now");
                else if constexpr (std::is_same_v<T, AcknowledgeNotice>) json::str(w, "acknowledge_notice");
                else {
                    json::str(w, "set_channel");
                    json::key(w, "channel");
                    json::str(w, channel_name(r.channel));
                    json::key(w, "choice");
                    json::str(w, name_of(kChoices, r.choice));
                }
            },
            request);
    });
}

std::optional<Request> parse_request(const Value& doc) {
    auto kind = json::string(doc, "kind");
    if (!kind) return std::nullopt;
    if (*kind == "restart_to_update") return RestartToUpdate{};
    if (*kind == "check_now") return CheckNow{};
    if (*kind == "acknowledge_notice") return AcknowledgeNotice{};
    if (*kind == "set_channel") {
        auto channel_text = json::string(doc, "channel");
        auto channel = channel_text ? parse_channel(*channel_text) : std::nullopt;
        auto choice = value_of(kChoices, json::string(doc, "choice"));
        if (!channel || !choice) return std::nullopt;
        return SetChannel{*channel, *choice};
    }
    return std::nullopt;
}

std::string write_status(const Status& s) {
    return document("status", [&](Writer& w) {
        json::key(w, "phase");
        json::str(w, name_of(kPhases, s.phase));
        json::key(w, "channel");
        json::str(w, channel_name(s.channel));
        write_version(w, "current", s.current);
        if (s.available) write_version(w, "available", *s.available);
        if (s.percent) {
            json::key(w, "percent");
            w.Uint(*s.percent);
        }
        if (s.pinned_until) write_version(w, "pinned_until", *s.pinned_until);
        if (s.downgrade) {
            json::key(w, "downgrade");
            w.StartObject();
            write_version(w, "version", s.downgrade->version);
            json::key(w, "data_effect");
            json::str(w, name_of(kEffects, s.downgrade->data_effect));
            w.EndObject();
        }
        if (s.notice) {
            json::key(w, "notice");
            w.StartObject();
            json::key(w, "kind");
            json::str(w, name_of(kNoticeKinds, s.notice->kind));
            write_version(w, "from", s.notice->from);
            write_version(w, "to", s.notice->to);
            if (s.notice->preserved) {
                json::key(w, "preserved");
                json::str(w, *s.notice->preserved);
            }
            w.EndObject();
        }
    });
}

std::optional<Status> parse_status(const Value& doc) {
    Status s;
    auto phase = value_of(kPhases, json::string(doc, "phase"));
    auto channel_text = json::string(doc, "channel");
    auto channel = channel_text ? parse_channel(*channel_text) : std::nullopt;
    auto current = version_at(doc, "current");
    if (!phase || !channel || !current) return std::nullopt;
    s.phase = *phase;
    s.channel = *channel;
    s.current = *current;
    s.available = version_at(doc, "available");
    if (auto percent = json::u32(doc, "percent"); percent && *percent <= 100)
        s.percent = static_cast<uint8_t>(*percent);
    s.pinned_until = version_at(doc, "pinned_until");
    if (const Value* d = json::object(doc, "downgrade")) {
        auto version = version_at(*d, "version");
        auto effect = value_of(kEffects, json::string(*d, "data_effect"));
        if (version && effect) s.downgrade = DowngradeOffer{*version, *effect};
    }
    if (const Value* n = json::object(doc, "notice")) {
        auto kind = value_of(kNoticeKinds, json::string(*n, "kind"));
        auto from = version_at(*n, "from");
        auto to = version_at(*n, "to");
        if (kind && from && to) {
            StatusNotice notice{*kind, *from, *to, std::nullopt};
            if (auto preserved = json::string(*n, "preserved")) notice.preserved = std::string(*preserved);
            s.notice = std::move(notice);
        }
    }
    return s;
}

bool parse_document(rapidjson::Document& doc, std::string_view line, std::string_view& type) {
    if (!json::parse(doc, line)) return false;
    auto t = json::string(doc, "type");
    if (!t) return false;
    type = *t;
    return true;
}

} // namespace

std::optional<uint32_t> negotiate(const Hello& hello, uint32_t launcher_protocol) {
    uint32_t k = std::min(hello.link_protocol, launcher_protocol);
    if (k < hello.min_launcher_link || k == 0) return std::nullopt;
    return k;
}

std::string write(const AppMessage& message) {
    return std::visit(
        [](const auto& m) -> std::string {
            using T = std::decay_t<decltype(m)>;
            if constexpr (std::is_same_v<T, Hello>) {
                return document("hello", [&](Writer& w) {
                    json::key(w, "link_protocol");
                    w.Uint(m.link_protocol);
                    json::key(w, "min_launcher_link");
                    w.Uint(m.min_launcher_link);
                    write_version(w, "version", m.version);
                });
            } else if constexpr (std::is_same_v<T, Healthy>) {
                return document("healthy", [&](Writer& w) {
                    json::key(w, "data_schema");
                    w.Uint(m.data_schema);
                });
            } else {
                return write_request(m);
            }
        },
        message);
}

std::string write(const LauncherMessage& message) {
    return std::visit(
        [](const auto& m) -> std::string {
            using T = std::decay_t<decltype(m)>;
            if constexpr (std::is_same_v<T, Welcome>) {
                return document("welcome", [&](Writer& w) {
                    json::key(w, "link_protocol");
                    w.Uint(m.link_protocol);
                });
            } else if constexpr (std::is_same_v<T, Status>) {
                return write_status(m);
            } else {
                return document("exit_requested", [](Writer&) {});
            }
        },
        message);
}

std::optional<AppMessage> parse_app_message(std::string_view line) {
    rapidjson::Document doc;
    std::string_view type;
    if (!parse_document(doc, line, type)) return std::nullopt;
    if (type == "hello") {
        auto protocol = json::u32(doc, "link_protocol");
        auto min_link = json::u32(doc, "min_launcher_link");
        auto version = version_at(doc, "version");
        if (!protocol || !min_link || !version) return std::nullopt;
        return Hello{*protocol, *min_link, *version};
    }
    if (type == "healthy") {
        auto schema = json::u32(doc, "data_schema");
        if (!schema) return std::nullopt;
        return Healthy{*schema};
    }
    if (type == "request") {
        if (auto request = parse_request(doc)) return *request;
    }
    return std::nullopt;
}

std::optional<LauncherMessage> parse_launcher_message(std::string_view line) {
    rapidjson::Document doc;
    std::string_view type;
    if (!parse_document(doc, line, type)) return std::nullopt;
    if (type == "welcome") {
        auto protocol = json::u32(doc, "link_protocol");
        if (!protocol) return std::nullopt;
        return Welcome{*protocol};
    }
    if (type == "status") {
        if (auto status = parse_status(doc)) return *status;
        return std::nullopt;
    }
    if (type == "exit_requested") return ExitRequested{};
    return std::nullopt;
}

void LineReader::append(std::span<const char> bytes) {
    for (char c : bytes) {
        if (discarding_) {
            if (c == '\n') discarding_ = false;
            continue;
        }
        buffer_.push_back(c);
        if (c == '\n') {
            partial_ = 0;
        } else if (++partial_ > kMaxLine) {
            buffer_.resize(buffer_.size() - partial_);
            partial_ = 0;
            discarding_ = true;
        }
    }
}

std::optional<std::string> LineReader::next() {
    size_t end = buffer_.find('\n');
    if (end == std::string::npos) return std::nullopt;
    std::string line = buffer_.substr(0, end);
    buffer_.erase(0, end + 1);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    return line;
}

} // namespace parties::update::link
