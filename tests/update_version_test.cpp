// Offline tests for the release version grammar and build directory names.

#include <parties/update/version.h>

#include <iostream>
#include <string>

using namespace parties::update;

namespace {

bool fail(const std::string& msg) {
    std::cerr << "update_version_test: " << msg << "\n";
    return false;
}

bool test_parse_round_trip() {
    for (const char* text : {"0.0.0.0", "1.6.0.0", "12.34.56.78", "4294967295.0.0.1"}) {
        auto v = Version::parse(text);
        if (!v) return fail(std::string("rejected valid version ") + text);
        if (v->to_string() != text) return fail(std::string("round trip changed ") + text);
    }
    auto v = Version::parse("1.2.3.4");
    if (v->parts != std::array<uint32_t, 4>{1, 2, 3, 4}) return fail("parts are not in order");
    return true;
}

bool test_parse_rejects_malformed() {
    const char* bad[] = {
        "", "1", "1.2", "1.2.3", "1.2.3.4.5", "1.2.3.", ".1.2.3", "1..2.3",
        "v1.2.3.4", "1.2.3.4 ", " 1.2.3.4", "1.2.3.-4", "1.2.3.+4", "1.2.3.a",
        "1.2.3.4-beta", "01.2.3.4", "1.2.3.00", "4294967296.0.0.0",
        "99999999999.0.0.0", "1,2,3,4",
    };
    for (const char* text : bad) {
        if (Version::parse(text)) return fail(std::string("accepted malformed version '") + text + "'");
    }
    return true;
}

bool test_ordering() {
    auto v = [](const char* t) { return *Version::parse(t); };
    if (!(v("1.6.0.0") < v("1.6.0.1"))) return fail("build part does not order");
    if (!(v("1.6.0.9") < v("1.6.1.0"))) return fail("patch outranks build");
    if (!(v("1.9.9.9") < v("2.0.0.0"))) return fail("major outranks the rest");
    if (!(v("1.10.0.0") > v("1.9.0.0"))) return fail("parts compare numerically, not as text");
    if (!(v("1.6.0.0") == v("1.6.0.0"))) return fail("equal versions differ");
    return true;
}

bool test_dir_name() {
    auto digest = Sha256::from_hex("ab12cd34ef5678900112233445566778899aabbccddeeff00112233445566778");
    if (!digest) return fail("valid digest rejected");
    BuildId id{*Version::parse("1.6.0.0"), *digest};
    if (id.dir_name() != "1.6.0.0-ab12cd34") return fail("dir name is " + id.dir_name());
    return true;
}

bool test_digest_hex_is_strict() {
    std::string good(64, 'a');
    if (!Sha256::from_hex(good)) return fail("lowercase hex rejected");
    if (Sha256::from_hex(std::string(64, 'A'))) return fail("uppercase hex accepted");
    if (Sha256::from_hex(std::string(63, 'a'))) return fail("short hex accepted");
    if (Sha256::from_hex(std::string(65, 'a'))) return fail("long hex accepted");
    if (Sha256::from_hex(std::string(62, 'a') + "g0")) return fail("non-hex accepted");
    if (Sha256::from_hex(good)->to_hex() != good) return fail("hex round trip");
    return true;
}

} // namespace

int main() {
    bool ok = true;
    ok &= test_parse_round_trip();
    ok &= test_parse_rejects_malformed();
    ok &= test_ordering();
    ok &= test_dir_name();
    ok &= test_digest_hex_is_strict();
    if (ok) std::cout << "update_version_test: all passed\n";
    return ok ? 0 : 1;
}
