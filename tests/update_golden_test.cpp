// The bytes Windows launchers in the field read and write: a manifest payload,
// a sealed build's build.json, supervisor.json and boot-contract.json. The
// expected bytes were captured by running this test against 45912b4, before
// the update core learned about a second product, and must never change.

#include <parties/update/boot.h>
#include <parties/update/manifest.h>
#include <parties/update/state_codec.h>
#include <parties/update/supervisor_state.h>

#include <parties/crypto.h>

#if defined(_WIN32)
#include "build_info.h"
#endif

#include <chrono>
#include <cstdio>
#include <iostream>
#include <span>
#include <string>
#include <vector>

using namespace parties::update;
using namespace std::chrono;

namespace {

const std::string kPayload =
    "{\"schema\":1,\"product\":\"parties-client-windows\",\"channel\":\"beta\",\"serial\":17,\"issued_at\":\"2026-10-31T12:00:00Z\","
    "\"expires_at\":\"2026-12-15T12:00:00Z\",\"builds\":[{\"version\":\"1.6.0.0\",\"data_schema\":3,"
    "\"data_compat_floor\":2,\"link_protocol\":4,\"min_supported_launcher_link\":2,\"files\":[{\"name\":\"PartiesLauncher.exe\","
    "\"size\":2412544,\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\","
    "\"urls\":[\"https://github.com/emcifuntik/parties/releases/download/v1.6.0.0/PartiesLauncher.exe\","
    "\"https://mirror.example/l\"]},{\"name\":\"parties_client.exe\",\"size\":41943040,\"sha256\":\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\","
    "\"urls\":[\"https://github.com/emcifuntik/parties/releases/download/v1.6.0.0/parties_client.exe\"]}]},"
    "{\"version\":\"1.5.2.0\",\"data_schema\":2,\"data_compat_floor\":1,\"link_protocol\":2,\"min_supported_launcher_link\":1,"
    "\"files\":[{\"name\":\"PartiesLauncher.exe\",\"size\":2412544,\"sha256\":\"3333333333333333333333333333333333333333333333333333333333333333\","
    "\"urls\":[\"https://github.com/emcifuntik/parties/releases/download/v1.5.2.0/PartiesLauncher.exe\","
    "\"https://mirror.example/l\"]},{\"name\":\"parties_client.exe\",\"size\":41943040,\"sha256\":\"4444444444444444444444444444444444444444444444444444444444444444\","
    "\"urls\":[\"https://github.com/emcifuntik/parties/releases/download/v1.5.2.0/parties_client.exe\"]}]}],"
    "\"revoked\":[\"1.5.9.0\"]}";

#if defined(_WIN32)
const std::string kBuildJson =
    "{\n"
    "    \"format\": 1,\n"
    "    \"version\": \"1.6.0.0\",\n"
    "    \"data_schema\": 3,\n"
    "    \"data_compat_floor\": 2,\n"
    "    \"link_protocol\": 4,\n"
    "    \"min_supported_launcher_link\": 2,\n"
    "    \"files\": [\n"
    "        {\n"
    "            \"name\": \"PartiesLauncher.exe\",\n"
    "            \"size\": 2412544,\n"
    "            \"sha256\": \"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"\n"
    "        },\n"
    "        {\n"
    "            \"name\": \"parties_client.exe\",\n"
    "            \"size\": 41943040,\n"
    "            \"sha256\": \"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\"\n"
    "        }\n"
    "    ]\n"
    "}";
#endif

const std::string kSupervisorJson =
    "{\"schema\":1,\"generation\":9,\"channel\":\"beta\",\"intent\":{\"kind\":\"pinned_until_at_least\","
    "\"version\":\"1.6.0.0\"},\"good\":{\"version\":\"1.5.2.0\",\"digest\":\"03177f3e4426bb623e83c3c4901c891b0b576d0ef129564202717e27e004432d\"},"
    "\"previous_good\":{\"version\":\"1.5.1.0\",\"digest\":\"ab24664b63d3282b870902be5298478b25903caf86e785715887c1c31730c1ef\"},"
    "\"builds\":[{\"id\":{\"version\":\"1.5.2.0\",\"digest\":\"03177f3e4426bb623e83c3c4901c891b0b576d0ef129564202717e27e004432d\"},"
    "\"data_schema\":2,\"data_compat_floor\":1,\"link_protocol\":2,\"min_supported_launcher_link\":1,"
    "\"launcher_sha256\":\"3333333333333333333333333333333333333333333333333333333333333333\",\"app_sha256\":\"4444444444444444444444444444444444444444444444444444444444444444\","
    "\"verdict\":{\"kind\":\"proven\"}},{\"id\":{\"version\":\"1.5.1.0\",\"digest\":\"ab24664b63d3282b870902be5298478b25903caf86e785715887c1c31730c1ef\"},"
    "\"data_schema\":2,\"data_compat_floor\":1,\"link_protocol\":1,\"min_supported_launcher_link\":1,"
    "\"launcher_sha256\":\"1111111111111111111111111111111111111111111111111111111111111111\",\"app_sha256\":\"2222222222222222222222222222222222222222222222222222222222222222\","
    "\"verdict\":{\"kind\":\"proven\"}},{\"id\":{\"version\":\"1.6.0.0\",\"digest\":\"061df5b58d5e26897c282679738fcc6bde95341f6582cd66bcc2981e091d38bd\"},"
    "\"data_schema\":3,\"data_compat_floor\":2,\"link_protocol\":4,\"min_supported_launcher_link\":2,"
    "\"launcher_sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"app_sha256\":\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\","
    "\"verdict\":{\"kind\":\"unproven\"}},{\"id\":{\"version\":\"1.5.3.0\",\"digest\":\"1828835ac094a236a303c813865a4ddd05ca6e26bd9847ce8053c740f824a67f\"},"
    "\"data_schema\":2,\"data_compat_floor\":1,\"link_protocol\":2,\"min_supported_launcher_link\":1,"
    "\"launcher_sha256\":\"5555555555555555555555555555555555555555555555555555555555555555\",\"app_sha256\":\"6666666666666666666666666666666666666666666666666666666666666666\","
    "\"verdict\":{\"kind\":\"quarantined\",\"reason\":\"crashed_before_healthy\"}}],\"revoked\":{\"stable\":[],"
    "\"beta\":[\"1.5.9.0\"]},\"activation\":{\"kind\":\"probation\",\"candidate\":{\"version\":\"1.6.0.0\","
    "\"digest\":\"061df5b58d5e26897c282679738fcc6bde95341f6582cd66bcc2981e091d38bd\"},\"fallback\":{\"version\":\"1.5.2.0\","
    "\"digest\":\"03177f3e4426bb623e83c3c4901c891b0b576d0ef129564202717e27e004432d\"},\"pre_snapshot\":{\"id\":\"pre-1.6.0.0-g9\","
    "\"data\":{\"version\":2,\"compat_floor\":1}},\"failed_runs\":1,\"restore_on_rollback\":false},"
    "\"operation\":{\"kind\":\"running\",\"build\":{\"version\":\"1.6.0.0\",\"digest\":\"061df5b58d5e26897c282679738fcc6bde95341f6582cd66bcc2981e091d38bd\"},"
    "\"healthy\":true,\"healthy_uptime_s\":12}}";

const std::string kBootContractJson =
    "{\"format\":1,\"good\":{\"dir\":\"builds\\\\1.5.2.0-03177f3e\",\"sha256\":\"3333333333333333333333333333333333333333333333333333333333333333\"},"
    "\"previous_good\":{\"dir\":\"builds\\\\1.5.1.0-ab24664b\",\"sha256\":\"1111111111111111111111111111111111111111111111111111111111111111\"},"
    "\"last_good_app\":{\"dir\":\"builds\\\\1.5.2.0-03177f3e\",\"sha256\":\"4444444444444444444444444444444444444444444444444444444444444444\"},"
    "\"data_dir\":\"C:\\\\Users\\\\golden\\\\AppData\\\\Local\\\\Parties\\\\data\"}";

// The text as C++ string literals, one per line, ready to paste above.
std::string escaped(const std::string& text) {
    std::string out = "\"";
    size_t line_start = 0;
    for (char c : text) {
        if (c == '"' || c == '\\') out += '\\';
        out += c == '\n' ? std::string("\\n") : std::string(1, c);
        if (c == '\n' || (c == ',' && out.size() - line_start > 90)) {
            out += "\"\n    \"";
            line_start = out.size();
        }
    }
    return out + "\"";
}

bool fail(const std::string& msg) {
    std::cerr << "update_golden_test: " << msg << "\n";
    return false;
}

bool same(const std::string& what, const std::string& actual, const std::string& expected) {
    if (actual == expected) return true;
    return fail(what + " changed. Now:\n    " + escaped(actual));
}

Version v(const char* text) { return *Version::parse(text); }
Sha256 sha(char fill) { return *Sha256::from_hex(std::string(64, fill)); }

const std::string kReleases = "https://github.com/emcifuntik/parties/releases/download/";

BuildEntry entry(const char* version, uint32_t schema, uint32_t floor, uint32_t link, uint32_t min_link, char fill) {
    BuildEntry b;
    b.version = v(version);
    b.data_schema = schema;
    b.data_compat_floor = floor;
    b.link_protocol = link;
    b.min_supported_launcher_link = min_link;
    const std::string tag = kReleases + "v" + version + "/";
    b.files = {
        {std::string(kLauncherFile), 2'412'544, sha(fill), {tag + "PartiesLauncher.exe", "https://mirror.example/l"}},
        {std::string(kAppFile), 41'943'040, sha(char(fill + 1)), {tag + "parties_client.exe"}},
    };
    return b;
}

const BuildEntry k151 = entry("1.5.1.0", 2, 1, 1, 1, '1');
const BuildEntry k152 = entry("1.5.2.0", 2, 1, 2, 1, '3');
const BuildEntry k160 = entry("1.6.0.0", 3, 2, 4, 2, 'a');

ManifestPayload payload() {
    ManifestPayload p;
    p.channel = Channel::Beta;
    p.serial = 17;
    p.issued_at = sys_days{2026y / October / 31} + 12h;
    p.expires_at = p.issued_at + days{45};
    p.builds = {k160, k152};
    p.revoked = {v("1.5.9.0")};
    return p;
}

SupervisorState state() {
    SupervisorState s = initial_state(BuildRecord::from_entry(k152), Channel::Beta);
    s.generation = 9;
    BuildRecord previous = BuildRecord::from_entry(k151);
    previous.verdict = Proven{};
    s.previous_good = previous.id;
    s.builds.push_back(previous);
    s.builds.push_back(BuildRecord::from_entry(k160));
    BuildRecord failed = BuildRecord::from_entry(entry("1.5.3.0", 2, 1, 2, 1, '5'));
    failed.verdict = Quarantined{FailureReason::CrashedBeforeHealthy};
    s.builds.push_back(failed);
    s.intent = PinnedUntilAtLeast{v("1.6.0.0")};
    s.revoked.beta = {v("1.5.9.0")};
    s.activation = Probation{k160.id(), k152.id(), Snapshot{{"pre-1.6.0.0-g9"}, {2, 1}}, 1, false};
    s.operation = Running{k160.id(), true, 12};
    return s;
}

std::span<const uint8_t> bytes(const std::string& s) { return {reinterpret_cast<const uint8_t*>(s.data()), s.size()}; }

bool test_payload() {
    const std::string text = write_payload(payload());
    bool ok = same("the manifest payload", text, kPayload);
    SigningKey key{"golden"};
    key.secret_key.fill(0x2a);
    if (!parties::derive_pubkey(key.secret_key, key.public_key)) return false;
    const auto envelope = sign_manifest(bytes(text), std::vector{key});
    const PinnedKey pin{key.key_id, key.public_key};
    const auto verified = verify_manifest(bytes(*envelope), {&pin, 1}, Channel::Beta);
    if (!verified) return fail("the golden manifest is rejected: " + verified.error().detail);
    ok &= same("a verified payload written again", write_payload(verified->payload()), kPayload);
    for (const BuildEntry& b : payload().builds) {
        const auto parsed = parse_build_entry(write_build_entry(b));
        if (!parsed || *parsed != b) ok &= fail(b.version.to_string() + " changed in a write and parse");
    }
    return ok;
}

bool test_build_json() {
#if defined(_WIN32)
    return same("build.json", launcher::write_build_info(k160), kBuildJson);
#else
    return true;
#endif
}

bool test_supervisor_json() {
    const SupervisorState s = state();
    bool ok = same("supervisor.json", write_state(s), kSupervisorJson);
    ok &= same("boot-contract.json",
               boot::write_boot_contract(derive_boot_contract(s, "C:\\Users\\golden\\AppData\\Local\\Parties\\data")),
               kBootContractJson);
    return ok;
}

} // namespace

int main() {
    bool ok = true;
    ok &= test_payload();
    ok &= test_build_json();
    ok &= test_supervisor_json();
    if (!ok) return 1;
    std::printf("update_golden_test: ok\n");
    return 0;
}
