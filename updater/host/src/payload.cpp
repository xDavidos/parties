#include "payload.h"

#include <CommonCrypto/CommonDigest.h>
#include <dlfcn.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <vector>

namespace parties::host {

namespace {

double ms_since(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

} // namespace

std::expected<update::Sha256, LoadError> digest_file(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return std::unexpected(LoadError::Missing);

    CC_SHA256_CTX context;
    CC_SHA256_Init(&context);
    std::vector<char> chunk(1 << 20);
    while (in.read(chunk.data(), static_cast<std::streamsize>(chunk.size())) || in.gcount() > 0)
        CC_SHA256_Update(&context, chunk.data(), static_cast<CC_LONG>(in.gcount()));
    if (in.bad()) return std::unexpected(LoadError::Missing);

    update::Sha256 digest;
    CC_SHA256_Final(digest.bytes.data(), &context);
    return digest;
}

std::expected<LoadedPayload, LoadError>
load(const std::filesystem::path& file, const std::optional<update::Sha256>& expected, const Entry& entry) {
    const auto hash_start = std::chrono::steady_clock::now();
    const auto digest = digest_file(file);
    const double hash_ms = ms_since(hash_start);
    if (!digest) {
        std::fprintf(stderr, "[host] payload missing: %s\n", file.c_str());
        return std::unexpected(digest.error());
    }
    if (expected && *digest != *expected) {
        std::fprintf(stderr, "[host] payload digest mismatch: %s is %s, expected %s\n",
                     file.c_str(), digest->to_hex().c_str(), expected->to_hex().c_str());
        return std::unexpected(LoadError::DigestMismatch);
    }

    const auto dlopen_start = std::chrono::steady_clock::now();
    void* handle = dlopen(file.c_str(), RTLD_NOW | RTLD_LOCAL);
    const double dlopen_ms = ms_since(dlopen_start);
    if (!handle) {
        std::fprintf(stderr, "[host] payload dlopen failed: %s\n", dlerror());
        return std::unexpected(LoadError::DlopenFailed);
    }

    const auto* min_abi = static_cast<const uint32_t*>(dlsym(handle, entry.min_host_abi));
    auto* run = reinterpret_cast<int (*)(const PartiesHostApi*)>(dlsym(handle, entry.main));
    if (!min_abi || !run) {
        std::fprintf(stderr, "[host] payload lacks its entry points: %s\n", file.c_str());
        return std::unexpected(LoadError::DlopenFailed);
    }
    if (*min_abi > PARTIES_HOST_ABI) {
        std::fprintf(stderr, "[host] payload needs host ABI %u, this host has %u\n", *min_abi, PARTIES_HOST_ABI);
        return std::unexpected(LoadError::HostTooOld);
    }

    std::fprintf(stderr, "[host] payload loaded: %s sha256=%s hash_ms=%.2f dlopen_ms=%.2f\n",
                 file.c_str(), digest->to_hex().c_str(), hash_ms, dlopen_ms);
    return LoadedPayload{handle, run};
}

} // namespace parties::host
