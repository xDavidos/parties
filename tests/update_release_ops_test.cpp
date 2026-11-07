#include "release_ops.h"

#include <chrono>
#include <iostream>
#include <string>

using namespace parties::update;
using namespace parties::update::release;
using namespace std::chrono;

namespace {

bool fail(const std::string& msg) {
    std::cerr << "update_release_ops_test: " << msg << "\n";
    return false;
}

const Timestamp kNow = sys_days{2026y / October / 31} + 12h;

BuildEntry build(const char* version, uint32_t link = 1, uint32_t min_link = 1) {
    BuildEntry b;
    b.version = *Version::parse(version);
    b.data_schema = 2;
    b.link_protocol = link;
    b.min_supported_launcher_link = min_link;
    b.files = {{std::string(kWindowsClient.launcher_file), 10, *Sha256::from_hex(std::string(64, 'a')), {"https://h/l"}},
               {std::string(kWindowsClient.app_file), 20, *Sha256::from_hex(std::string(64, 'b')), {"https://h/a"}}};
    return b;
}

Issue issue(uint64_t serial) { return {serial, kNow, days{45}}; }

std::string versions(const std::vector<BuildEntry>& builds) {
    std::string out;
    for (const auto& b : builds) out += b.version.to_string() + " ";
    return out;
}

bool test_build_info() {
    BuildInfo info{"1.6.0.0", 3, 2, 2, 1};
    auto parsed = parse_build_info(format_build_info(info));
    if (!parsed || parsed->version != "1.6.0.0" || parsed->data_schema != 3 || parsed->data_compat_floor != 2 ||
        parsed->link_protocol != 2 || parsed->min_supported_launcher_link != 1)
        return fail("build info round trip");
    if (parse_build_info(R"({"schema":1,"version":"v1.6.0","data_schema":1,"data_compat_floor":1,"link_protocol":1,"min_supported_launcher_link":1})"))
        return fail("a non A.B.C.D version must be refused");
    if (parse_build_info(R"({"schema":2,"version":"1.6.0.0"})")) return fail("an unknown build info schema must be refused");
    return true;
}

bool test_make_entry() {
    StagedFile files[] = {{std::string(kWindowsClient.launcher_file), "PartiesLauncher-1.6.0.0.exe", 5, *Sha256::from_hex(std::string(64, 'c'))},
                          {std::string(kWindowsClient.app_file), "parties_client-1.6.0.0.exe", 6, *Sha256::from_hex(std::string(64, 'd'))}};
    BuildInfo info{"1.6.0.0", 3, 2, 2, 1};
    auto entry = make_entry(kWindowsClient, info, files, "https://github.com/o/r/releases/download/v1.6.0.0/");
    if (!entry || entry->files[1].urls.front() != "https://github.com/o/r/releases/download/v1.6.0.0/parties_client-1.6.0.0.exe")
        return fail("the url is url_base + asset name");
    if (make_entry(kWindowsClient, info, files, "http://github.com/o/r/")) return fail("a non-https url base must be refused");
    if (make_entry(kWindowsClient, info, std::span(files, 1), "https://h/")) return fail("an entry without the app must be refused");
    return true;
}

bool test_retention() {
    std::vector<BuildEntry> builds{build("1.0.0.0"), build("1.4.0.0"), build("1.2.0.0"), build("1.3.0.0")};
    if (versions(retained(builds, 2)) != "1.4.0.0 1.3.0.0 ") return fail("keeps the newest n: " + versions(retained(builds, 2)));
    builds.push_back(build("1.5.0.0", 2, 2));
    builds.push_back(build("1.6.0.0", 2, 2));
    if (versions(retained(builds, 2)) != "1.6.0.0 1.5.0.0 1.4.0.0 ")
        return fail("keeps the bridge for older launchers: " + versions(retained(builds, 2)));
    return true;
}

bool test_add_build() {
    auto first = add_build(kWindowsClient, nullptr, Channel::Beta, build("1.4.0.0"), issue(5), 3);
    if (!first || first->serial != 5 || first->channel != Channel::Beta || first->expires_at != kNow + days{45})
        return fail("add to an empty channel");
    BuildEntry rebuilt = build("1.4.0.0");
    rebuilt.files[1].size = 21;
    auto replaced = add_build(kWindowsClient, &*first, Channel::Beta, rebuilt, issue(6), 3);
    if (!replaced || replaced->builds.size() != 1 || replaced->builds[0] != rebuilt)
        return fail("the same version replaces its entry");
    if (add_build(kWindowsClient, &*replaced, Channel::Beta, build("1.5.0.0"), issue(6), 3)) return fail("an equal serial must be refused");
    if (add_build(kWindowsClient, &*replaced, Channel::Beta, build("1.5.0.0"), issue(2), 3)) return fail("a lower serial must be refused");
    if (add_build(kWindowsClient, &*replaced, Channel::Stable, build("1.5.0.0"), issue(7), 3)) return fail("a base of another channel must be refused");
    auto revoked = revoke(kWindowsClient, &*replaced, Channel::Beta, *Version::parse("1.4.0.0"), issue(7));
    if (add_build(kWindowsClient, &*revoked, Channel::Beta, build("1.4.0.0"), issue(8), 3)) return fail("a revoked version must not come back");
    auto two = add_build(kWindowsClient, &*revoked, Channel::Beta, build("1.6.0.0"), issue(8), 1);
    if (add_build(kWindowsClient, &*two, Channel::Beta, build("1.5.0.0"), issue(9), 1))
        return fail("an entry the retention drops at once must be refused");
    return true;
}

bool test_promote() {
    auto beta = add_build(kWindowsClient, nullptr, Channel::Beta, build("1.5.0.0"), issue(5), 3);
    beta = add_build(kWindowsClient, &*beta, Channel::Beta, build("1.6.0.0", 2, 1), issue(6), 3);
    auto stable = promote(kWindowsClient, *beta, nullptr, *Version::parse("1.6.0.0"), issue(1), 3);
    if (!stable || stable->channel != Channel::Stable || stable->builds.size() != 1 || stable->builds[0] != beta->builds[0])
        return fail("promotion copies beta's entry unchanged");
    if (promote(kWindowsClient, *beta, &*stable, *Version::parse("1.7.0.0"), issue(2), 3)) return fail("a version not in beta must be refused");
    auto beta_revoked = revoke(kWindowsClient, &*beta, Channel::Beta, *Version::parse("1.5.0.0"), issue(7));
    if (promote(kWindowsClient, *beta_revoked, &*stable, *Version::parse("1.5.0.0"), issue(2), 3))
        return fail("a version revoked in beta must not be promoted");
    return true;
}

bool test_revoke_and_reissue() {
    auto base = add_build(kWindowsClient, nullptr, Channel::Stable, build("1.5.0.0"), issue(5), 3);
    base = add_build(kWindowsClient, &*base, Channel::Stable, build("1.6.0.0"), issue(6), 3);
    auto once = revoke(kWindowsClient, &*base, Channel::Stable, *Version::parse("1.6.0.0"), issue(7));
    if (!once || versions(once->builds) != "1.5.0.0 " || once->revoked != std::vector{*Version::parse("1.6.0.0")})
        return fail("revoke drops the build and lists it");
    auto twice = revoke(kWindowsClient, &*once, Channel::Stable, *Version::parse("1.6.0.0"), issue(8));
    if (!twice || twice->revoked.size() != 1 || twice->builds != once->builds) return fail("revoking again only reissues");
    if (revoke(kWindowsClient, &*once, Channel::Beta, *Version::parse("1.6.0.0"), issue(9))) return fail("a base of another channel must be refused");
    auto fresh = revoke(kWindowsClient, nullptr, Channel::Stable, *Version::parse("1.6.0.0"), issue(1));
    if (!fresh || fresh->channel != Channel::Stable || !fresh->builds.empty() ||
        fresh->revoked != std::vector{*Version::parse("1.6.0.0")})
        return fail("revoking from a channel with no manifest lists the version in a new one");
    auto resigned = reissue(*twice, {9, kNow + days{7}, days{45}});
    if (!resigned || resigned->serial != 9 || resigned->builds != twice->builds || resigned->revoked != twice->revoked ||
        resigned->expires_at != kNow + days{52})
        return fail("reissue keeps builds and revocations and moves serial and expiry");
    return true;
}

} // namespace

int main() {
    bool ok = true;
    ok &= test_build_info();
    ok &= test_make_entry();
    ok &= test_retention();
    ok &= test_add_build();
    ok &= test_promote();
    ok &= test_revoke_and_reissue();
    if (ok) std::cout << "update_release_ops_test: all passed\n";
    return ok ? 0 : 1;
}
