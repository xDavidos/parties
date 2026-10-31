#include "build_info.h"

#include <rapidjson/document.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>

namespace parties::update::launcher {

namespace {

std::optional<uint32_t> u32(const rapidjson::Value& obj, const char* name) {
    auto it = obj.FindMember(name);
    if (it == obj.MemberEnd() || !it->value.IsUint()) return std::nullopt;
    return it->value.GetUint();
}

std::optional<std::string_view> str(const rapidjson::Value& obj, const char* name) {
    auto it = obj.FindMember(name);
    if (it == obj.MemberEnd() || !it->value.IsString()) return std::nullopt;
    return std::string_view(it->value.GetString(), it->value.GetStringLength());
}

bool bare_name(std::string_view name) {
    return !name.empty() && name.find_first_of("\\/:") == std::string_view::npos && name != "." && name != "..";
}

} // namespace

std::string write_build_info(const BuildEntry& e) {
    rapidjson::StringBuffer buffer;
    rapidjson::PrettyWriter<rapidjson::StringBuffer> w(buffer);
    w.StartObject();
    w.Key("format"); w.Uint(1);
    w.Key("version"); w.String(e.version.to_string().c_str());
    w.Key("data_schema"); w.Uint(e.data_schema);
    w.Key("data_compat_floor"); w.Uint(e.data_compat_floor);
    w.Key("link_protocol"); w.Uint(e.link_protocol);
    w.Key("min_supported_launcher_link"); w.Uint(e.min_supported_launcher_link);
    w.Key("files");
    w.StartArray();
    for (const FileEntry& f : e.files) {
        w.StartObject();
        w.Key("name"); w.String(f.name.c_str());
        w.Key("size"); w.Uint64(f.size);
        w.Key("sha256"); w.String(f.sha256.to_hex().c_str());
        w.EndObject();
    }
    w.EndArray();
    w.EndObject();
    return {buffer.GetString(), buffer.GetSize()};
}

std::optional<BuildEntry> parse_build_info(std::string_view text) {
    rapidjson::Document doc;
    doc.Parse(text.data(), text.size());
    if (doc.HasParseError() || !doc.IsObject()) return std::nullopt;
    BuildEntry e;
    auto version = str(doc, "version");
    auto schema = u32(doc, "data_schema");
    auto floor = u32(doc, "data_compat_floor");
    auto link = u32(doc, "link_protocol");
    auto min_link = u32(doc, "min_supported_launcher_link");
    if (!version || !schema || !floor || !link || !min_link) return std::nullopt;
    auto parsed = Version::parse(*version);
    if (!parsed) return std::nullopt;
    e.version = *parsed;
    e.data_schema = *schema;
    e.data_compat_floor = *floor;
    e.link_protocol = *link;
    e.min_supported_launcher_link = *min_link;
    auto files = doc.FindMember("files");
    if (files == doc.MemberEnd() || !files->value.IsArray()) return std::nullopt;
    for (const auto& f : files->value.GetArray()) {
        if (!f.IsObject()) return std::nullopt;
        auto name = str(f, "name");
        auto sha = str(f, "sha256");
        auto size = f.FindMember("size");
        if (!name || !bare_name(*name) || !sha || size == f.MemberEnd() || !size->value.IsUint64())
            return std::nullopt;
        auto digest = Sha256::from_hex(*sha);
        if (!digest) return std::nullopt;
        e.files.push_back({std::string(*name), size->value.GetUint64(), *digest, {}});
    }
    if (!e.file(kLauncherFile) || !e.file(kAppFile)) return std::nullopt;
    return e;
}

} // namespace parties::update::launcher
