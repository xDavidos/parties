#include "release_ops.h"

#include <rapidjson/document.h>

#include <algorithm>
#include <functional>

namespace parties::update::release {

namespace {

std::unexpected<std::string> fail(std::string message) {
    return std::unexpected(std::move(message));
}

std::optional<uint32_t> u32(const rapidjson::Document& doc, const char* name) {
    auto it = doc.FindMember(name);
    if (it == doc.MemberEnd() || !it->value.IsUint()) return std::nullopt;
    return it->value.GetUint();
}

} // namespace

Result<BuildInfo> parse_build_info(std::string_view json) {
    rapidjson::Document doc;
    doc.Parse(json.data(), json.size());
    if (doc.HasParseError() || !doc.IsObject()) return fail("build info is not a JSON object");
    if (u32(doc, "schema") != kBuildInfoSchema) return fail("build info schema is not " + std::to_string(kBuildInfoSchema));
    auto version = doc.FindMember("version");
    auto schema = u32(doc, "data_schema");
    auto floor = u32(doc, "data_compat_floor");
    auto link = u32(doc, "link_protocol");
    auto min_link = u32(doc, "min_supported_launcher_link");
    if (version == doc.MemberEnd() || !version->value.IsString() || !schema || !floor || !link || !min_link)
        return fail("build info lacks a field");
    BuildInfo info{version->value.GetString(), *schema, *floor, *link, *min_link};
    if (!Version::parse(info.version)) return fail("build info version " + info.version + " is not A.B.C.D");
    return info;
}

Result<BuildEntry> make_entry(const BuildInfo& info, std::span<const StagedFile> files, std::string_view url_base) {
    if (!url_base.starts_with("https://") || !url_base.ends_with('/'))
        return fail("--url-base must start with https:// and end with /");
    auto version = Version::parse(info.version);
    if (!version) return fail("build info version " + info.version + " is not A.B.C.D");
    BuildEntry entry;
    entry.version = *version;
    entry.data_schema = info.data_schema;
    entry.data_compat_floor = info.data_compat_floor;
    entry.link_protocol = info.link_protocol;
    entry.min_supported_launcher_link = info.min_supported_launcher_link;
    for (const StagedFile& f : files)
        entry.files.push_back({f.name, f.size, f.sha256, {std::string(url_base) + f.asset_name}});
    // The same parser the launcher runs, so an entry the tool emits is one
    // verify_manifest accepts.
    auto checked = parse_build_entry(write_build_entry(entry));
    if (!checked) return fail(checked.error().detail);
    return *checked;
}

std::vector<BuildEntry> retained(std::vector<BuildEntry> builds, size_t keep) {
    std::ranges::sort(builds, std::greater{}, &BuildEntry::version);
    std::vector<bool> kept(builds.size(), false);
    for (size_t i = 0; i < builds.size() && i < keep; ++i) kept[i] = true;
    uint32_t newest_protocol = 0;
    for (const BuildEntry& b : builds) newest_protocol = std::max(newest_protocol, b.link_protocol);
    for (uint32_t protocol = 1; protocol <= newest_protocol; ++protocol) {
        auto stageable = std::ranges::find_if(builds, [&](const BuildEntry& b) {
            return b.min_supported_launcher_link <= protocol;
        });
        if (stageable != builds.end()) kept[size_t(stageable - builds.begin())] = true;
    }
    std::vector<BuildEntry> out;
    for (size_t i = 0; i < builds.size(); ++i)
        if (kept[i]) out.push_back(std::move(builds[i]));
    return out;
}

Result<ManifestPayload> reissue(const ManifestPayload& base, const Issue& issue) {
    if (issue.serial <= base.serial)
        return fail("serial " + std::to_string(issue.serial) + " does not exceed the published serial " +
                    std::to_string(base.serial));
    ManifestPayload next = base;
    next.serial = issue.serial;
    next.issued_at = issue.issued_at;
    next.expires_at = issue.issued_at + issue.lifetime;
    return next;
}

Result<ManifestPayload> add_build(const ManifestPayload* base, Channel channel, const BuildEntry& entry,
                                  const Issue& issue, size_t keep) {
    ManifestPayload empty;
    empty.channel = channel;
    const ManifestPayload& from = base ? *base : empty;
    if (from.channel != channel) return fail("the base manifest is not for channel " + std::string(channel_name(channel)));
    if (std::ranges::find(from.revoked, entry.version) != from.revoked.end())
        return fail(entry.version.to_string() + " is revoked in " + std::string(channel_name(channel)));
    auto next = reissue(from, issue);
    if (!next) return next;
    std::erase_if(next->builds, [&](const BuildEntry& b) { return b.version == entry.version; });
    next->builds.push_back(entry);
    next->builds = retained(std::move(next->builds), keep);
    if (std::ranges::find(next->builds, entry) == next->builds.end())
        return fail(entry.version.to_string() + " is older than the newest " + std::to_string(keep) +
                    " builds and would be dropped at once");
    return next;
}

Result<ManifestPayload> promote(const ManifestPayload& beta, const ManifestPayload* stable, const Version& version,
                                const Issue& issue, size_t keep) {
    if (std::ranges::find(beta.revoked, version) != beta.revoked.end())
        return fail(version.to_string() + " is revoked in beta");
    auto entry = std::ranges::find(beta.builds, version, &BuildEntry::version);
    if (entry == beta.builds.end()) return fail(version.to_string() + " is not in the beta manifest");
    return add_build(stable, Channel::Stable, *entry, issue, keep);
}

Result<ManifestPayload> revoke(const ManifestPayload* base, Channel channel, const Version& version,
                               const Issue& issue) {
    ManifestPayload empty;
    empty.channel = channel;
    const ManifestPayload& from = base ? *base : empty;
    if (from.channel != channel) return fail("the base manifest is not for channel " + std::string(channel_name(channel)));
    auto next = reissue(from, issue);
    if (!next) return next;
    std::erase_if(next->builds, [&](const BuildEntry& b) { return b.version == version; });
    if (std::ranges::find(next->revoked, version) == next->revoked.end()) next->revoked.push_back(version);
    std::ranges::sort(next->revoked);
    return next;
}

} // namespace parties::update::release
