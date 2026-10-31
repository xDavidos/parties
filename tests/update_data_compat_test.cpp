// Offline tests for the data compatibility rule shared by the app and launcher.

#include <parties/update/data_compat.h>

#include <iostream>

using namespace parties::update;

namespace {

bool fail(const char* msg) {
    std::cerr << "update_data_compat_test: " << msg << "\n";
    return false;
}

// The rule as design.md states it, written independently: a build with schema
// S opens {v, f} if S >= v or f <= S; it migrates only when S > v.
Compatibility expected(uint32_t s, DataInfo d) {
    bool opens = s >= d.version || d.compat_floor <= s;
    if (!opens) return Compatibility::Incompatible;
    return s > d.version ? Compatibility::MigratesForward : Compatibility::ReadsAsIs;
}

bool test_named_cases() {
    if (compatibility(2, DataInfo::legacy()) != Compatibility::MigratesForward)
        return fail("newer build must migrate a legacy database");
    if (compatibility(2, {2, 1}) != Compatibility::ReadsAsIs)
        return fail("same schema reads as is");
    if (compatibility(2, {3, 1}) != Compatibility::ReadsAsIs)
        return fail("additive newer schema must stay readable by an older build");
    if (compatibility(2, {3, 3}) != Compatibility::Incompatible)
        return fail("a raised floor must lock out older builds");
    if (compatibility(3, {3, 3}) != Compatibility::ReadsAsIs)
        return fail("the floor build itself reads as is");
    return true;
}

bool test_exhaustive_small_domain() {
    for (uint32_t s = 1; s <= 6; ++s)
        for (uint32_t v = 1; v <= 6; ++v)
            for (uint32_t f = 1; f <= v; ++f)
                if (compatibility(s, {v, f}) != expected(s, {v, f}))
                    return fail("rule disagrees with design.md on the small domain");
    return true;
}

static_assert(compatibility(1, {1, 1}) == Compatibility::ReadsAsIs);

} // namespace

int main() {
    bool ok = true;
    ok &= test_named_cases();
    ok &= test_exhaustive_small_domain();
    if (ok) std::cout << "update_data_compat_test: all passed\n";
    return ok ? 0 : 1;
}
