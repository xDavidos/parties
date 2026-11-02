#include <parties/update/state_codec.h>

#include "encoding.h"
#include "json_util.h"

#include <algorithm>
#include <array>
#include <utility>

namespace parties::update {

namespace {

using json::Value;
using json::Writer;

template <class E, size_t N>
using Names = std::array<std::pair<E, std::string_view>, N>;

constexpr Names<FailureReason, 10> kReasons{{
    {FailureReason::CrashedBeforeHealthy, "crashed_before_healthy"},
    {FailureReason::InitFailed, "init_failed"},
    {FailureReason::HungBeforeHealthy, "hung_before_healthy"},
    {FailureReason::TerminatedBeforeHealthy, "terminated_before_healthy"},
    {FailureReason::DataTooNew, "data_too_new"},
    {FailureReason::CrashedRepeatedlyAfterHealthy, "crashed_repeatedly_after_healthy"},
    {FailureReason::UnknownOutcomeRepeated, "unknown_outcome_repeated"},
    {FailureReason::Corrupt, "corrupt"},
    {FailureReason::Revoked, "revoked"},
    {FailureReason::UserRolledBack, "user_rolled_back"},
}};

constexpr Names<Notice::Kind, 2> kNoticeKinds{{
    {Notice::Kind::RolledBack, "rolled_back"},
    {Notice::Kind::DataRestored, "data_restored"},
}};

template <class E, size_t N>
std::string_view name_of(const Names<E, N>& names, E value) {
    return std::ranges::find(names, value, &std::pair<E, std::string_view>::first)->second;
}

template <class E, size_t N>
std::optional<E> value_of(const Names<E, N>& names, std::optional<std::string_view> text) {
    if (!text) return std::nullopt;
    auto it = std::ranges::find(names, *text, &std::pair<E, std::string_view>::second);
    if (it == names.end()) return std::nullopt;
    return it->first;
}

bool is_safe_stem(std::string_view stem) {
    if (stem.empty() || stem.size() > 200 || stem.front() == '.' || stem.find("..") != std::string_view::npos)
        return false;
    return std::ranges::all_of(stem, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
               c == '_' || c == '-';
    });
}

// --- Writing ---

void write_id(Writer& w, std::string_view k, const BuildId& id) {
    json::key(w, k);
    w.StartObject();
    json::key(w, "version"); json::str(w, id.version.to_string());
    json::key(w, "digest"); json::str(w, id.digest.to_hex());
    w.EndObject();
}

void write_data(Writer& w, std::string_view k, const DataInfo& d) {
    json::key(w, k);
    w.StartObject();
    json::key(w, "version"); w.Uint(d.version);
    json::key(w, "compat_floor"); w.Uint(d.compat_floor);
    w.EndObject();
}

void write_snapshot(Writer& w, std::string_view k, const Snapshot& s) {
    json::key(w, k);
    w.StartObject();
    json::key(w, "id"); json::str(w, s.id.stem);
    write_data(w, "data", s.data);
    w.EndObject();
}

void write_kind(Writer& w, std::string_view kind) {
    json::key(w, "kind");
    json::str(w, kind);
}

void write_record(Writer& w, const BuildRecord& r) {
    w.StartObject();
    write_id(w, "id", r.id);
    json::key(w, "data_schema"); w.Uint(r.data_schema);
    json::key(w, "data_compat_floor"); w.Uint(r.data_compat_floor);
    json::key(w, "link_protocol"); w.Uint(r.link_protocol);
    json::key(w, "min_supported_launcher_link"); w.Uint(r.min_supported_launcher_link);
    json::key(w, "launcher_sha256"); json::str(w, r.launcher_sha256.to_hex());
    json::key(w, "app_sha256"); json::str(w, r.app_sha256.to_hex());
    json::key(w, "verdict");
    w.StartObject();
    std::visit(
        [&](const auto& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, Unproven>) write_kind(w, "unproven");
            else if constexpr (std::is_same_v<T, Proven>) write_kind(w, "proven");
            else {
                write_kind(w, "quarantined");
                json::key(w, "reason"); json::str(w, name_of(kReasons, v.reason));
            }
        },
        r.verdict);
    w.EndObject();
    w.EndObject();
}

void write_activation(Writer& w, const Activation& a) {
    json::key(w, "activation");
    w.StartObject();
    std::visit(
        [&](const auto& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, Steady>) {
                write_kind(w, "steady");
            } else if constexpr (std::is_same_v<T, Staged>) {
                write_kind(w, "staged");
                write_id(w, "build", v.build);
                if (v.restore_first) write_snapshot(w, "restore_first", *v.restore_first);
            } else {
                write_kind(w, "probation");
                write_id(w, "candidate", v.candidate);
                write_id(w, "fallback", v.fallback);
                if (v.pre_snapshot) write_snapshot(w, "pre_snapshot", *v.pre_snapshot);
                json::key(w, "failed_runs"); w.Uint(v.failed_runs);
                json::key(w, "restore_on_rollback"); w.Bool(v.restore_on_rollback);
            }
        },
        a);
    w.EndObject();
}

void write_operation(Writer& w, const Operation& o) {
    json::key(w, "operation");
    w.StartObject();
    std::visit(
        [&](const auto& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, Idle>) {
                write_kind(w, "idle");
            } else if constexpr (std::is_same_v<T, Running>) {
                write_kind(w, "running");
                write_id(w, "build", v.build);
                json::key(w, "healthy"); w.Bool(v.healthy);
                json::key(w, "healthy_uptime_s"); w.Uint(v.healthy_uptime_s);
            } else {
                write_kind(w, "restoring");
                write_snapshot(w, "from", v.from);
                json::key(w, "preserve_live_as"); json::str(w, v.preserve_live_as.stem);
            }
        },
        o);
    w.EndObject();
}

// --- Parsing ---

std::optional<BuildId> parse_id(const Value* v) {
    if (!v) return std::nullopt;
    auto version = json::string(*v, "version");
    auto digest = json::string(*v, "digest");
    if (!version || !digest) return std::nullopt;
    auto parsed_version = Version::parse(*version);
    auto parsed_digest = Sha256::from_hex(*digest);
    if (!parsed_version || !parsed_digest) return std::nullopt;
    return BuildId{*parsed_version, *parsed_digest};
}

std::optional<BuildId> id_at(const Value& obj, const char* name) { return parse_id(json::object(obj, name)); }

std::optional<DataInfo> parse_data(const Value* v) {
    if (!v) return std::nullopt;
    auto version = json::u32(*v, "version");
    auto floor = json::u32(*v, "compat_floor");
    if (!version || !floor) return std::nullopt;
    return DataInfo{*version, *floor};
}

std::optional<SnapshotId> parse_stem(std::optional<std::string_view> text) {
    if (!text || !is_safe_stem(*text)) return std::nullopt;
    return SnapshotId{std::string(*text)};
}

std::optional<Snapshot> parse_snapshot(const Value* v) {
    if (!v) return std::nullopt;
    auto id = parse_stem(json::string(*v, "id"));
    auto data = parse_data(json::object(*v, "data"));
    if (!id || !data) return std::nullopt;
    return Snapshot{*id, *data};
}

// An absent member is fine; a present one must parse. nullopt = malformed.
template <class T, class F>
std::optional<std::optional<T>> optional_member(const Value& obj, const char* name, F parse) {
    const Value* v = json::member(obj, name);
    if (!v || v->IsNull()) return std::optional<T>{};
    auto parsed = parse(v->IsObject() ? v : nullptr);
    if (!parsed) return std::nullopt;
    return std::optional<T>{*parsed};
}

std::optional<Verdict> parse_verdict(const Value* v) {
    if (!v) return std::nullopt;
    auto kind = json::string(*v, "kind");
    if (kind == "unproven") return Unproven{};
    if (kind == "proven") return Proven{};
    if (kind == "quarantined") {
        auto reason = value_of(kReasons, json::string(*v, "reason"));
        if (reason) return Quarantined{*reason};
    }
    return std::nullopt;
}

std::optional<BuildRecord> parse_record(const Value& v) {
    BuildRecord r;
    auto id = id_at(v, "id");
    auto schema = json::u32(v, "data_schema");
    auto floor = json::u32(v, "data_compat_floor");
    auto link = json::u32(v, "link_protocol");
    auto min_link = json::u32(v, "min_supported_launcher_link");
    auto launcher = json::string(v, "launcher_sha256");
    auto app = json::string(v, "app_sha256");
    auto verdict = parse_verdict(json::object(v, "verdict"));
    if (!id || !schema || !floor || !link || !min_link || !launcher || !app || !verdict) return std::nullopt;
    auto launcher_sha = Sha256::from_hex(*launcher);
    auto app_sha = Sha256::from_hex(*app);
    if (!launcher_sha || !app_sha) return std::nullopt;
    return BuildRecord{*id, *schema, *floor, *link, *min_link, *launcher_sha, *app_sha, *verdict};
}

std::optional<Activation> parse_activation(const Value* v) {
    if (!v) return std::nullopt;
    auto kind = json::string(*v, "kind");
    if (kind == "steady") return Steady{};
    if (kind == "staged") {
        auto build = id_at(*v, "build");
        auto restore = optional_member<Snapshot>(*v, "restore_first", parse_snapshot);
        if (!build || !restore) return std::nullopt;
        return Staged{*build, *restore};
    }
    if (kind == "probation") {
        auto candidate = id_at(*v, "candidate");
        auto fallback = id_at(*v, "fallback");
        auto pre = optional_member<Snapshot>(*v, "pre_snapshot", parse_snapshot);
        auto failed = json::u32(*v, "failed_runs");
        auto restore = json::boolean(*v, "restore_on_rollback");
        if (!candidate || !fallback || !pre || !failed || *failed > 255 || !restore) return std::nullopt;
        return Probation{*candidate, *fallback, *pre, uint8_t(*failed), *restore};
    }
    return std::nullopt;
}

std::optional<Operation> parse_operation(const Value* v) {
    if (!v) return std::nullopt;
    auto kind = json::string(*v, "kind");
    if (kind == "idle") return Idle{};
    if (kind == "running") {
        auto build = id_at(*v, "build");
        auto healthy = json::boolean(*v, "healthy");
        auto uptime = json::u32(*v, "healthy_uptime_s");
        if (!build || !healthy || !uptime) return std::nullopt;
        return Running{*build, *healthy, *uptime};
    }
    if (kind == "restoring") {
        auto from = parse_snapshot(json::object(*v, "from"));
        auto keep = parse_stem(json::string(*v, "preserve_live_as"));
        if (!from || !keep) return std::nullopt;
        return Restoring{*from, *keep};
    }
    return std::nullopt;
}

std::optional<ChannelIntent> parse_intent(const Value* v) {
    if (!v) return std::nullopt;
    auto kind = json::string(*v, "kind");
    if (kind == "follow") return Follow{};
    if (kind == "pinned_until_at_least") {
        auto version = json::string(*v, "version");
        auto parsed = version ? Version::parse(*version) : std::nullopt;
        if (parsed) return PinnedUntilAtLeast{*parsed};
    }
    return std::nullopt;
}

std::optional<Notice> parse_notice(const Value* v) {
    if (!v) return std::nullopt;
    auto kind = value_of(kNoticeKinds, json::string(*v, "kind"));
    auto from = json::string(*v, "from");
    auto to = json::string(*v, "to");
    auto from_v = from ? Version::parse(*from) : std::nullopt;
    auto to_v = to ? Version::parse(*to) : std::nullopt;
    if (!kind || !from_v || !to_v) return std::nullopt;
    Notice n{*kind, *from_v, *to_v, std::nullopt};
    if (const Value* p = json::member(*v, "preserved"); p && !p->IsNull()) {
        n.preserved = parse_stem(p->IsString() ? std::optional<std::string_view>({p->GetString(), p->GetStringLength()})
                                               : std::nullopt);
        if (!n.preserved) return std::nullopt;
    }
    return n;
}

bool consistent(const SupervisorState& s) {
    for (size_t i = 0; i < s.builds.size(); ++i)
        for (size_t j = i + 1; j < s.builds.size(); ++j)
            if (s.builds[i].id == s.builds[j].id) return false;
    const BuildRecord* good = s.find(s.good);
    if (!good || !std::holds_alternative<Proven>(good->verdict)) return false;
    if (s.previous_good && (*s.previous_good == s.good || !s.find(*s.previous_good))) return false;
    if (auto* st = std::get_if<Staged>(&s.activation)) {
        const BuildRecord* r = s.find(st->build);
        if (!r || st->build == s.good || r->quarantined()) return false;
    }
    if (auto* p = std::get_if<Probation>(&s.activation)) {
        if (p->candidate == s.good || p->fallback != s.good || !s.find(p->candidate)) return false;
    }
    if (auto* r = std::get_if<Running>(&s.operation); r && !s.find(r->build)) return false;
    return true;
}

std::optional<TrustState> parse_trust_state(const Value* v) {
    if (!v) return std::nullopt;
    auto serial = json::u64(*v, "serial");
    auto digest = json::string(*v, "payload_sha256");
    auto issued = json::string(*v, "last_issued_at");
    if (!serial || !digest || !issued) return std::nullopt;
    auto d = Sha256::from_hex(*digest);
    auto t = parse_utc(*issued);
    if (!d || !t) return std::nullopt;
    return TrustState{*serial, *d, *t};
}

std::expected<void, StateParseError> check_schema(const rapidjson::Document& doc) {
    auto schema = json::u32(doc, "schema");
    if (!schema || *schema == 0) return std::unexpected(StateParseError::Malformed);
    if (*schema > kStateSchema) return std::unexpected(StateParseError::FutureSchema);
    return {};
}

} // namespace

std::string write_state(const SupervisorState& s) {
    rapidjson::StringBuffer buf;
    Writer w(buf);
    w.StartObject();
    json::key(w, "schema"); w.Uint(kStateSchema);
    json::key(w, "generation"); w.Uint64(s.generation);
    json::key(w, "channel"); json::str(w, channel_name(s.channel));
    json::key(w, "intent");
    w.StartObject();
    if (auto* pin = std::get_if<PinnedUntilAtLeast>(&s.intent)) {
        write_kind(w, "pinned_until_at_least");
        json::key(w, "version"); json::str(w, pin->floor.to_string());
    } else {
        write_kind(w, "follow");
    }
    w.EndObject();
    write_id(w, "good", s.good);
    if (s.previous_good) write_id(w, "previous_good", *s.previous_good);
    json::key(w, "builds");
    w.StartArray();
    for (const auto& r : s.builds) write_record(w, r);
    w.EndArray();
    if (s.notice) {
        json::key(w, "notice");
        w.StartObject();
        write_kind(w, name_of(kNoticeKinds, s.notice->kind));
        json::key(w, "from"); json::str(w, s.notice->from.to_string());
        json::key(w, "to"); json::str(w, s.notice->to.to_string());
        if (s.notice->preserved) {
            json::key(w, "preserved");
            json::str(w, s.notice->preserved->stem);
        }
        w.EndObject();
    }
    write_activation(w, s.activation);
    write_operation(w, s.operation);
    w.EndObject();
    return {buf.GetString(), buf.GetSize()};
}

std::expected<SupervisorState, StateParseError> parse_state(std::string_view text) {
    rapidjson::Document doc;
    if (!json::parse(doc, text)) return std::unexpected(StateParseError::Malformed);
    if (auto schema = check_schema(doc); !schema) return std::unexpected(schema.error());

    SupervisorState s;
    auto generation = json::u64(doc, "generation");
    auto channel_text = json::string(doc, "channel");
    auto channel = channel_text ? parse_channel(*channel_text) : std::nullopt;
    auto intent = parse_intent(json::object(doc, "intent"));
    auto good = id_at(doc, "good");
    auto previous = optional_member<BuildId>(doc, "previous_good", parse_id);
    auto notice = optional_member<Notice>(doc, "notice", parse_notice);
    auto activation = parse_activation(json::object(doc, "activation"));
    auto operation = parse_operation(json::object(doc, "operation"));
    const Value* builds = json::array(doc, "builds");
    if (!generation || !channel || !intent || !good || !previous || !notice || !activation || !operation || !builds)
        return std::unexpected(StateParseError::Malformed);
    s.generation = *generation;
    s.channel = *channel;
    s.intent = *intent;
    s.good = *good;
    s.previous_good = *previous;
    s.notice = *notice;
    s.activation = *activation;
    s.operation = *operation;
    for (const auto& b : builds->GetArray()) {
        auto r = parse_record(b);
        if (!r) return std::unexpected(StateParseError::Malformed);
        s.builds.push_back(std::move(*r));
    }
    if (!consistent(s)) return std::unexpected(StateParseError::Inconsistent);
    return s;
}

std::string write_trust(const TrustFile& t) {
    rapidjson::StringBuffer buf;
    Writer w(buf);
    w.StartObject();
    json::key(w, "schema"); w.Uint(kStateSchema);
    json::key(w, "channels");
    w.StartObject();
    for (Channel c : {Channel::Stable, Channel::Beta}) {
        const TrustState& s = t.of(c);
        json::key(w, channel_name(c));
        w.StartObject();
        json::key(w, "serial"); w.Uint64(s.serial);
        json::key(w, "payload_sha256"); json::str(w, s.payload_digest.to_hex());
        json::key(w, "last_issued_at"); json::str(w, format_utc(s.last_issued_at));
        w.EndObject();
    }
    w.EndObject();
    w.EndObject();
    return {buf.GetString(), buf.GetSize()};
}

std::expected<TrustFile, StateParseError> parse_trust(std::string_view text) {
    rapidjson::Document doc;
    if (!json::parse(doc, text)) return std::unexpected(StateParseError::Malformed);
    if (auto schema = check_schema(doc); !schema) return std::unexpected(schema.error());
    const Value* channels = json::object(doc, "channels");
    if (!channels) return std::unexpected(StateParseError::Malformed);
    TrustFile t;
    for (Channel c : {Channel::Stable, Channel::Beta}) {
        const Value* v = json::member(*channels, std::string(channel_name(c)).c_str());
        if (!v) continue;
        auto s = parse_trust_state(v->IsObject() ? v : nullptr);
        if (!s) return std::unexpected(StateParseError::Malformed);
        t.of(c) = *s;
    }
    return t;
}

} // namespace parties::update
