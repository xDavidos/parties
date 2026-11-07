#include <parties/update/front_door.h>

#include <parties/update/boot.h>

#include "json_util.h"

#include <type_traits>

namespace parties::update::front_door {

namespace {

using json::Value;
using json::Writer;

void string_field(Writer& w, std::string_view k, std::string_view v) {
    json::key(w, k);
    json::str(w, v);
}

std::optional<std::string> build_at(const Value& doc) {
    auto build = json::string(doc, "build");
    if (!build || !boot::is_build_dir(*build)) return std::nullopt;
    return std::string(*build);
}

std::optional<int32_t> i32(const Value& doc, const char* name) {
    const Value* v = json::member(doc, name);
    if (!v || !v->IsInt()) return std::nullopt;
    return v->GetInt();
}

std::optional<std::vector<std::string>> strings(const Value& doc, const char* name) {
    const Value* list = json::array(doc, name);
    if (!list) return std::nullopt;
    std::vector<std::string> out;
    for (const Value& item : list->GetArray()) {
        if (!item.IsString()) return std::nullopt;
        out.emplace_back(item.GetString(), item.GetStringLength());
    }
    return out;
}

} // namespace

std::string write(const HostMessage& message) {
    return std::visit(
        [](const auto& m) -> std::string {
            using T = std::decay_t<decltype(m)>;
            if constexpr (std::is_same_v<T, Front>) {
                return json::message("front", [&](Writer& w) {
                    json::key(w, "pid");
                    w.Int(m.pid);
                    json::key(w, "host_abi");
                    w.Uint(m.host_abi);
                    string_field(w, "host", m.host);
                    json::key(w, "recover");
                    w.Bool(m.recover);
                    json::key(w, "seed_pending");
                    w.Bool(m.seed_pending);
                    json::key(w, "argv");
                    w.StartArray();
                    for (const std::string& arg : m.argv) json::str(w, arg);
                    w.EndArray();
                });
            } else if constexpr (std::is_same_v<T, Attach>) {
                return json::message("attach", [&](Writer& w) {
                    json::key(w, "ticket");
                    w.Uint64(m.ticket);
                });
            } else if constexpr (std::is_same_v<T, Loaded>) {
                return json::message("loaded", [&](Writer& w) { string_field(w, "build", m.build); });
            } else if constexpr (std::is_same_v<T, Corrupt>) {
                return json::message("corrupt", [&](Writer& w) { string_field(w, "build", m.build); });
            } else if constexpr (std::is_same_v<T, Exiting>) {
                return json::message("exiting", [&](Writer& w) {
                    json::key(w, "code");
                    w.Int(m.code);
                });
            } else {
                return json::message("recovery_choice", [&](Writer& w) { string_field(w, "id", m.id); });
            }
        },
        message);
}

std::string write(const LauncherMessage& message) {
    return std::visit(
        [](const auto& m) -> std::string {
            using T = std::decay_t<decltype(m)>;
            if constexpr (std::is_same_v<T, Assign>) {
                return json::message("assign", [&](Writer& w) {
                    string_field(w, "build", m.build);
                    string_field(w, "app_sha256", m.app_sha256.to_hex());
                    string_field(w, "data_dir", m.data_dir);
                    string_field(w, "backups_dir", m.backups_dir);
                    json::key(w, "ticket");
                    w.Uint64(m.ticket);
                });
            } else if constexpr (std::is_same_v<T, ActivateOther>) {
                return json::message("activate_other", [](Writer&) {});
            } else if constexpr (std::is_same_v<T, Activate>) {
                return json::message("activate", [](Writer&) {});
            } else if constexpr (std::is_same_v<T, QuitForInstall>) {
                return json::message("quit_for_install", [](Writer&) {});
            } else {
                return json::message("recovery", [&](Writer& w) {
                    string_field(w, "reason", m.reason);
                    string_field(w, "data_dir", m.data_dir);
                    json::key(w, "options");
                    w.StartArray();
                    for (const RecoveryOption& o : m.options) {
                        w.StartObject();
                        string_field(w, "id", o.id);
                        string_field(w, "label", o.label);
                        string_field(w, "detail", o.detail);
                        w.EndObject();
                    }
                    w.EndArray();
                });
            }
        },
        message);
}

std::optional<HostMessage> parse_host_message(std::string_view line) {
    rapidjson::Document doc;
    std::string_view type;
    if (!json::parse_message(doc, line, type)) return std::nullopt;
    if (type == "front") {
        auto pid = i32(doc, "pid");
        auto abi = json::u32(doc, "host_abi");
        auto host = json::string(doc, "host");
        auto recover = json::boolean(doc, "recover");
        auto seed_pending = json::boolean(doc, "seed_pending");
        auto argv = strings(doc, "argv");
        if (!pid || !abi || !host || !recover || !seed_pending || !argv) return std::nullopt;
        return Front{*pid, *abi, std::string(*host), *recover, *seed_pending, std::move(*argv)};
    }
    if (type == "attach") {
        auto ticket = json::u64(doc, "ticket");
        if (!ticket) return std::nullopt;
        return Attach{*ticket};
    }
    if (type == "loaded" || type == "corrupt") {
        auto build = build_at(doc);
        if (!build) return std::nullopt;
        if (type == "loaded") return Loaded{std::move(*build)};
        return Corrupt{std::move(*build)};
    }
    if (type == "exiting") {
        auto code = i32(doc, "code");
        if (!code) return std::nullopt;
        return Exiting{*code};
    }
    if (type == "recovery_choice") {
        auto id = json::string(doc, "id");
        if (!id || id->empty()) return std::nullopt;
        return RecoveryChoice{std::string(*id)};
    }
    return std::nullopt;
}

std::optional<LauncherMessage> parse_launcher_message(std::string_view line) {
    rapidjson::Document doc;
    std::string_view type;
    if (!json::parse_message(doc, line, type)) return std::nullopt;
    if (type == "assign") {
        auto build = build_at(doc);
        auto sha_hex = json::string(doc, "app_sha256");
        auto sha = sha_hex ? Sha256::from_hex(*sha_hex) : std::nullopt;
        auto data_dir = json::string(doc, "data_dir");
        auto backups_dir = json::string(doc, "backups_dir");
        auto ticket = json::u64(doc, "ticket");
        if (!build || !sha || !data_dir || !backups_dir || !ticket) return std::nullopt;
        return Assign{std::move(*build), *sha, std::string(*data_dir), std::string(*backups_dir), *ticket};
    }
    if (type == "activate_other") return ActivateOther{};
    if (type == "activate") return Activate{};
    if (type == "quit_for_install") return QuitForInstall{};
    if (type == "recovery") {
        auto reason = json::string(doc, "reason");
        auto data_dir = json::string(doc, "data_dir");
        const Value* list = json::array(doc, "options");
        if (!reason || !data_dir || !list) return std::nullopt;
        Recovery r{std::string(*reason), std::string(*data_dir), {}};
        for (const Value& item : list->GetArray()) {
            if (!item.IsObject()) return std::nullopt;
            auto id = json::string(item, "id");
            auto label = json::string(item, "label");
            auto detail = json::string(item, "detail");
            if (!id || id->empty() || !label || !detail) return std::nullopt;
            r.options.push_back({std::string(*id), std::string(*label), std::string(*detail)});
        }
        return r;
    }
    return std::nullopt;
}

} // namespace parties::update::front_door
