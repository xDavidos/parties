#pragma once

#include <parties/update/sha256_digest.h>
#include <parties/update/version.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace parties::update {

enum class Channel : uint8_t { Stable, Beta };

std::optional<Channel> parse_channel(std::string_view name);
std::string_view channel_name(Channel channel);

using Timestamp = std::chrono::sys_seconds;

inline constexpr std::string_view kProduct = "parties-client-windows";
inline constexpr std::string_view kLauncherFile = "PartiesLauncher.exe";
inline constexpr std::string_view kAppFile = "parties_client.exe";
inline constexpr size_t kMaxEnvelopeBytes = 1 << 20;

using Ed25519PublicKey = std::array<uint8_t, 32>;
using Ed25519SecretKey = std::array<uint8_t, 32>;

// Public keys compiled into the launcher. At most two: current and next.
struct PinnedKey {
    std::string key_id;
    Ed25519PublicKey public_key{};
};
using Keyring = std::span<const PinnedKey>;

struct SigningKey {
    std::string key_id;
    Ed25519SecretKey secret_key{};
    Ed25519PublicKey public_key{};
};

struct FileEntry {
    std::string name;  // a bare file name inside the build directory
    uint64_t size = 0;
    Sha256 sha256;
    std::vector<std::string> urls;  // https only, tried in order

    friend bool operator==(const FileEntry&, const FileEntry&) = default;
};

// One build. Every build carries both kLauncherFile and kAppFile.
struct BuildEntry {
    Version version;
    uint32_t data_schema = 1;
    uint32_t data_compat_floor = 1;
    uint32_t link_protocol = 1;                // newest launcher link protocol the app speaks
    uint32_t min_supported_launcher_link = 1;  // oldest launcher protocol the app negotiates down to
    std::vector<FileEntry> files;

    // Version plus build_digest(files).
    BuildId id() const;
    const FileEntry* file(std::string_view name) const;

    friend bool operator==(const BuildEntry&, const BuildEntry&) = default;
};

Sha256 sha256_of(std::span<const uint8_t> bytes);

// SHA-256 over the sorted (name, size, sha256) list. URLs are excluded, so a
// mirror change never changes the build's identity.
Sha256 build_digest(std::span<const FileEntry> files);

struct ManifestPayload {
    std::string product{kProduct};
    Channel channel = Channel::Stable;
    uint64_t serial = 0;  // strictly increasing per channel, >= 1
    Timestamp issued_at{};
    Timestamp expires_at{};
    std::vector<BuildEntry> builds;  // newest first after verification
    std::vector<Version> revoked;

    friend bool operator==(const ManifestPayload&, const ManifestPayload&) = default;
};

// Canonical payload bytes, as the release tool signs them.
std::string write_payload(const ManifestPayload& payload);

// The exact bytes a signature covers: "parties-update-manifest-v1\0" || payload.
std::vector<uint8_t> signing_message(std::span<const uint8_t> payload_bytes);

// Envelope JSON {format, payload: base64, signatures: [{key_id, sig}]}, signed
// by every key given (two during a rotation). nullopt if signing failed.
std::optional<std::string> sign_manifest(std::span<const uint8_t> payload_bytes,
                                         std::span<const SigningKey> keys);

struct ManifestError {
    enum class Kind : uint8_t {
        Malformed,       // not an envelope or payload we can read
        InvalidKeyring,  // empty, or more than two keys
        UnknownKey,      // no signature by a pinned key
        BadSignature,    // a pinned key's signature does not verify
        WrongProduct,
        WrongChannel,
        InvalidField,    // path in a file name, non-https URL, duplicate version, ...
    } kind;
    std::string detail;
};

// One build entry as canonical JSON: the exact bytes write_payload emits for
// it, so an entry copied between channels stays byte for byte the same.
std::string write_build_entry(const BuildEntry& entry);
// Parses one entry with the rules verify_manifest applies to each build.
std::expected<BuildEntry, ManifestError> parse_build_entry(std::string_view json);

// Holding one proves: a pinned key signed these payload bytes, the product and
// channel match, and every field passed validation. Freshness is separate:
// see accept().
class VerifiedManifest {
public:
    const ManifestPayload& payload() const { return payload_; }
    Channel channel() const { return payload_.channel; }
    uint64_t serial() const { return payload_.serial; }
    Timestamp issued_at() const { return payload_.issued_at; }
    Timestamp expires_at() const { return payload_.expires_at; }
    std::span<const BuildEntry> builds() const { return payload_.builds; }
    const Sha256& payload_digest() const { return payload_digest_; }

    bool is_revoked(const Version& version) const;
    const BuildEntry* find(const Version& version) const;

private:
    VerifiedManifest() = default;
    friend std::expected<VerifiedManifest, ManifestError>
    verify_manifest(std::span<const uint8_t> envelope_bytes, Keyring keys, Channel expected);

    ManifestPayload payload_;
    Sha256 payload_digest_;
};

std::expected<VerifiedManifest, ManifestError>
verify_manifest(std::span<const uint8_t> envelope_bytes, Keyring keys, Channel expected);

// Anti-replay and anti-freeze memory for one channel (trust.json).
struct TrustState {
    uint64_t serial = 0;
    Sha256 payload_digest;
    Timestamp last_issued_at{};

    friend bool operator==(const TrustState&, const TrustState&) = default;
};

enum class TrustRejection : uint8_t {
    Replayed,      // serial below the remembered one
    Equivocation,  // same serial, different payload bytes
    Stale,         // expires_at < max(now, last_issued_at)
};

struct Accepted { TrustState next; };
struct Rejected { TrustRejection reason; };
using TrustDecision = std::variant<Accepted, Rejected>;

// A rejected or stale manifest only stops offers; the running build is never
// affected by it.
TrustDecision accept(const TrustState& trust, const VerifiedManifest& manifest, Timestamp now);

} // namespace parties::update
