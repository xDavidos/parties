#include "testing.h"

#include "file_io.h"
#include "log.h"

#include <parties/update/release_keys.h>

#include <charconv>
#include <cstdlib>
#include <string_view>
#include <vector>

#include <windows.h>

namespace parties::update::launcher::testing {

#ifdef PARTIES_UPDATE_TESTING

namespace {

std::optional<std::string> env(const char* name) {
    char* value = nullptr;
    size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || !value) return std::nullopt;
    std::string out(value);
    std::free(value);
    if (out.empty()) return std::nullopt;
    return out;
}

std::optional<uint64_t> number(std::string_view text) {
    uint64_t value = 0;
    auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || end != text.data() + text.size()) return std::nullopt;
    return value;
}

// The harness's own key, generated for it. Never a production key.
const PinnedKey kKeys[] = {{
    "parties-e2e-test",
    {0x47, 0xac, 0x18, 0xc9, 0xb1, 0x2d, 0xc1, 0xbe, 0x44, 0x54, 0x33, 0x86, 0x8b, 0x82, 0xb5, 0x68,
     0x8e, 0x7a, 0x30, 0xbc, 0x47, 0x65, 0xe5, 0x14, 0x87, 0x66, 0x97, 0x79, 0x16, 0x31, 0x44, 0x1a},
}};

} // namespace

bool enabled() { return true; }

Timing timing() {
    Timing t;
    auto text = env("PARTIES_TEST_TIMING");
    if (!text) return t;
    std::vector<uint64_t> parts;
    std::string_view rest = *text;
    while (!rest.empty()) {
        size_t comma = rest.find(',');
        auto n = number(rest.substr(0, comma));
        if (!n) return t;
        parts.push_back(*n);
        rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
    }
    if (parts.size() != 4) return t;
    t.unit = std::chrono::milliseconds(parts[0]);
    t.policy.health_deadline_s = static_cast<uint32_t>(parts[1]);
    t.policy.confirm_uptime_s = static_cast<uint32_t>(parts[2]);
    t.policy.confirm_on_clean_exit_after_s = static_cast<uint32_t>(parts[3]);
    return t;
}

void before_effect() {
    static const std::optional<uint64_t> kill_at = [] {
        auto text = env("PARTIES_KILL_AT");
        return text ? number(*text) : std::nullopt;
    }();
    static const std::optional<std::string> counter_file = env("PARTIES_KILL_COUNTER");
    static uint64_t count = 0;
    if (!kill_at) return;
    if (counter_file) {
        if (auto text = read_file(*counter_file)) count = number(*text).value_or(0);
    }
    ++count;
    if (counter_file) {
        std::string ignored;
        durable_replace(*counter_file, std::to_string(count), ignored);
    }
    if (count == *kill_at) {
        log("PARTIES_KILL_AT={} reached, terminating", *kill_at);
        TerminateProcess(GetCurrentProcess(), kKilledAtEffect);
    }
}

std::optional<std::string> scripted_recovery() { return env("PARTIES_TEST_RECOVERY"); }

std::optional<std::chrono::milliseconds> exit_after_healthy() {
    auto text = env("PARTIES_TEST_EXIT_AFTER_HEALTHY_MS");
    auto n = text ? number(*text) : std::nullopt;
    if (!n) return std::nullopt;
    return std::chrono::milliseconds(*n);
}

std::optional<std::chrono::milliseconds> job_cap() {
    auto text = env("PARTIES_TEST_JOB_CAP_MS");
    auto n = text ? number(*text) : std::nullopt;
    if (!n) return std::nullopt;
    return std::chrono::milliseconds(*n);
}

Keyring pinned_keys() { return kKeys; }

#else

bool enabled() { return false; }
Timing timing() { return {}; }
void before_effect() {}
std::optional<std::string> scripted_recovery() { return std::nullopt; }
std::optional<std::chrono::milliseconds> exit_after_healthy() { return std::nullopt; }
std::optional<std::chrono::milliseconds> job_cap() { return std::nullopt; }
Keyring pinned_keys() { return production_keyring(); }

#endif

} // namespace parties::update::launcher::testing
