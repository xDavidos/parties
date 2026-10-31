#pragma once

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace parties::update::json {

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

} // namespace parties::update::json
