#include <parties/update/manifest.h>

#include "encoding.h"
#include "json_util.h"

#include <parties/crypto.h>

#include <algorithm>
#include <cctype>

namespace parties::update {

namespace {

constexpr std::string_view kSigningDomain{"parties-update-manifest-v1\0", 27};
constexpr uint32_t kPayloadSchema = 1;

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = char(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool is_reserved_device_name(std::string_view name) {
    std::string stem = lower(name.substr(0, name.find('.')));
    for (std::string_view reserved : {"con", "prn", "aux", "nul"})
        if (stem == reserved) return true;
    if (stem.size() == 4 && (stem.starts_with("com") || stem.starts_with("lpt")) &&
        stem[3] >= '1' && stem[3] <= '9')
        return true;
    return false;
}

// Allowlist instead of a denylist of separators: only plain names reach disk.
bool is_valid_file_name(std::string_view name) {
    if (name.empty() || name.size() > 64 || name.front() == '.') return false;
    if (name.find("..") != std::string_view::npos) return false;
    for (char c : name) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '.' || c == '_' || c == '-';
        if (!ok) return false;
    }
    return !is_reserved_device_name(name);
}

bool is_valid_url(std::string_view url) {
    constexpr std::string_view scheme = "https://";
    if (!url.starts_with(scheme) || url.size() > 2048) return false;
    for (char c : url)
        if (static_cast<unsigned char>(c) <= 0x20 || c == 0x7f || c == '\\') return false;
    std::string_view rest = url.substr(scheme.size());
    std::string_view authority = rest.substr(0, rest.find('/'));
    return !authority.empty() && authority.find('@') == std::string_view::npos;
}

std::unexpected<ManifestError> error(ManifestError::Kind kind, std::string detail) {
    return std::unexpected(ManifestError{kind, std::move(detail)});
}

std::expected<FileEntry, ManifestError> parse_file(const json::Value& v) {
    using K = ManifestError::Kind;
    auto name = json::string(v, "name");
    auto size = json::u64(v, "size");
    auto sha = json::string(v, "sha256");
    const json::Value* urls = json::array(v, "urls");
    if (!name || !size || !sha || !urls) return error(K::Malformed, "file entry fields");
    FileEntry f;
    if (!is_valid_file_name(*name)) return error(K::InvalidField, "file name " + std::string(*name));
    f.name = *name;
    if (*size == 0) return error(K::InvalidField, "file size 0");
    f.size = *size;
    auto digest = Sha256::from_hex(*sha);
    if (!digest) return error(K::InvalidField, "file sha256");
    f.sha256 = *digest;
    if (urls->Empty()) return error(K::InvalidField, "file has no urls");
    for (const auto& u : urls->GetArray()) {
        if (!u.IsString()) return error(K::Malformed, "url type");
        std::string_view text(u.GetString(), u.GetStringLength());
        if (!is_valid_url(text)) return error(K::InvalidField, "url " + std::string(text));
        f.urls.emplace_back(text);
    }
    return f;
}

std::expected<BuildEntry, ManifestError> parse_build(const json::Value& v, const ProductSpec& product) {
    using K = ManifestError::Kind;
    auto version = json::string(v, "version");
    auto schema = json::u32(v, "data_schema");
    auto floor = json::u32(v, "data_compat_floor");
    auto link = json::u32(v, "link_protocol");
    auto min_link = json::u32(v, "min_supported_launcher_link");
    const json::Value* files = json::array(v, "files");
    if (!version || !schema || !floor || !link || !min_link || !files)
        return error(K::Malformed, "build entry fields");
    BuildEntry b;
    auto parsed = Version::parse(*version);
    if (!parsed) return error(K::InvalidField, "build version " + std::string(*version));
    b.version = *parsed;
    if (*schema == 0 || *floor == 0 || *floor > *schema)
        return error(K::InvalidField, "data schema/floor");
    if (*link == 0 || *min_link == 0 || *min_link > *link)
        return error(K::InvalidField, "link protocol range");
    b.data_schema = *schema;
    b.data_compat_floor = *floor;
    b.link_protocol = *link;
    b.min_supported_launcher_link = *min_link;
    std::vector<std::string> seen;
    for (const auto& fv : files->GetArray()) {
        auto f = parse_file(fv);
        if (!f) return std::unexpected(f.error());
        std::string key = lower(f->name);
        if (std::ranges::find(seen, key) != seen.end())
            return error(K::InvalidField, "duplicate file " + f->name);
        seen.push_back(std::move(key));
        b.files.push_back(std::move(*f));
    }
    if (!b.file(product.launcher_file) || !b.file(product.app_file))
        return error(K::InvalidField, "build lacks launcher or app");
    return b;
}

void write_build(json::Writer& w, const BuildEntry& b) {
    w.StartObject();
    json::key(w, "version"); json::str(w, b.version.to_string());
    json::key(w, "data_schema"); w.Uint(b.data_schema);
    json::key(w, "data_compat_floor"); w.Uint(b.data_compat_floor);
    json::key(w, "link_protocol"); w.Uint(b.link_protocol);
    json::key(w, "min_supported_launcher_link"); w.Uint(b.min_supported_launcher_link);
    json::key(w, "files");
    w.StartArray();
    for (const auto& f : b.files) {
        w.StartObject();
        json::key(w, "name"); json::str(w, f.name);
        json::key(w, "size"); w.Uint64(f.size);
        json::key(w, "sha256"); json::str(w, f.sha256.to_hex());
        json::key(w, "urls");
        w.StartArray();
        for (const auto& u : f.urls) json::str(w, u);
        w.EndArray();
        w.EndObject();
    }
    w.EndArray();
    w.EndObject();
}

std::expected<ManifestPayload, ManifestError> parse_payload(std::string_view text, Channel expected,
                                                            const ProductSpec& spec) {
    using K = ManifestError::Kind;
    rapidjson::Document doc;
    if (!json::parse(doc, text)) return error(K::Malformed, "payload is not a JSON object");
    if (json::u32(doc, "schema") != kPayloadSchema) return error(K::Malformed, "payload schema");
    ManifestPayload p;
    auto product = json::string(doc, "product");
    if (!product) return error(K::Malformed, "product");
    if (*product != spec.id) return error(K::WrongProduct, std::string(*product));
    p.product = *product;
    auto channel_text = json::string(doc, "channel");
    auto channel = channel_text ? parse_channel(*channel_text) : std::nullopt;
    if (!channel) return error(K::InvalidField, "channel");
    if (*channel != expected) return error(K::WrongChannel, std::string(*channel_text));
    p.channel = *channel;
    auto serial = json::u64(doc, "serial");
    if (!serial || *serial == 0) return error(K::InvalidField, "serial");
    p.serial = *serial;
    auto issued = json::string(doc, "issued_at");
    auto expires = json::string(doc, "expires_at");
    auto issued_at = issued ? parse_utc(*issued) : std::nullopt;
    auto expires_at = expires ? parse_utc(*expires) : std::nullopt;
    if (!issued_at || !expires_at || *expires_at < *issued_at)
        return error(K::InvalidField, "issued_at/expires_at");
    p.issued_at = *issued_at;
    p.expires_at = *expires_at;
    const json::Value* builds = json::array(doc, "builds");
    if (!builds) return error(K::Malformed, "builds");
    for (const auto& bv : builds->GetArray()) {
        auto b = parse_build(bv, spec);
        if (!b) return std::unexpected(b.error());
        p.builds.push_back(std::move(*b));
    }
    std::ranges::sort(p.builds, std::greater{}, &BuildEntry::version);
    auto dup = std::ranges::adjacent_find(p.builds, {}, &BuildEntry::version);
    if (dup != p.builds.end()) return error(K::InvalidField, "duplicate version " + dup->version.to_string());
    if (const json::Value* revoked = json::member(doc, "revoked")) {
        if (!revoked->IsArray()) return error(K::Malformed, "revoked");
        for (const auto& rv : revoked->GetArray()) {
            auto v = rv.IsString() ? Version::parse({rv.GetString(), rv.GetStringLength()}) : std::nullopt;
            if (!v) return error(K::InvalidField, "revoked version");
            p.revoked.push_back(*v);
        }
    }
    return p;
}

} // namespace

const FileEntry* BuildEntry::file(std::string_view name) const {
    auto it = std::ranges::find(files, name, &FileEntry::name);
    return it == files.end() ? nullptr : &*it;
}

Sha256 sha256_of(std::span<const uint8_t> bytes) {
    std::string hex = parties::sha256_hex(bytes.data(), bytes.size());
    std::erase(hex, ':');
    return *Sha256::from_hex(hex);
}

Sha256 build_digest(std::span<const FileEntry> files) {
    std::vector<const FileEntry*> sorted;
    for (const auto& f : files) sorted.push_back(&f);
    std::ranges::sort(sorted, {}, [](const FileEntry* f) { return f->name; });
    std::string canonical = "parties-build-v1\n";
    for (const FileEntry* f : sorted) {
        canonical += f->name;
        canonical += '\0';
        canonical += std::to_string(f->size);
        canonical += '\0';
        canonical += f->sha256.to_hex();
        canonical += '\n';
    }
    return sha256_of({reinterpret_cast<const uint8_t*>(canonical.data()), canonical.size()});
}

BuildId BuildEntry::id() const {
    return {version, build_digest(files)};
}

std::string write_build_entry(const BuildEntry& entry) {
    rapidjson::StringBuffer buf;
    json::Writer w(buf);
    write_build(w, entry);
    return {buf.GetString(), buf.GetSize()};
}

std::expected<BuildEntry, ManifestError> parse_build_entry(std::string_view text, const ProductSpec& product) {
    rapidjson::Document doc;
    if (!json::parse(doc, text)) return error(ManifestError::Kind::Malformed, "entry is not a JSON object");
    return parse_build(doc, product);
}

std::string write_payload(const ManifestPayload& p) {
    rapidjson::StringBuffer buf;
    json::Writer w(buf);
    w.StartObject();
    json::key(w, "schema"); w.Uint(kPayloadSchema);
    json::key(w, "product"); json::str(w, p.product);
    json::key(w, "channel"); json::str(w, channel_name(p.channel));
    json::key(w, "serial"); w.Uint64(p.serial);
    json::key(w, "issued_at"); json::str(w, format_utc(p.issued_at));
    json::key(w, "expires_at"); json::str(w, format_utc(p.expires_at));
    json::key(w, "builds");
    w.StartArray();
    for (const auto& b : p.builds) write_build(w, b);
    w.EndArray();
    json::key(w, "revoked");
    w.StartArray();
    for (const auto& v : p.revoked) json::str(w, v.to_string());
    w.EndArray();
    w.EndObject();
    return {buf.GetString(), buf.GetSize()};
}

std::vector<uint8_t> signing_message(std::span<const uint8_t> payload_bytes) {
    std::vector<uint8_t> msg(kSigningDomain.begin(), kSigningDomain.end());
    msg.insert(msg.end(), payload_bytes.begin(), payload_bytes.end());
    return msg;
}

std::optional<std::string> sign_manifest(std::span<const uint8_t> payload_bytes,
                                         std::span<const SigningKey> keys) {
    if (keys.empty()) return std::nullopt;
    auto msg = signing_message(payload_bytes);
    rapidjson::StringBuffer buf;
    json::Writer w(buf);
    w.StartObject();
    json::key(w, "format"); w.Uint(1);
    json::key(w, "payload"); json::str(w, base64_encode(payload_bytes));
    json::key(w, "signatures");
    w.StartArray();
    for (const auto& k : keys) {
        parties::Signature sig{};
        if (!parties::ed25519_sign(msg.data(), msg.size(), k.secret_key, k.public_key, sig))
            return std::nullopt;
        w.StartObject();
        json::key(w, "key_id"); json::str(w, k.key_id);
        json::key(w, "sig"); json::str(w, base64_encode(sig));
        w.EndObject();
    }
    w.EndArray();
    w.EndObject();
    return std::string(buf.GetString(), buf.GetSize());
}

std::expected<VerifiedManifest, ManifestError>
verify_manifest(std::span<const uint8_t> envelope_bytes, Keyring keys, Channel expected, const ProductSpec& product) {
    using K = ManifestError::Kind;
    if (keys.empty() || keys.size() > 2) return error(K::InvalidKeyring, "keyring must hold one or two keys");
    if (envelope_bytes.size() > kMaxEnvelopeBytes) return error(K::Malformed, "envelope too large");
    rapidjson::Document doc;
    if (!json::parse(doc, {reinterpret_cast<const char*>(envelope_bytes.data()), envelope_bytes.size()}))
        return error(K::Malformed, "envelope is not a JSON object");
    auto payload_b64 = json::string(doc, "payload");
    const json::Value* sigs = json::array(doc, "signatures");
    if (!payload_b64 || !sigs) return error(K::Malformed, "envelope fields");
    auto payload = base64_decode(*payload_b64);
    if (!payload) return error(K::Malformed, "payload base64");

    auto msg = signing_message(*payload);
    bool verified = false;
    for (const auto& s : sigs->GetArray()) {
        auto key_id = json::string(s, "key_id");
        auto sig_b64 = json::string(s, "sig");
        if (!key_id || !sig_b64) return error(K::Malformed, "signature fields");
        auto key = std::ranges::find(keys, *key_id, &PinnedKey::key_id);
        if (key == keys.end()) continue;
        auto sig_bytes = base64_decode(*sig_b64);
        if (!sig_bytes || sig_bytes->size() != 64) return error(K::BadSignature, "signature encoding");
        parties::Signature sig{};
        std::ranges::copy(*sig_bytes, sig.begin());
        if (!parties::ed25519_verify(msg.data(), msg.size(), sig, key->public_key))
            return error(K::BadSignature, "signature by " + key->key_id);
        verified = true;
    }
    if (!verified) return error(K::UnknownKey, "no signature by a pinned key");

    auto parsed = parse_payload({reinterpret_cast<const char*>(payload->data()), payload->size()}, expected, product);
    if (!parsed) return std::unexpected(parsed.error());
    VerifiedManifest m;
    m.payload_ = std::move(*parsed);
    m.payload_digest_ = sha256_of(*payload);
    return m;
}

bool VerifiedManifest::is_revoked(const Version& version) const {
    return std::ranges::find(payload_.revoked, version) != payload_.revoked.end();
}

const BuildEntry* VerifiedManifest::find(const Version& version) const {
    auto it = std::ranges::find(payload_.builds, version, &BuildEntry::version);
    return it == payload_.builds.end() ? nullptr : &*it;
}

TrustDecision accept(const TrustState& trust, const VerifiedManifest& m, Timestamp now) {
    if (m.serial() < trust.serial) return Rejected{TrustRejection::Replayed};
    if (m.serial() == trust.serial && m.payload_digest() != trust.payload_digest)
        return Rejected{TrustRejection::Equivocation};
    if (m.expires_at() < std::max(now, trust.last_issued_at)) return Rejected{TrustRejection::Stale};
    return Accepted{TrustState{m.serial(), m.payload_digest(), std::max(trust.last_issued_at, m.issued_at())}};
}

} // namespace parties::update
