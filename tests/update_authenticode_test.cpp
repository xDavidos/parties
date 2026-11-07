#include "authenticode.h"

#include <cstdlib>
#include <iostream>
#include <string>

#include <windows.h>

using namespace parties::update::launcher;

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what) {
    if (ok) return;
    std::cerr << "update_authenticode_test: " << what << "\n";
    ++g_failures;
}

fs::path self() {
    wchar_t buffer[MAX_PATH];
    GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    return buffer;
}

std::optional<fs::path> signed_microsoft_binary() {
    for (const char* var : {"ProgramFiles(x86)", "ProgramFiles"}) {
        const char* root = std::getenv(var);
        if (!root) continue;
        fs::path edge = fs::path(root) / "Microsoft" / "Edge" / "Application" / "msedge.exe";
        if (fs::exists(edge)) return edge;
    }
    return std::nullopt;
}

} // namespace

int main() {
    auto unsigned_result = publisher_mismatch(self(), "Anyone");
    check(unsigned_result && unsigned_result->find("no valid Authenticode signature") != std::string::npos,
          "an unsigned executable is rejected");

    if (auto edge = signed_microsoft_binary()) {
        auto match = publisher_mismatch(*edge, "Microsoft Corporation");
        // 0x80092013: the revocation server could not be reached from this machine.
        if (match && match->find("0x80092013") != std::string::npos) {
            std::cout << "update_authenticode_test: revocation is unreachable, signed checks skipped\n";
        } else {
            check(!match, "a valid signature by the pinned publisher is accepted: " + match.value_or(""));
            auto other = publisher_mismatch(*edge, "Someone Else");
            check(other && other->find("not \"Someone Else\"") != std::string::npos,
                  "a valid signature by another publisher is rejected");
        }
    } else {
        std::cout << "update_authenticode_test: no signed Microsoft binary found, signed checks skipped\n";
    }

    if (g_failures == 0) std::cout << "update_authenticode_test: all passed\n";
    return g_failures ? 1 : 0;
}
