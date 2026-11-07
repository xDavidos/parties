#pragma once

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace parties::update::json {

template <class E, size_t N>
using Names = std::array<std::pair<E, std::string_view>, N>;

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

using Value = rapidjson::Value;
using Writer = rapidjson::Writer<rapidjson::StringBuffer>;

inline const Value* member(const Value& obj, const char* name) {
    if (!obj.IsObject()) return nullptr;
    auto it = obj.FindMember(name);
    return it == obj.MemberEnd() ? nullptr : &it->value;
}

inline const Value* object(const Value& obj, const char* name) {
    const Value* v = member(obj, name);
    return v && v->IsObject() ? v : nullptr;
}

inline const Value* array(const Value& obj, const char* name) {
    const Value* v = member(obj, name);
    return v && v->IsArray() ? v : nullptr;
}

inline std::optional<std::string_view> string(const Value& obj, const char* name) {
    const Value* v = member(obj, name);
    if (!v || !v->IsString()) return std::nullopt;
    return std::string_view(v->GetString(), v->GetStringLength());
}

inline std::optional<uint64_t> u64(const Value& obj, const char* name) {
    const Value* v = member(obj, name);
    if (!v || !v->IsUint64()) return std::nullopt;
    return v->GetUint64();
}

inline std::optional<uint32_t> u32(const Value& obj, const char* name) {
    const Value* v = member(obj, name);
    if (!v || !v->IsUint()) return std::nullopt;
    return v->GetUint();
}

inline std::optional<bool> boolean(const Value& obj, const char* name) {
    const Value* v = member(obj, name);
    if (!v || !v->IsBool()) return std::nullopt;
    return v->GetBool();
}

inline bool parse(rapidjson::Document& doc, std::string_view text) {
    doc.Parse(text.data(), text.size());
    return !doc.HasParseError() && doc.IsObject();
}

inline void key(Writer& w, std::string_view k) {
    w.Key(k.data(), static_cast<rapidjson::SizeType>(k.size()));
}

inline void str(Writer& w, std::string_view s) {
    w.String(s.data(), static_cast<rapidjson::SizeType>(s.size()));
}

// One protocol line: {"type":<type>, fields...} and a trailing '\n'.
template <class F>
std::string message(std::string_view type, F&& fields) {
    rapidjson::StringBuffer buffer;
    Writer w(buffer);
    w.StartObject();
    key(w, "type");
    str(w, type);
    fields(w);
    w.EndObject();
    std::string line(buffer.GetString(), buffer.GetSize());
    line.push_back('\n');
    return line;
}

inline bool parse_message(rapidjson::Document& doc, std::string_view line, std::string_view& type) {
    if (!parse(doc, line)) return false;
    auto t = string(doc, "type");
    if (!t) return false;
    type = *t;
    return true;
}

} // namespace parties::update::json
