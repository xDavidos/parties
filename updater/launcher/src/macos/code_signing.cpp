#include "code_signing.h"

#ifdef PARTIES_RETAIL
#include <Security/Security.h>
#include <bsm/libbsm.h>
#include <sys/socket.h>
#include <sys/un.h>
#endif

namespace parties::update::launcher::macos {

#ifdef PARTIES_RETAIL

namespace {

#ifndef PARTIES_APPLE_TEAM_ID
#define PARTIES_APPLE_TEAM_ID ""
#endif

template <class T>
struct Cf {
    T ref = nullptr;
    ~Cf() {
        if (ref) CFRelease(ref);
    }
};

std::string status_text(OSStatus status) {
    Cf<CFStringRef> text{SecCopyErrorMessageString(status, nullptr)};
    char buffer[256] = "";
    if (text.ref) CFStringGetCString(text.ref, buffer, sizeof buffer, kCFStringEncodingUTF8);
    return std::string(buffer) + " (" + std::to_string(status) + ")";
}

std::optional<std::string> requirement(bool host, SecRequirementRef& out) {
    const std::string team = PARTIES_APPLE_TEAM_ID;
    if (team.empty()) return "this retail build pins no Team ID (PARTIES_APPLE_TEAM_ID)";
    std::string text = "anchor apple generic and certificate leaf[subject.OU] = \"" + team + "\"";
    if (host) text += " and identifier \"org.parties.client\"";
    Cf<CFStringRef> string{CFStringCreateWithCString(nullptr, text.c_str(), kCFStringEncodingUTF8)};
    OSStatus status = SecRequirementCreateWithString(string.ref, kSecCSDefaultFlags, &out);
    if (status != errSecSuccess) return "cannot build the code requirement: " + status_text(status);
    return std::nullopt;
}

} // namespace

std::optional<std::string> code_requirement_failure(const std::filesystem::path& code, bool is_host_bundle) {
    Cf<SecRequirementRef> req;
    if (auto why = requirement(is_host_bundle, req.ref)) return why;
    const std::string path = code.string();
    Cf<CFURLRef> url{CFURLCreateFromFileSystemRepresentation(
        nullptr, reinterpret_cast<const UInt8*>(path.data()), CFIndex(path.size()), is_host_bundle)};
    Cf<SecStaticCodeRef> static_code;
    OSStatus status = SecStaticCodeCreateWithPath(url.ref, kSecCSDefaultFlags, &static_code.ref);
    if (status != errSecSuccess) return path + " has no readable signature: " + status_text(status);
    SecCSFlags flags = kSecCSStrictValidate | kSecCSCheckAllArchitectures;
    if (is_host_bundle) flags |= kSecCSCheckNestedCode;
    status = SecStaticCodeCheckValidity(static_code.ref, flags, req.ref);
    if (status != errSecSuccess) return path + " fails the pinned code requirement: " + status_text(status);
    return std::nullopt;
}

std::optional<std::string> peer_requirement_failure(int socket) {
    audit_token_t token;
    socklen_t size = sizeof token;
    if (getsockopt(socket, SOL_LOCAL, LOCAL_PEERTOKEN, &token, &size) != 0) return "no audit token for the peer";
    Cf<SecRequirementRef> req;
    if (auto why = requirement(true, req.ref)) return why;
    Cf<CFDataRef> data{CFDataCreate(nullptr, reinterpret_cast<const UInt8*>(&token), sizeof token)};
    const void* keys[] = {kSecGuestAttributeAudit};
    const void* values[] = {data.ref};
    Cf<CFDictionaryRef> attributes{CFDictionaryCreate(nullptr, keys, values, 1, &kCFTypeDictionaryKeyCallBacks,
                                                      &kCFTypeDictionaryValueCallBacks)};
    Cf<SecCodeRef> code;
    OSStatus status = SecCodeCopyGuestWithAttributes(nullptr, attributes.ref, kSecCSDefaultFlags, &code.ref);
    if (status != errSecSuccess) return "cannot find the peer's code: " + status_text(status);
    status = SecCodeCheckValidity(code.ref, kSecCSDefaultFlags, req.ref);
    if (status != errSecSuccess) return "the peer fails the host requirement: " + status_text(status);
    return std::nullopt;
}

#else

std::optional<std::string> code_requirement_failure(const std::filesystem::path&, bool) { return std::nullopt; }
std::optional<std::string> peer_requirement_failure(int) { return std::nullopt; }

#endif

} // namespace parties::update::launcher::macos
