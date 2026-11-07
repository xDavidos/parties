// The only code in the repository that dlopens a Parties payload.
// tools/update/check_host.sh fails CI if dlopen appears anywhere else.

#pragma once

#include <parties/host_api.h>
#include <parties/update/sha256_digest.h>

#include <expected>
#include <filesystem>
#include <optional>

namespace parties::host {

struct LoadedPayload {
    void* handle = nullptr;  // never dlclose'd: the process ends with it
    int (*entry)(const PartiesHostApi*) = nullptr;
};

// The names of the two symbols a payload exports.
struct Entry {
    const char* min_host_abi;  // const uint32_t: the lowest host ABI it needs
    const char* main;          // int (const PartiesHostApi*)
};

enum class LoadError : uint8_t {
    Missing,
    DigestMismatch,
    DlopenFailed,  // includes library validation refusing the mapping
    HostTooOld,
};

// Retail: the file carries a Developer ID signature of the Team ID the build
// pins (PARTIES_APPLE_TEAM_ID). Other builds are ad-hoc signed and pass.
bool meets_requirement(const std::filesystem::path& file);

// SHA-256 of the file, read in 1 MiB chunks.
std::expected<update::Sha256, LoadError> digest_file(const std::filesystem::path& file);

// Hashes the file, compares it with `expected` when given, then dlopens it and
// checks its ABI. dlopen runs the image's static initializers, so nothing from
// the file runs before its digest matches.
std::expected<LoadedPayload, LoadError>
load(const std::filesystem::path& file, const std::optional<update::Sha256>& expected, const Entry& entry);

} // namespace parties::host
