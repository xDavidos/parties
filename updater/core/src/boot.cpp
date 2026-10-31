#include <parties/update/boot.h>

#include "json_util.h"

#include <algorithm>

namespace parties::update::boot {

namespace {

uint8_t bump(uint8_t n) { return n == 255 ? n : uint8_t(n + 1); }

LauncherHealth& health_of(BootRecord& rec, const Sha256& launcher) {
    auto it = std::ranges::find(rec.launchers, launcher, &LauncherHealth::launcher);
    if (it != rec.launchers.end()) return *it;
    return rec.launchers.emplace_back(LauncherHealth{launcher});
}

void count_failure(BootRecord& rec, const Sha256& launcher) {
    LauncherHealth& h = health_of(rec, launcher);
    if (!h.confirmed) h.failed_boots = bump(h.failed_boots);
}

void fold_attempt(BootRecord& rec, const std::optional<SessionReport>& session) {
    if (!rec.attempt) return;
    Attempt a = *rec.attempt;
    rec.attempt.reset();
    bool clean = session && session->session == a.session && session->launcher == a.launcher &&
                 session->ended_clean;
    if (!clean) return count_failure(rec, a.launcher);
    LauncherHealth& h = health_of(rec, a.launcher);
    h.clean_sessions = bump(h.clean_sessions);
    if (h.clean_sessions >= kCleanSessionsToConfirm) h.confirmed = true;
}

bool eligible(const BootRecord& rec, const LauncherRef& ref, std::span<const Sha256> unusable) {
    if (std::ranges::find(unusable, ref.sha256) != unusable.end()) return false;
    auto it = std::ranges::find(rec.launchers, ref.sha256, &LauncherHealth::launcher);
    return it == rec.launchers.end() || it->confirmed || it->failed_boots < kMaxFailedBoots;
}

// Only "builds\<name>" with a plain name, so a damaged contract can never point
// the stub outside the install root.
bool is_build_dir(std::string_view dir) {
    constexpr std::string_view prefix = "builds\\";
    if (!dir.starts_with(prefix)) return false;
    std::string_view name = dir.substr(prefix.size());
    if (name.empty() || name.front() == '.' || name.find("..") != std::string_view::npos) return false;
    return std::ranges::all_of(name, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
               c == '_' || c == '-';
    });
}

std::optional<Sha256> digest(const json::Value& v, const char* name) {
    auto hex = json::string(v, name);
    return hex ? Sha256::from_hex(*hex) : std::nullopt;
}

template <class Ref>
std::optional<Ref> parse_ref(const json::Value* v) {
    if (!v) return std::nullopt;
    auto dir = json::string(*v, "dir");
    auto sha = digest(*v, "sha256");
    if (!dir || !sha || !is_build_dir(*dir)) return std::nullopt;
    return Ref{std::string(*dir), *sha};
}

template <class Ref>
void write_ref(json::Writer& w, std::string_view k, const Ref& ref) {
    json::key(w, k);
    w.StartObject();
    json::key(w, "dir"); json::str(w, ref.dir);
    json::key(w, "sha256"); json::str(w, ref.sha256.to_hex());
    w.EndObject();
}

std::optional<uint8_t> small_count(const json::Value& v, const char* name) {
    auto n = json::u32(v, name);
    if (!n) return std::nullopt;
    return uint8_t(std::min<uint32_t>(*n, 255));
}

std::string finish(rapidjson::StringBuffer& buf) { return {buf.GetString(), buf.GetSize()}; }

} // namespace

Decision choose(const std::optional<BootContract>& contract, const BootRecord& record,
                const std::optional<SessionReport>& session, std::span<const Sha256> unusable) {
    BootRecord rec = record;
    fold_attempt(rec, session);
    if (!contract) return {ShowStubRecovery{}, rec};

    std::erase_if(rec.launchers, [&](const LauncherHealth& h) {
        return h.launcher != contract->good.sha256 &&
               !(contract->previous_good && h.launcher == contract->previous_good->sha256);
    });

    for (const LauncherRef* ref : {&contract->good, contract->previous_good ? &*contract->previous_good : nullptr}) {
        if (!ref || !eligible(rec, *ref, unusable)) continue;
        rec.last_session += 1;
        rec.attempt = Attempt{rec.last_session, ref->sha256};
        return {RunLauncher{ref->dir, ref->sha256, rec.last_session}, rec};
    }
    return {ShowStubRecovery{}, rec};
}

BootRecord record_boot_failure(const BootRecord& record) {
    BootRecord rec = record;
    if (!rec.attempt) return rec;
    Sha256 launcher = rec.attempt->launcher;
    rec.attempt.reset();
    count_failure(rec, launcher);
    return rec;
}

std::optional<BootContract> parse_boot_contract(std::string_view text) {
    rapidjson::Document doc;
    if (!json::parse(doc, text)) return std::nullopt;
    auto good = parse_ref<LauncherRef>(json::object(doc, "good"));
    auto app = parse_ref<AppRef>(json::object(doc, "last_good_app"));
    auto data_dir = json::string(doc, "data_dir");
    if (!good || !app || !data_dir || data_dir->empty()) return std::nullopt;
    BootContract c{*good, std::nullopt, *app, std::string(*data_dir)};
    if (const json::Value* prev = json::member(doc, "previous_good"); prev && !prev->IsNull()) {
        c.previous_good = parse_ref<LauncherRef>(prev->IsObject() ? prev : nullptr);
        if (!c.previous_good) return std::nullopt;
    }
    return c;
}

std::string write_boot_contract(const BootContract& c) {
    rapidjson::StringBuffer buf;
    json::Writer w(buf);
    w.StartObject();
    json::key(w, "format"); w.Uint(1);
    write_ref(w, "good", c.good);
    if (c.previous_good) write_ref(w, "previous_good", *c.previous_good);
    write_ref(w, "last_good_app", c.last_good_app);
    json::key(w, "data_dir"); json::str(w, c.data_dir);
    w.EndObject();
    return finish(buf);
}

std::optional<BootRecord> parse_boot_record(std::string_view text) {
    rapidjson::Document doc;
    if (!json::parse(doc, text)) return std::nullopt;
    auto last = json::u64(doc, "last_session");
    const json::Value* launchers = json::array(doc, "launchers");
    if (!last || !launchers) return std::nullopt;
    BootRecord rec{*last, std::nullopt, {}};
    if (const json::Value* a = json::member(doc, "attempt"); a && !a->IsNull()) {
        auto session = json::u64(*a, "session");
        auto launcher = digest(*a, "launcher_sha256");
        if (!session || !launcher) return std::nullopt;
        rec.attempt = Attempt{*session, *launcher};
    }
    for (const auto& h : launchers->GetArray()) {
        auto sha = digest(h, "sha256");
        auto failed = small_count(h, "failed_boots");
        auto clean = small_count(h, "clean_sessions");
        auto confirmed = json::boolean(h, "confirmed");
        if (!sha || !failed || !clean || !confirmed) return std::nullopt;
        rec.launchers.push_back({*sha, *failed, *clean, *confirmed});
    }
    return rec;
}

std::string write_boot_record(const BootRecord& rec) {
    rapidjson::StringBuffer buf;
    json::Writer w(buf);
    w.StartObject();
    json::key(w, "format"); w.Uint(1);
    json::key(w, "last_session"); w.Uint64(rec.last_session);
    if (rec.attempt) {
        json::key(w, "attempt");
        w.StartObject();
        json::key(w, "session"); w.Uint64(rec.attempt->session);
        json::key(w, "launcher_sha256"); json::str(w, rec.attempt->launcher.to_hex());
        w.EndObject();
    }
    json::key(w, "launchers");
    w.StartArray();
    for (const auto& h : rec.launchers) {
        w.StartObject();
        json::key(w, "sha256"); json::str(w, h.launcher.to_hex());
        json::key(w, "failed_boots"); w.Uint(h.failed_boots);
        json::key(w, "clean_sessions"); w.Uint(h.clean_sessions);
        json::key(w, "confirmed"); w.Bool(h.confirmed);
        w.EndObject();
    }
    w.EndArray();
    w.EndObject();
    return finish(buf);
}

std::optional<SessionReport> parse_session_report(std::string_view text) {
    rapidjson::Document doc;
    if (!json::parse(doc, text)) return std::nullopt;
    auto session = json::u64(doc, "session");
    auto launcher = digest(doc, "launcher_sha256");
    auto clean = json::boolean(doc, "ended_clean");
    if (!session || !launcher || !clean) return std::nullopt;
    return SessionReport{*session, *launcher, *clean};
}

std::string write_session_report(const SessionReport& r) {
    rapidjson::StringBuffer buf;
    json::Writer w(buf);
    w.StartObject();
    json::key(w, "format"); w.Uint(1);
    json::key(w, "session"); w.Uint64(r.session);
    json::key(w, "launcher_sha256"); json::str(w, r.launcher.to_hex());
    json::key(w, "ended_clean"); w.Bool(r.ended_clean);
    w.EndObject();
    return finish(buf);
}

} // namespace parties::update::boot
