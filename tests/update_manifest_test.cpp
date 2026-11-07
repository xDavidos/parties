#include <parties/update/manifest.h>

#include <parties/crypto.h>

#include <chrono>
#include <functional>
#include <iostream>
#include <string>

using namespace parties::update;
using namespace std::chrono;
using Kind = ManifestError::Kind;

namespace {

bool fail(const std::string& msg) {
    std::cerr << "update_manifest_test: " << msg << "\n";
    return false;
}

SigningKey make_key(std::string id) {
    SigningKey k;
    k.key_id = std::move(id);
    if (!parties::random_bytes(k.secret_key.data(), k.secret_key.size()) ||
        !parties::derive_pubkey(k.secret_key, k.public_key))
        std::abort();
    return k;
}

PinnedKey pin(const SigningKey& k) { return {k.key_id, k.public_key}; }

Sha256 digest_of(char fill) {
    return *Sha256::from_hex(std::string(64, fill));
}

FileEntry file(std::string name, char fill) {
    return {std::move(name), 1000, digest_of(fill), {"https://github.com/emcifuntik/parties/releases/download/x/" + std::string(1, fill)}};
}

BuildEntry build(const char* version, uint32_t schema = 2) {
    BuildEntry b;
    b.version = *Version::parse(version);
    b.data_schema = schema;
    b.data_compat_floor = 1;
    b.link_protocol = 2;
    b.min_supported_launcher_link = 1;
    b.files = {file(std::string(kWindowsClient.launcher_file), 'a'), file(std::string(kWindowsClient.app_file), 'b')};
    return b;
}

const Timestamp kIssued = sys_days{2026y / October / 31} + 12h;

ManifestPayload payload(Channel channel = Channel::Beta) {
    ManifestPayload p;
    p.product = kWindowsClient.id;
    p.channel = channel;
    p.serial = 10;
    p.issued_at = kIssued;
    p.expires_at = kIssued + days{45};
    p.builds = {build("1.5.2.0"), build("1.6.0.0", 3)};
    p.revoked = {*Version::parse("1.5.0.0")};
    return p;
}

std::vector<uint8_t> bytes(std::string_view s) { return {s.begin(), s.end()}; }

std::string sign(const std::string& payload_text, std::initializer_list<SigningKey> keys) {
    auto env = sign_manifest(bytes(payload_text), std::vector<SigningKey>(keys));
    if (!env) std::abort();
    return *env;
}

std::expected<VerifiedManifest, ManifestError> verify(const std::string& envelope,
                                                      std::initializer_list<PinnedKey> ring,
                                                      Channel channel = Channel::Beta,
                                                      const ProductSpec& product = kWindowsClient) {
    std::vector<PinnedKey> keys(ring);
    return verify_manifest(bytes(envelope), keys, channel, product);
}

bool expect_error(const std::expected<VerifiedManifest, ManifestError>& r, Kind kind, const std::string& what) {
    if (r) return fail(what + ": accepted");
    if (r.error().kind != kind)
        return fail(what + ": wrong error kind " + std::to_string(int(r.error().kind)) + " (" + r.error().detail + ")");
    return true;
}

const SigningKey kA = make_key("release-2026a");
const SigningKey kB = make_key("release-2027a");
const SigningKey kStranger = make_key("someone-else");

bool test_round_trip() {
    ManifestPayload p = payload();
    auto r = verify(sign(write_payload(p), {kA}), {pin(kA)});
    if (!r) return fail("valid manifest rejected: " + r.error().detail);
    if (r->channel() != Channel::Beta || r->serial() != 10) return fail("header fields lost");
    if (r->issued_at() != p.issued_at || r->expires_at() != p.expires_at) return fail("timestamps lost");
    if (r->builds().size() != 2 || r->builds()[0].version != *Version::parse("1.6.0.0"))
        return fail("builds are not newest first");
    if (r->builds()[0] != p.builds[1] || r->builds()[1] != p.builds[0]) return fail("build entry fields lost");
    if (!r->is_revoked(*Version::parse("1.5.0.0")) || r->is_revoked(*Version::parse("1.5.2.0")))
        return fail("revocation list lost");
    if (!r->find(*Version::parse("1.5.2.0"))) return fail("find by version");
    return true;
}

bool test_single_byte_flips_fail() {
    std::string text = write_payload(payload());
    auto good_env = sign(text, {kA});
    std::string original_signatures = good_env.substr(good_env.find("\"signatures\""));
    for (size_t i = 0; i < text.size(); ++i) {
        std::string tampered = text;
        tampered[i] = char(tampered[i] ^ 0x01);
        auto tampered_env = sign(tampered, {kA});
        std::string spliced = tampered_env.substr(0, tampered_env.find("\"signatures\"")) + original_signatures;
        if (verify(spliced, {pin(kA)})) return fail("payload byte flip at " + std::to_string(i) + " verified");
    }
    auto sig_start = good_env.find("\"sig\":\"") + 7;
    auto sig_end = good_env.find('"', sig_start);
    for (size_t i = sig_start; i < sig_end; ++i) {
        std::string tampered = good_env;
        tampered[i] = tampered[i] == 'A' ? 'B' : 'A';
        if (verify(tampered, {pin(kA)})) return fail("signature char flip at " + std::to_string(i) + " verified");
    }
    return true;
}

bool test_keys_and_rotation() {
    std::string text = write_payload(payload());
    if (!expect_error(verify(sign(text, {kStranger}), {pin(kA)}), Kind::UnknownKey, "unknown key id"))
        return false;
    if (!verify(sign(text, {kB}), {pin(kA), pin(kB)})) return fail("second keyring key does not verify");
    if (!verify(sign(text, {kA, kB}), {pin(kA)})) return fail("an extra signature by a future key broke verification");
    SigningKey impostor = kStranger;
    impostor.key_id = kA.key_id;
    if (!expect_error(verify(sign(text, {impostor}), {pin(kA)}), Kind::BadSignature, "forged key id"))
        return false;
    if (!expect_error(verify(sign(text, {kA}), {}), Kind::InvalidKeyring, "empty keyring")) return false;
    if (!expect_error(verify(sign(text, {kA}), {pin(kA), pin(kB), pin(kStranger)}), Kind::InvalidKeyring,
                      "three-key keyring"))
        return false;
    return true;
}

bool expect_payload_error(const ManifestPayload& p, Kind kind, const std::string& what,
                          Channel channel = Channel::Beta) {
    return expect_error(verify(sign(write_payload(p), {kA}), {pin(kA)}, channel), kind, what);
}

bool test_payload_validation() {
    bool ok = true;
    ok &= expect_payload_error(payload(Channel::Stable), Kind::WrongChannel, "beta request served stable");
    ok &= expect_payload_error(payload(Channel::Beta), Kind::WrongChannel, "stable request served beta", Channel::Stable);
    {
        auto p = payload();
        p.product = "parties-client-macos";
        ok &= expect_payload_error(p, Kind::WrongProduct, "wrong product");
    }
    for (const char* name : {"../PartiesLauncher.exe", "builds/evil.exe", "builds\\evil.exe", "C:evil.exe", "..",
                             ".hidden", "CON.exe", "nul", "lpt1.dll", "evil .exe", ""}) {
        auto p = payload();
        p.builds[0].files.push_back(file(name, 'c'));
        ok &= expect_payload_error(p, Kind::InvalidField, std::string("file name '") + name + "'");
    }
    for (const char* url : {"http://github.com/x", "ftp://github.com/x", "https://", "https:///path",
                            "https://user@evil.example/x", "https://github.com/a b", "file:///C:/x"}) {
        auto p = payload();
        p.builds[0].files[0].urls = {url};
        ok &= expect_payload_error(p, Kind::InvalidField, std::string("url '") + url + "'");
    }
    {
        auto p = payload();
        p.builds.push_back(build("1.6.0.0"));
        ok &= expect_payload_error(p, Kind::InvalidField, "duplicate version");
    }
    {
        auto p = payload();
        p.builds[0].files.erase(p.builds[0].files.begin());
        ok &= expect_payload_error(p, Kind::InvalidField, "build without launcher");
    }
    {
        auto p = payload();
        p.builds[0].files.push_back(file("partieslauncher.EXE", 'c'));
        ok &= expect_payload_error(p, Kind::InvalidField, "case-insensitive duplicate file");
    }
    {
        auto p = payload();
        p.builds[0].files[0].size = 0;
        ok &= expect_payload_error(p, Kind::InvalidField, "zero size");
    }
    {
        auto p = payload();
        p.builds[0].data_compat_floor = 3;
        ok &= expect_payload_error(p, Kind::InvalidField, "floor above schema");
    }
    {
        auto p = payload();
        p.builds[0].min_supported_launcher_link = 3;
        ok &= expect_payload_error(p, Kind::InvalidField, "min link above link");
    }
    {
        auto p = payload();
        p.expires_at = p.issued_at - seconds{1};
        ok &= expect_payload_error(p, Kind::InvalidField, "expires before issued");
    }
    {
        auto p = payload();
        p.serial = 0;
        ok &= expect_payload_error(p, Kind::InvalidField, "serial 0");
    }
    {
        std::string text = write_payload(payload());
        auto pos = text.find(std::string(64, 'a'));
        std::string upper = text;
        upper.replace(pos, 64, std::string(64, 'A'));
        ok &= expect_error(verify(sign(upper, {kA}), {pin(kA)}), Kind::InvalidField, "uppercase sha256");
        std::string short_sha = text;
        short_sha.replace(pos, 64, std::string(63, 'a'));
        ok &= expect_error(verify(sign(short_sha, {kA}), {pin(kA)}), Kind::InvalidField, "short sha256");
        std::string future = text;
        future.replace(future.find("\"schema\":1"), 10, "\"schema\":2");
        ok &= expect_error(verify(sign(future, {kA}), {pin(kA)}), Kind::Malformed, "future payload schema");
    }
    ok &= expect_error(verify("not json", {pin(kA)}), Kind::Malformed, "garbage envelope");
    ok &= expect_error(verify(R"({"payload":"!!!","signatures":[]})", {pin(kA)}), Kind::Malformed, "bad base64");
    return ok;
}

bool test_build_identity() {
    BuildEntry a = build("1.6.0.0");
    BuildEntry other_launcher = a;
    other_launcher.files[0].sha256 = digest_of('c');
    if (a.id() == other_launcher.id()) return fail("a different launcher must change the build id");
    BuildEntry mirrored = a;
    mirrored.files[0].urls = {"https://mirror.example/x"};
    std::swap(mirrored.files[0], mirrored.files[1]);
    if (a.id() != mirrored.id()) return fail("urls or file order changed the build id");
    return true;
}

bool test_build_entry_codec() {
    ManifestPayload p = payload();
    std::string text = write_build_entry(p.builds[1]);
    if (write_payload(p).find(text) == std::string::npos)
        return fail("an entry must serialize to the same bytes it has inside a payload");
    auto parsed = parse_build_entry(text, kWindowsClient);
    if (!parsed || *parsed != p.builds[1]) return fail("entry round trip");
    BuildEntry no_app = p.builds[1];
    no_app.files.pop_back();
    auto refused = parse_build_entry(write_build_entry(no_app), kWindowsClient);
    if (refused || refused.error().kind != Kind::InvalidField) return fail("an entry without the app must be refused");
    return true;
}

// A manifest verifies only as the product it names, and each product's builds
// must carry that product's launcher and app.
bool test_products() {
    bool ok = true;
    ManifestPayload mac = payload();
    mac.product = kMacClient.id;
    for (BuildEntry& b : mac.builds)
        b.files = {file(std::string(kMacClient.launcher_file), 'a'), file(std::string(kMacClient.app_file), 'b')};
    const std::string mac_envelope = sign(write_payload(mac), {kA});
    if (!verify(mac_envelope, {pin(kA)}, Channel::Beta, kMacClient)) ok &= fail("a macOS manifest verifies as macOS");
    ok &= expect_error(verify(mac_envelope, {pin(kA)}, Channel::Beta, kWindowsClient), Kind::WrongProduct,
                       "a macOS manifest read by Windows");
    ok &= expect_error(verify(sign(write_payload(payload()), {kA}), {pin(kA)}, Channel::Beta, kMacClient),
                       Kind::WrongProduct, "a Windows manifest read by macOS");
    ManifestPayload windows_files = payload();
    windows_files.product = kMacClient.id;
    ok &= expect_error(verify(sign(write_payload(windows_files), {kA}), {pin(kA)}, Channel::Beta, kMacClient),
                       Kind::InvalidField, "a macOS build of Windows files");
    auto entry = parse_build_entry(write_build_entry(mac.builds[0]), kMacClient);
    if (!entry || *entry != mac.builds[0]) ok &= fail("a macOS entry round trips");
    auto refused = parse_build_entry(write_build_entry(mac.builds[0]), kWindowsClient);
    if (refused || refused.error().kind != Kind::InvalidField) ok &= fail("Windows refuses an entry of dylibs");
    return ok;
}

VerifiedManifest verified(const ManifestPayload& p) {
    auto r = verify(sign(write_payload(p), {kA}), {pin(kA)});
    if (!r) std::abort();
    return *r;
}

bool is_rejected(const TrustDecision& d, TrustRejection reason) {
    auto* r = std::get_if<Rejected>(&d);
    return r && r->reason == reason;
}

bool test_trust_ratchet() {
    auto p = payload();
    VerifiedManifest m = verified(p);
    Timestamp now = kIssued + days{1};

    auto first = accept(TrustState{}, m, now);
    auto* a = std::get_if<Accepted>(&first);
    if (!a) return fail("first manifest rejected");
    if (a->next.serial != 10 || a->next.payload_digest != m.payload_digest() || a->next.last_issued_at != kIssued)
        return fail("ratchet did not remember serial, digest and issued_at");

    auto again = accept(a->next, m, now);
    auto* same = std::get_if<Accepted>(&again);
    if (!same || same->next != a->next) return fail("refetching the same manifest must be a no-op accept");

    auto older = p;
    older.serial = 9;
    if (!is_rejected(accept(a->next, verified(older), now), TrustRejection::Replayed)) return fail("replay accepted");

    auto forked = p;
    forked.builds.pop_back();
    if (!is_rejected(accept(a->next, verified(forked), now), TrustRejection::Equivocation))
        return fail("equivocation accepted");

    if (!is_rejected(accept(a->next, m, p.expires_at + seconds{1}), TrustRejection::Stale))
        return fail("expired manifest accepted");
    if (!std::holds_alternative<Accepted>(accept(a->next, m, p.expires_at)))
        return fail("manifest rejected at exactly expires_at");

    TrustState seen_later = a->next;
    seen_later.last_issued_at = p.expires_at + days{1};
    if (!is_rejected(accept(seen_later, m, kIssued), TrustRejection::Stale))
        return fail("a clock set back must not revive an expired manifest");

    auto newer = p;
    newer.serial = 11;
    newer.issued_at = kIssued - hours{1};
    auto next = accept(a->next, verified(newer), now);
    auto* n = std::get_if<Accepted>(&next);
    if (!n || n->next.serial != 11 || n->next.last_issued_at != kIssued)
        return fail("last_issued_at must never move backwards");
    return true;
}

} // namespace

int main() {
    bool ok = true;
    ok &= test_round_trip();
    ok &= test_single_byte_flips_fail();
    ok &= test_keys_and_rotation();
    ok &= test_payload_validation();
    ok &= test_build_identity();
    ok &= test_build_entry_codec();
    ok &= test_products();
    ok &= test_trust_ratchet();
    if (ok) std::cout << "update_manifest_test: all passed\n";
    return ok ? 0 : 1;
}
