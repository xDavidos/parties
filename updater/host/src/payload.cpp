#include "payload.h"

#include <CommonCrypto/CommonDigest.h>
#include <dlfcn.h>
#ifdef PARTIES_RETAIL
#include <Security/Security.h>
#endif

#include <cstdio>
#include <fstream>
#include <vector>

namespace parties::host {

#ifdef PARTIES_RETAIL
bool meets_requirement(const std::filesystem::path& file) {
#ifndef PARTIES_APPLE_TEAM_ID
#define PARTIES_APPLE_TEAM_ID ""
#endif
    const std::string team = PARTIES_APPLE_TEAM_ID;
    const std::string text = "anchor apple generic and certificate leaf[subject.OU] = \"" + team + "\"";
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(nullptr, reinterpret_cast<const UInt8*>(file.c_str()),
                                                           CFIndex(file.native().size()), false);
    CFStringRef string = CFStringCreateWithCString(nullptr, text.c_str(), kCFStringEncodingUTF8);
    SecStaticCodeRef code = nullptr;
    SecRequirementRef requirement = nullptr;
    const bool ok = !team.empty() && SecStaticCodeCreateWithPath(url, kSecCSDefaultFlags, &code) == errSecSuccess &&
                    SecRequirementCreateWithString(string, kSecCSDefaultFlags, &requirement) == errSecSuccess &&
                    SecStaticCodeCheckValidity(code, kSecCSStrictValidate, requirement) == errSecSuccess;
    for (CFTypeRef ref : {CFTypeRef(url), CFTypeRef(string), CFTypeRef(code), CFTypeRef(requirement)})
        if (ref) CFRelease(ref);
    if (!ok) std::fprintf(stderr, "[host] %s fails the pinned code requirement\n", file.c_str());
    return ok;
}
#else
bool meets_requirement(const std::filesystem::path&) { return true; }
#endif

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
    const auto digest = digest_file(file);
    if (!digest) {
        std::fprintf(stderr, "[host] payload missing: %s\n", file.c_str());
        return std::unexpected(digest.error());
    }
    if (expected && *digest != *expected) {
        std::fprintf(stderr, "[host] payload digest mismatch: %s is %s, expected %s\n",
                     file.c_str(), digest->to_hex().c_str(), expected->to_hex().c_str());
        return std::unexpected(LoadError::DigestMismatch);
    }

    void* handle = dlopen(file.c_str(), RTLD_NOW | RTLD_LOCAL);
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

    std::fprintf(stderr, "[host] payload loaded: %s sha256=%s\n", file.c_str(), digest->to_hex().c_str());
    return LoadedPayload{handle, run};
}

} // namespace parties::host
